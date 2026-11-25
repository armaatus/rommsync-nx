#include "rommsync/backup_prune.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

#include "rommsync/disk.hpp"
#include "rommsync/config_text.hpp"
#include "rommsync/json.hpp"
#include "rommsync/md5.hpp"

namespace rommsync::backup {

namespace {

constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

/// `BackUpFirst` walks its uniquifier below this, so a longer trailing number is
/// a timestamp or part of a discriminator, never a uniquifier.
constexpr std::size_t kMaxUniquifierDigits = 3;

/// A timestamp this client writes has ten digits until 2286. Anything shorter
/// in front of a short trailing number is a discriminator's own digits.
constexpr std::size_t kMinTimestampDigits = 4;

bool AllDigits(std::string_view text) {
  return !text.empty() && text.size() <= 18 &&
         std::all_of(text.begin(), text.end(),
                     [](char character) { return character >= '0' && character <= '9'; });
}

std::int64_t ToInt(std::string_view digits) {
  std::int64_t value = 0;
  for (const char character : digits) value = value * 10 + (character - '0');
  return value;
}

/// How many roms' server digests one prune holds at once. A listing is up to
/// `http::kMaxResponseBytes` of rows, ~10 KiB of digests, and a card with
/// candidates for every rom it holds would otherwise keep all of them beside
/// the backups' own list. Past this, the oldest-asked rom is forgotten and
/// asked again if it comes up -- a request, never a wrong answer.
constexpr std::size_t kMaxCachedRoms = 8;

/// One backup, as the prune sees it.
struct Backup {
  std::string name;
  std::string sd_path;
  BackupName parsed;
  std::int64_t size = 0;
  /// Position in its group, newest first: 0 is the group's newest.
  std::size_t rank = 0;
  std::size_t group = 0;
  bool removed = false;
  /// Examined and kept as the only copy; not offered a second time.
  bool kept = false;
  /// The card refused to remove it once; never asked again this prune.
  bool unremovable = false;
  /// Position among its state's backups the state budget counts, newest first
  /// and from 1; 0 is one it does not count (#340).
  std::size_t state_rank = 0;
  std::optional<std::string> digest;  ///< computed once, empty string = unreadable
};

/// Oldest first; ties broken by name so the order is the directory's, not the
/// listing's.
bool Older(const Backup& left, const Backup& right) {
  return std::tie(left.parsed.unix_seconds, left.parsed.uniquifier, left.name) <
         std::tie(right.parsed.unix_seconds, right.parsed.uniquifier, right.name);
}

class Pruner {
 public:
  Pruner(fs::FileSystem& files, const PruneOptions& options, PruneReport* report)
      : files_(files), options_(options), report_(report) {}

  void Run() {
    const fs::Listing listing = files_.List(options_.backup_dir);
    if (!listing.ok() && listing.error != fs::ListError::kTooManyEntries) {
      // A missing directory is a card that never backed anything up.
      if (listing.error != fs::ListError::kMissing) Warn(listing.message);
      return;
    }
    report_->truncated = listing.error == fs::ListError::kTooManyEntries;
    Collect(listing);
    Rank();

    const Retention& keep = options_.retention;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                 (options_.now ? options_.now() : std::chrono::system_clock::now())
                                     .time_since_epoch())
                                 .count();
    const std::int64_t young_after =
        keep.keep_days > 0 ? now - static_cast<std::int64_t>(keep.keep_days) * kSecondsPerDay
                           : now + 1;
    const std::size_t keep_count = static_cast<std::size_t>(std::max(keep.keep, 1));

    std::vector<std::size_t> order(backups_.size());
    for (std::size_t at = 0; at < order.size(); ++at) order[at] = at;
    std::sort(order.begin(), order.end(),
              [this](std::size_t left, std::size_t right) {
                return Older(backups_[left], backups_[right]);
              });

    // By count and age: whatever is neither among the newest nor young.
    for (const std::size_t at : order) {
      const Backup& backup = backups_[at];
      if (backup.rank < keep_count || backup.parsed.unix_seconds > young_after) continue;
      Offer(at);
    }
    // By size: anything but a group's newest, oldest first, until under.
    if (keep.max_bytes > 0) {
      for (const std::size_t at : order) {
        if (total_ <= keep.max_bytes) break;
        const Backup& backup = backups_[at];
        if (backup.removed || backup.kept || backup.rank == 0) continue;
        Offer(at);
      }
    }
    // Last, and only over states: the one pass that removes without a proof.
    if (keep.max_state_bytes > 0 && options_.is_state_backup) Budget(order, keep);
    // Then what the budget cannot rank, by age: a previous server's (#346),
    // and a state this server's `state.db` dropped (#348).
    if (keep.orphan_days > 0) {
      Orphans(order, keep, now, true, options_.is_previous_state_backup);
      Orphans(order, keep, now, false, options_.is_dropped_state_backup);
    }
    for (const Backup& backup : backups_) {
      if (!backup.removed && backup.parsed.discriminator.rfind("state-", 0) == 0 &&
          !(!Previous(backup) && options_.is_state_backup &&
            options_.is_state_backup(backup.parsed, sync::ExtensionOf(backup.name)))) {
        ++report_->state_orphans;
      }
    }
    report_->over_cap = keep.max_bytes > 0 && total_ > keep.max_bytes;
    report_->bytes_after = total_;
  }

 private:
  void Collect(const fs::Listing& listing) {
    std::optional<std::int64_t> cut;
    if (report_->truncated) {
      // The listing keeps the lexicographically first entries, so the last rom
      // it reached may have backups that were not listed -- and a count over a
      // partial group would call its newest listed copy old. That whole rom
      // sits this prune out. The rom and not only the last group, because one
      // rom's groups interleave (`slot` and `slot-5` share a prefix) while two
      // roms' never do: a rom id is digits and then `-`, which sorts before
      // every digit.
      // The *set* is the first by name; its order is the backend's, so the
      // last name is looked for rather than read off the end.
      const std::string* last = nullptr;
      for (const fs::Entry& entry : listing.entries) {
        if (entry.is_directory || !ParseBackupName(entry.name)) continue;
        if (last == nullptr || entry.name > *last) last = &entry.name;
      }
      if (last != nullptr) cut = ParseBackupName(*last)->rom_id;
    }
    for (const fs::Entry& entry : listing.entries) {
      if (entry.is_directory) continue;
      std::optional<BackupName> parsed = ParseBackupName(entry.name);
      if (!parsed) continue;
      if (cut && *cut == parsed->rom_id) continue;
      Backup backup;
      backup.name = entry.name;
      backup.sd_path = options_.backup_dir + "/" + entry.name;
      backup.parsed = std::move(*parsed);
      backup.size = entry.size_bytes;
      total_ += backup.size;
      backups_.push_back(std::move(backup));
    }
    report_->backups = backups_.size();
    report_->bytes_before = total_;
  }

  void Rank() {
    std::map<std::pair<std::int64_t, std::string>, std::vector<std::size_t>> groups;
    for (std::size_t at = 0; at < backups_.size(); ++at) {
      groups[{backups_[at].parsed.rom_id, backups_[at].parsed.discriminator}].push_back(at);
    }
    std::size_t group = 0;
    for (auto& [key, members] : groups) {
      std::sort(members.begin(), members.end(), [this](std::size_t left, std::size_t right) {
        return Older(backups_[right], backups_[left]);
      });
      for (std::size_t rank = 0; rank < members.size(); ++rank) {
        backups_[members[rank]].rank = rank;
        backups_[members[rank]].group = group;
      }
      members_.push_back(members);
      ++group;
    }
  }

  /// The state budget (#340): while the state backups `is_state_backup` names
  /// hold more than `max_state_bytes`, the oldest of them beyond the newest
  /// `keep_states` of their state go -- by proof when there is one, and
  /// without one when there is not. See the header.
  void Budget(const std::vector<std::size_t>& order, const Retention& keep) {
    const std::size_t keep_states = static_cast<std::size_t>(std::max(keep.keep_states, 1));
    std::int64_t state_total = RankStates(false, options_.is_state_backup);
    for (const std::size_t at : order) {
      if (state_total <= keep.max_state_bytes) break;
      Backup& backup = backups_[at];
      if (backup.removed || backup.unremovable || backup.state_rank <= keep_states) continue;
      const Taken taken = Take(at);
      if (taken == Taken::kNot) continue;
      if (taken == Taken::kUnproven) ++report_->budget_pruned;
      state_total -= backup.size;
    }
    report_->state_bytes_after = state_total;
  }

  /// The orphan bound: the state backups `named` names on one side of
  /// `server_since` -- a previous server's by a set-aside `state.db` (#346),
  /// or this one's by a state it dropped (#348) -- older than `orphan_days`, go
  /// oldest first beyond the newest `keep_states` of their state -- by proof
  /// when there is one, without one when there is not. See the header.
  void Orphans(const std::vector<std::size_t>& order, const Retention& keep, std::int64_t now,
               bool previous, const IsStateBackup& named) {
    if (!named) return;
    const std::size_t keep_states = static_cast<std::size_t>(std::max(keep.keep_states, 1));
    RankStates(previous, named);
    const std::int64_t old_before = now - static_cast<std::int64_t>(keep.orphan_days) * kSecondsPerDay;
    for (const std::size_t at : order) {
      const Backup& backup = backups_[at];
      // `named` again: this side's ranks include the budget's.
      if (backup.removed || backup.unremovable || Previous(backup) != previous ||
          backup.state_rank <= keep_states || backup.parsed.unix_seconds >= old_before ||
          !named(backup.parsed, sync::ExtensionOf(backup.name))) {
        continue;
      }
      if (Take(at) == Taken::kUnproven) ++report_->orphan_pruned;
    }
  }

  /// Named before `server_since`: another server's, or one nothing names (#294).
  bool Previous(const Backup& backup) const {
    return backup.parsed.unix_seconds < options_.server_since;
  }

  /// Rank what `named` counts -- this server's backups, or with `previous`
  /// those from before it -- newest first from 1, and return their bytes.
  ///
  /// Counted over what is still on the card and what `named` counts: a save
  /// backup that happens to share a group, or one of the other side of
  /// `server_since`, must not stand in for one of a state's newest. And per
  /// extension, not per group: RetroArch's slots of one game, `.state1` and
  /// `.state2`, share the discriminator `state-Game` and are two states.
  std::int64_t RankStates(bool previous, const IsStateBackup& named) {
    std::int64_t bytes = 0;
    for (const std::vector<std::size_t>& members : members_) {
      for (std::size_t i = 0; i < members.size(); ++i) {
        Backup& backup = backups_[members[i]];
        const std::string_view extension = sync::ExtensionOf(backup.name);
        if (backup.removed || Previous(backup) != previous ||
            !named(backup.parsed, extension)) {
          continue;
        }
        backup.state_rank = 1;
        for (std::size_t newer = 0; newer < i; ++newer) {
          const Backup& other = backups_[members[newer]];
          // The budget's ranks stay set; the other side of `server_since`
          // must not count towards this side's newest.
          if (other.state_rank > 0 && Previous(other) == previous &&
              sync::ExtensionOf(other.name) == extension) {
            ++backup.state_rank;
          }
        }
        bytes += backup.size;
      }
    }
    return bytes;
  }

  enum class Taken { kNot, kProven, kUnproven };

  /// Remove `at` for the budget or the orphan bound: as a proven copy when
  /// `Offer` finds one elsewhere, and otherwise without one. Only what is left
  /// -- which `Offer` has counted as kept -- is theirs to answer for.
  Taken Take(std::size_t at) {
    Backup& backup = backups_[at];
    if (!backup.kept) Offer(at);
    if (backup.unremovable) return Taken::kNot;
    if (backup.removed) return Taken::kProven;
    if (!Remove(at)) return Taken::kNot;
    --report_->kept_unique;
    return Taken::kUnproven;
  }

  /// Remove `at` if its bytes are provably somewhere else; otherwise keep it.
  void Offer(std::size_t at) {
    Backup& backup = backups_[at];
    const std::string& digest = DigestOf(at);
    // A backup from before this server is another server's rom id (#294): the
    // digests this one holds under it prove nothing about it.
    const bool this_server = backup.parsed.unix_seconds >= options_.server_since;
    if (digest.empty() ||
        !(InNewerCopy(at, digest) || (this_server && OnServer(backup.parsed.rom_id, digest)))) {
      backup.kept = true;
      ++report_->kept_unique;
      return;
    }
    if (!Remove(at)) ++report_->kept_unique;
  }

  /// One `remove()`, accounted. False, with the backup marked kept and a
  /// warning, when the card would not.
  bool Remove(std::size_t at) {
    Backup& backup = backups_[at];
    const std::string resolved = files_.Resolve(backup.sd_path);
    if (resolved.empty() || io::Remove(resolved) != 0) {
      backup.kept = true;
      backup.unremovable = true;
      Warn(backup.sd_path + " could not be removed");
      return false;
    }
    backup.removed = true;
    backup.kept = false;
    total_ -= backup.size;
    ++report_->pruned;
    return true;
  }

  const std::string& DigestOf(std::size_t at) {
    Backup& backup = backups_[at];
    if (!backup.digest) {
      const std::string resolved = files_.Resolve(backup.sd_path);
      backup.digest = resolved.empty() ? std::string() : crypto::Md5FileHex(resolved);
      if (backup.digest->empty()) Warn(backup.sd_path + " could not be read to compare");
    }
    return *backup.digest;
  }

  /// A newer backup of the same group, still on the card, with these bytes.
  ///
  /// Removal runs oldest first, so the newest of a run of identical copies is
  /// examined last and has no newer twin of its own: it goes only on the
  /// server's word, which then covers every copy that leaned on it.
  bool InNewerCopy(std::size_t at, const std::string& digest) {
    const Backup& backup = backups_[at];
    for (const std::size_t other : members_[backup.group]) {
      const Backup& newer = backups_[other];
      if (newer.rank >= backup.rank) break;
      if (newer.removed || newer.size != backup.size) continue;
      if (DigestOf(other) == digest) return true;
    }
    return false;
  }

  bool OnServer(std::int64_t rom_id, const std::string& digest) {
    if (!options_.server_digests) return false;
    auto found = server_.find(rom_id);
    if (found == server_.end()) {
      if (server_.size() >= kMaxCachedRoms) {
        server_.erase(asked_.front());
        asked_.erase(asked_.begin());
      }
      asked_.push_back(rom_id);
      std::optional<std::vector<std::string>> listed = options_.server_digests(rom_id);
      if (listed) {
        for (std::string& each : *listed) each = config::text::LowerAscii(each);
      }
      found = server_.emplace(rom_id, std::move(listed)).first;
    }
    return found->second &&
           std::find(found->second->begin(), found->second->end(), digest) != found->second->end();
  }

  void Warn(std::string line) {
    if (report_->warnings.size() < kMaxPruneWarnings) report_->warnings.push_back(std::move(line));
  }

  fs::FileSystem& files_;
  const PruneOptions& options_;
  PruneReport* report_;
  std::vector<Backup> backups_;
  std::vector<std::vector<std::size_t>> members_;  ///< per group, newest first
  std::map<std::int64_t, std::optional<std::vector<std::string>>> server_;
  std::vector<std::int64_t> asked_;  ///< `server_`'s keys, oldest first
  std::int64_t total_ = 0;
};

}  // namespace

std::optional<BackupName> ParseBackupName(std::string_view name) {
  std::string_view base = name.substr(0, name.size() - sync::ExtensionOf(name).size());
  std::vector<std::string_view> parts;
  for (std::size_t start = 0;;) {
    const std::size_t dash = base.find('-', start);
    parts.push_back(base.substr(start, dash == std::string_view::npos ? base.npos : dash - start));
    if (dash == std::string_view::npos) break;
    start = dash + 1;
  }
  // rom, at least one discriminator part, and a timestamp.
  if (parts.size() < 3 || !AllDigits(parts.front())) return std::nullopt;
  BackupName parsed;
  parsed.rom_id = ToInt(parts.front());
  std::size_t end = parts.size();
  if (parts.size() >= 4 && AllDigits(parts[end - 1]) && parts[end - 1].size() <= kMaxUniquifierDigits &&
      AllDigits(parts[end - 2]) && parts[end - 2].size() >= kMinTimestampDigits) {
    parsed.uniquifier = static_cast<int>(ToInt(parts[end - 1]));
    --end;
  }
  if (!AllDigits(parts[end - 1])) return std::nullopt;
  parsed.unix_seconds = ToInt(parts[end - 1]);
  --end;
  for (std::size_t at = 1; at < end; ++at) {
    if (at > 1) parsed.discriminator += '-';
    parsed.discriminator += parts[at];
  }
  if (parsed.discriminator.empty() || parsed.rom_id <= 0) return std::nullopt;
  return parsed;
}

Retention RetentionFor(const config::BackupConfig& backup) {
  Retention retention;
  retention.keep = backup.keep;
  retention.keep_days = backup.keep_days;
  retention.max_bytes = std::int64_t{backup.max_mib} * 1024 * 1024;
  retention.max_state_bytes = std::int64_t{backup.max_state_mib} * 1024 * 1024;
  retention.keep_states = backup.keep_states;
  retention.orphan_days = backup.orphan_days;
  return retention;
}

namespace {

/// `<rom>/<discriminator>/<extension>`: one string per group, so the lookup is
/// a sorted vector's rather than a set of tuples' -- the sysmodule's image is
/// budgeted by the page (scripts/image-budget.sh). A discriminator is
/// sanitised and an extension is part of a single file name, so neither holds
/// a `/`.
std::string GroupKey(std::int64_t rom_id, std::string_view discriminator,
                     std::string_view extension) {
  std::string key = std::to_string(rom_id);
  key += '/';
  key += discriminator;
  key += '/';
  key += extension;
  return key;
}

/// The group a state download or restore of `file_name` names its backups in.
std::string StateGroup(std::int64_t rom_id, const std::string& file_name) {
  return GroupKey(rom_id, sync::StateBackupDiscriminator(file_name), sync::ExtensionOf(file_name));
}

}  // namespace

IsStateBackup StateBackupsOf(const state::Baseline& baseline) {
  // What a state download (`state_sync.cpp`) and a state restore
  // (`conflict_record.cpp`) name a backup: the row's rom, the discriminator of
  // its name, and its name's extension.
  std::vector<std::string> states;
  for (const auto& [key, row] : baseline.state_rows()) {
    states.push_back(StateGroup(row.rom_id, row.file_name));
  }
  if (states.empty()) return nullptr;
  // A save's slot is whatever some client chose, so one can be spelled
  // `state-Game` (sync_execute.hpp). A group a save row of the same rom could
  // also have written is not known to be a state's, and stays out.
  std::vector<std::string> saves;
  for (const auto& [key, row] : baseline.rows()) {
    saves.push_back(GroupKey(row.rom_id, sync::SlotBackupDiscriminator(row.slot), {}));
  }
  std::sort(states.begin(), states.end());
  std::sort(saves.begin(), saves.end());
  return [states = std::move(states), saves = std::move(saves)](const BackupName& name,
                                                                std::string_view extension) {
    return !std::binary_search(saves.begin(), saves.end(),
                               GroupKey(name.rom_id, name.discriminator, {})) &&
           std::binary_search(states.begin(), states.end(),
                              GroupKey(name.rom_id, name.discriminator, extension));
  };
}

IsStateBackup SetAsideStateBackupsOf(fs::FileSystem& files, const std::string& state_sd_path,
                                     std::int64_t server_since, const state::Baseline& current) {
  if (server_since <= 0) return nullptr;
  const std::size_t slash = state_sd_path.rfind('/');
  const std::string dir = state_sd_path.substr(0, slash);
  const std::string prefix = state_sd_path.substr(slash + 1) + ".";
  // One per set-aside file: from when it was bound, the states it names. A
  // file naming none still ends the window of the one before it.
  struct Era {
    std::int64_t since = 0;
    std::vector<std::string> states;
    std::vector<std::string> kept;  ///< of `states`, those no other file names
  };
  std::vector<Era> eras;
  std::vector<std::string> saves;
  // A save row of any server on the card shadows a group: rom ids are reused
  // between servers, and a slot can be spelled `state-Game`.
  const auto add_saves = [&saves](const state::Baseline& baseline) {
    for (const auto& [key, row] : baseline.rows()) {
      saves.push_back(GroupKey(row.rom_id, sync::SlotBackupDiscriminator(row.slot), {}));
    }
  };
  add_saves(current);
  bool any = false;
  for (const fs::Entry& entry : files.List(dir).entries) {
    if (entry.is_directory || entry.name.rfind(prefix, 0) != 0) continue;
    // An interrupted commit's `.tmp` and `.old` are this server's file, not a
    // server move's (`binding::Archive` never writes either).
    const std::string_view tail = std::string_view(entry.name).substr(entry.name.size() - 4);
    if (tail == ".tmp" || tail == ".old") continue;
    const state::LoadedBaseline loaded = state::LoadBaseline(files.Resolve(dir + "/" + entry.name));
    // A set-aside file that will not read hides its save rows and its window:
    // nothing is then known well enough to remove.
    if (loaded.version == 0) return nullptr;
    const state::Baseline& set_aside = loaded.value;
    if (set_aside.bound_since() >= server_since) continue;
    add_saves(set_aside);
    Era era;
    era.since = set_aside.bound_since();
    for (const auto& [key, row] : set_aside.state_rows()) {
      era.states.push_back(StateGroup(row.rom_id, row.file_name));
    }
    // And the states it had dropped (#348): that server's too, in its window.
    for (const state::DroppedState& dropped : set_aside.dropped_states()) {
      era.states.push_back(StateGroup(dropped.rom_id, dropped.file_name));
    }
    std::sort(era.states.begin(), era.states.end());
    any = any || !era.states.empty();
    eras.push_back(std::move(era));
  }
  if (!any) return nullptr;
  // A state two set-aside files both name is two servers' (rom ids are
  // reused), and `keep_states` ranks a state across every file's window: a
  // newer server's backups would push an older one's newest out. Neither is
  // named, so each named state's backups are one server's.
  for (Era& era : eras) {
    std::vector<std::string> own;
    for (const std::string& key : era.states) {
      bool shared = false;
      for (const Era& other : eras) {
        shared = shared || (&other != &era &&
                            std::binary_search(other.states.begin(), other.states.end(), key));
      }
      if (!shared) own.push_back(key);
    }
    era.kept = std::move(own);
  }
  for (Era& era : eras) era.states = std::move(era.kept);
  std::sort(saves.begin(), saves.end());
  return [eras = std::move(eras), saves = std::move(saves), server_since](
             const BackupName& name, std::string_view extension) {
    if (name.unix_seconds >= server_since ||
        std::binary_search(saves.begin(), saves.end(),
                           GroupKey(name.rom_id, name.discriminator, {}))) {
      return false;
    }
    // The file bound last before the backup was taken. Unsorted, and scanned
    // whole: there are a handful, and a sort costs the image a page.
    const Era* era = nullptr;
    for (const Era& each : eras) {
      if (each.since <= name.unix_seconds && (era == nullptr || each.since > era->since)) {
        era = &each;
      }
    }
    return era != nullptr &&
           std::binary_search(era->states.begin(), era->states.end(),
                              GroupKey(name.rom_id, name.discriminator, extension));
  };
}

IsStateBackup DroppedStateBackupsOf(const state::Baseline& baseline) {
  if (baseline.dropped_states().empty()) return nullptr;
  // A group a row holds is not the dropped state's alone: a save's slot can be
  // spelled `state-Game`, and a live state's backups are the budget's to rank.
  std::vector<std::string> rows;
  for (const auto& [key, row] : baseline.rows()) {
    rows.push_back(GroupKey(row.rom_id, sync::SlotBackupDiscriminator(row.slot), {}));
  }
  for (const auto& [key, row] : baseline.state_rows()) {
    rows.push_back(StateGroup(row.rom_id, row.file_name));
  }
  std::sort(rows.begin(), rows.end());
  // The state's group, beside the second it went.
  std::vector<std::string> groups;
  std::vector<std::int64_t> when;
  for (const state::DroppedState& each : baseline.dropped_states()) {
    groups.push_back(StateGroup(each.rom_id, each.file_name));
    when.push_back(each.dropped);
  }
  return [rows = std::move(rows), groups = std::move(groups), when = std::move(when)](
             const BackupName& name, std::string_view extension) {
    const std::string group = GroupKey(name.rom_id, name.discriminator, extension);
    if (std::binary_search(rows.begin(), rows.end(), group) ||
        std::binary_search(rows.begin(), rows.end(),
                           GroupKey(name.rom_id, name.discriminator, {}))) {
      return false;
    }
    // Up to the second it went: a backup after it is not known to be its.
    for (std::size_t at = 0; at < groups.size(); ++at) {
      if (groups[at] == group && name.unix_seconds <= when[at]) return true;
    }
    return false;
  };
}

PruneReport Prune(fs::FileSystem& files, const PruneOptions& options) {
  PruneReport report;
  Pruner(files, options, &report).Run();
  return report;
}

std::string Describe(const PruneReport& report) {
  std::string line = std::to_string(report.pruned) + " of " + std::to_string(report.backups) +
                     " backups pruned, " + std::to_string(report.bytes_before - report.bytes_after) +
                     " bytes freed; " + std::to_string(report.bytes_after) + " bytes in " +
                     std::to_string(report.backups - report.pruned) + " left";
  if (report.kept_unique > 0) {
    line += "; " + std::to_string(report.kept_unique) +
            " kept past the retention rule because no other copy of their bytes could be found";
  }
  if (report.budget_pruned > 0) {
    line += "; " + std::to_string(report.budget_pruned) +
            " of them save-state backups [backup] max_state_mib removed with no other copy known";
  }
  if (report.orphan_pruned > 0) {
    line += "; " + std::to_string(report.orphan_pruned) +
            " of them save-state backups of a previous server or a dropped state [backup] "
            "orphan_days removed with no other copy known";
  }
  if (report.state_orphans > 0) {
    line += "; " + std::to_string(report.state_orphans) +
            " save-state backups left that max_state_mib cannot count";
  }
  if (report.over_cap) line += "; still over the size cap";
  if (report.truncated) line += "; the directory holds more than one listing shows";
  return line;
}

namespace {

/// `GET <path>?rom_id=<rom_id>`'s body, retried with backoff on a transient
/// failure; nullopt for anything that is not a clean 2xx of at most `max_bytes`.
/// The listings the prune proves a backup with, which an unread one never does.
std::optional<std::string> GetRomListing(http::HttpClient& client, const auth::StoredToken& token,
                                         const char* path, std::int64_t rom_id,
                                         std::size_t max_bytes, const sync::CallPolicy& policy) {
  http::Request request;
  request.method = http::Method::kGet;
  request.url = http::JoinUrl(token.server_url, path) + "?rom_id=" + std::to_string(rom_id);
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.timeout = policy.timeout;
  request.cancel = policy.cancel;

  const int attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
  retry::Backoff backoff = sync::BackoffFor(policy);
  for (int attempt = 1;; ++attempt) {
    if (policy.cancel != nullptr && policy.cancel->canceled()) return std::nullopt;
    http::Result result = client.Send(request);
    if (result.successful()) {
      if (result.response.body.size() > max_bytes) return std::nullopt;
      return std::move(result.response.body);
    }
    const bool transient = result.ok() ? http::TransientStatus(result.response.status)
                                       : result.error != http::Error::kCanceled;
    if (!transient || attempt >= attempts) return std::nullopt;
    sync::WaitToRetry(policy, &backoff);
  }
}

}  // namespace

std::optional<std::vector<std::string>> ListServerSaveDigests(http::HttpClient& client,
                                                              const auth::StoredToken& token,
                                                              std::int64_t rom_id,
                                                              const sync::CallPolicy& policy) {
  const std::optional<std::string> body =
      GetRomListing(client, token, "/api/saves", rom_id, http::kMaxResponseBytes, policy);
  if (!body) return std::nullopt;
  std::vector<std::string> digests;
  static constexpr std::string_view kKeep[] = {"content_hash"};
  const json::ParseResult parsed = json::ParseEach(
      *body, "", kKeep,
      [&digests](const json::Value& row, std::size_t) {
        const json::Value* hash = row.Find("content_hash");
        if (hash != nullptr && hash->is_string() && sync::IsContentHash(hash->string())) {
          digests.push_back(config::text::LowerAscii(hash->string()));
        }
        return json::Error{};
      },
      "the save listing");
  if (!parsed.ok()) return std::nullopt;
  return digests;
}

state::Baseline ProvenStates(const state::Baseline& baseline,
                             const sync::StateSyncReport& report) {
  state::Baseline proven;
  if (!report.ran) return proven;
  for (const sync::StateOperationResult& operation : report.operations) {
    if (operation.outcome != sync::StateOutcome::kNoOp &&
        operation.outcome != sync::StateOutcome::kDownloaded) {
      continue;
    }
    if (const state::StateRecord* row =
            baseline.FindState(operation.rom_id, operation.file_name)) {
      proven.SetState(*row);
    }
  }
  return proven;
}

std::vector<std::string> StateDigestsOnServer(const state::Baseline& baseline, std::int64_t rom_id,
                                              const std::vector<sync::ServerState>& listed) {
  std::vector<std::string> digests;
  for (const auto& [key, row] : baseline.state_rows()) {
    if (key.first != rom_id || !sync::IsContentHash(row.content_hash) ||
        row.file_size_bytes != row.server_file_size_bytes) {
      continue;
    }
    for (const sync::ServerState& server : listed) {
      if (server.rom_id == rom_id && sync::ServerUnmoved(row, server)) {
        digests.push_back(config::text::LowerAscii(row.content_hash));
        break;
      }
    }
  }
  return digests;
}

std::optional<std::vector<std::string>> ListServerStateDigests(http::HttpClient& client,
                                                               const auth::StoredToken& token,
                                                               std::int64_t rom_id,
                                                               const state::Baseline& baseline,
                                                               const sync::CallPolicy& policy) {
  const bool has_state_rows =
      std::any_of(baseline.state_rows().begin(), baseline.state_rows().end(),
                  [rom_id](const auto& row) { return row.first.first == rom_id; });
  if (!has_state_rows) return std::vector<std::string>{};
  const std::optional<std::string> body =
      GetRomListing(client, token, "/api/states", rom_id, sync::kMaxStateListBytes, policy);
  if (!body) return std::nullopt;
  // Strict, as the states half reads it: a row it cannot read is no listing,
  // never a listing missing a row.
  const auth::Parsed<std::vector<sync::ServerState>> listed = sync::ParseStateList(*body);
  if (!listed.ok()) return std::nullopt;
  return StateDigestsOnServer(baseline, rom_id, listed.value);
}

std::optional<std::vector<std::string>> ListProvenServerDigests(http::HttpClient& client,
                                                                const auth::StoredToken& token,
                                                                std::int64_t rom_id,
                                                                const state::Baseline* proven_states,
                                                                const sync::CallPolicy& policy) {
  std::optional<std::vector<std::string>> digests =
      ListServerSaveDigests(client, token, rom_id, policy);
  if (proven_states == nullptr) return digests;
  std::optional<std::vector<std::string>> of_states =
      ListServerStateDigests(client, token, rom_id, *proven_states, policy);
  if (!of_states) return digests;
  if (!digests) return of_states;
  digests->insert(digests->end(), of_states->begin(), of_states->end());
  return digests;
}

}  // namespace rommsync::backup

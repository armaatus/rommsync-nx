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
      report_->over_cap = total_ > keep.max_bytes;
    }
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

  /// Remove `at` if its bytes are provably somewhere else; otherwise keep it.
  void Offer(std::size_t at) {
    Backup& backup = backups_[at];
    const std::string& digest = DigestOf(at);
    if (digest.empty() || !(InNewerCopy(at, digest) || OnServer(backup.parsed.rom_id, digest))) {
      backup.kept = true;
      ++report_->kept_unique;
      return;
    }
    const std::string resolved = files_.Resolve(backup.sd_path);
    if (resolved.empty() || io::Remove(resolved) != 0) {
      backup.kept = true;
      ++report_->kept_unique;
      Warn(backup.sd_path + " could not be removed");
      return;
    }
    backup.removed = true;
    total_ -= backup.size;
    ++report_->pruned;
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
  return retention;
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
  if (report.over_cap) line += "; still over the size cap";
  if (report.truncated) line += "; the directory holds more than one listing shows";
  return line;
}

std::optional<std::vector<std::string>> ListServerSaveDigests(http::HttpClient& client,
                                                              const auth::StoredToken& token,
                                                              std::int64_t rom_id,
                                                              const sync::CallPolicy& policy) {
  http::Request request;
  request.method = http::Method::kGet;
  request.url = http::JoinUrl(token.server_url, "/api/saves") + "?rom_id=" + std::to_string(rom_id);
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.timeout = policy.timeout;
  request.cancel = policy.cancel;

  const int attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
  retry::Backoff backoff = sync::BackoffFor(policy);
  for (int attempt = 1;; ++attempt) {
    if (policy.cancel != nullptr && policy.cancel->canceled()) return std::nullopt;
    const http::Result result = client.Send(request);
    if (result.successful()) {
      if (result.response.body.size() > http::kMaxResponseBytes) return std::nullopt;
      std::vector<std::string> digests;
      static constexpr std::string_view kKeep[] = {"content_hash"};
      const json::ParseResult parsed = json::ParseEach(
          result.response.body, "", kKeep,
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
    const bool transient = result.ok() ? http::TransientStatus(result.response.status)
                                       : result.error != http::Error::kCanceled;
    if (!transient || attempt >= attempts) return std::nullopt;
    sync::WaitToRetry(policy, &backoff);
  }
}

}  // namespace rommsync::backup

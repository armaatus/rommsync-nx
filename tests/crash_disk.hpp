// A power cut at every point of an overwrite: the `io::Disk` decorator M9-14
// (#203) exists for.
//
// Hard rule 2 says a save is backed up *before* it is overwritten. The suite
// proved that as an order of **calls** -- `BackUpFirst` returns before
// `CommitStaged` starts. The guarantee a player needs is an order of
// **persistence**: after a power cut at any instant, the save is its old bytes
// or its new ones, and if it is the new ones, a backup of the old ones is on the
// card. Neither FAT32 nor exFAT journals, so the difference is real.
//
// `Recorder` forwards every call to the real disk -- the engine runs normally on
// real files -- and logs each one that changed something under a sandbox root:
// opens, writes (with their offset), renames, removes, and the `Sync` that is
// the only thing making any of them durable. `Replay` then rebuilds the card
// from a snapshot taken when the recorder was installed, cuts the log at every
// point, and for each point enumerates what a non-journalling card may have
// kept:
//
//   - **metadata**: per directory, any *prefix* of the names created, renamed
//     or removed in it since its last successful `Sync`. A directory's own
//     updates are taken to land in order; two directories are independent,
//     which is what makes a backup in `.backup/` and an overwrite in the save
//     folder two different races.
//   - **data**: per file written since its last successful `Sync`, one of
//     *lost* (nothing since the sync landed -- in cache, never persisted),
//     *truncated* (a "w" open's emptying landed and no byte after it),
//     *torn* (the first half of those bytes), *short* (all but the second half
//     of the last write), *out of order* (the last write landed and none
//     before it), or *all*.
//
// Each distinct card is written to a scratch directory, the engine's own boot
// sweep (`sync::RecoverStaging`) runs over it, and a judge reads what is left.
//
// **What this model does not cover, so nobody reads more into a green run.**
// A *lying* sync -- one that returns true and persisted nothing -- defeats any
// protocol built on renames and is not modelled. Neither are flying writes
// (bytes landing in a file nothing touched; docs/SYNC_PROTOCOL.md says what
// `state.db` does about them), metadata reordered *within* a directory, a
// rename across directories (refused loudly: no save path makes one), or
// `fseek`/`ftruncate`, which no save-overwrite path reaches. A kernel-level
// injector (lazyfs) is the check on these assumptions; it needs Linux and FUSE,
// and is not part of this suite.
#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "checks.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/sync_tick.hpp"

namespace crash {

/// Paths are relative to the sandbox root, `/`-separated, with no leading
/// slash: an SD path minus its first character.
using Tree = std::map<std::string, std::string>;

struct Snapshot {
  std::set<std::string> dirs;  ///< every directory, the root as ""
  Tree files;
};

inline std::string RelativeOf(const std::string& sd_path) {
  return !sd_path.empty() && sd_path.front() == '/' ? sd_path.substr(1) : sd_path;
}

inline std::string DirOf(const std::string& relative) {
  const std::string::size_type slash = relative.rfind('/');
  return slash == std::string::npos ? std::string() : relative.substr(0, slash);
}

inline std::string LeafOf(const std::string& relative) {
  const std::string::size_type slash = relative.rfind('/');
  return slash == std::string::npos ? relative : relative.substr(slash + 1);
}

inline Snapshot Read(const std::filesystem::path& root) {
  Snapshot snapshot;
  snapshot.dirs.insert("");
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(root, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    const std::string relative = it->path().lexically_relative(root).generic_string();
    if (it->is_directory(error)) {
      snapshot.dirs.insert(relative);
    } else if (it->is_regular_file(error)) {
      std::ifstream in(it->path(), std::ios::binary);
      snapshot.files[relative] =
          std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
  }
  return snapshot;
}

/// One call that changed something under the root.
struct Op {
  enum class Kind { kOpen, kWrite, kRename, kRemove, kSync };
  Kind kind = Kind::kOpen;
  std::string path;
  std::string to;             ///< kRename
  bool truncate = false;      ///< kOpen: a "w" mode, which creates or empties
  int handle = 0;             ///< kOpen, kWrite
  std::uint64_t offset = 0;   ///< kWrite
  std::string bytes;          ///< kWrite
};

inline std::string Describe(const Op& op) {
  switch (op.kind) {
    case Op::Kind::kOpen:
      return std::string(op.truncate ? "create " : "open ") + op.path;
    case Op::Kind::kWrite:
      return "write " + std::to_string(op.bytes.size()) + " bytes at " +
             std::to_string(op.offset) + " (handle " + std::to_string(op.handle) + ")";
    case Op::Kind::kRename:
      return "rename " + op.path + " -> " + op.to;
    case Op::Kind::kRemove:
      return "remove " + op.path;
    case Op::Kind::kSync:
      return "sync " + (op.path.empty() ? std::string("/") : op.path);
  }
  return "?";
}

/// Records every change under `root`, and refuses the syncs it is told to.
///
/// Installed for its scope, like `faulty::Disk`. The snapshot is taken in the
/// constructor, so seed the card first.
class Recorder final : public rommsync::io::Disk {
 public:
  /// `refuse_sync`: SD paths whose `Sync` answers false with `EIO` -- a card
  /// that would not confirm a name, which is the promise this replay asks what
  /// happens without.
  explicit Recorder(const std::filesystem::path& root, std::set<std::string> refuse_sync = {})
      : root_(root.lexically_normal().generic_string()),
        snapshot_(Read(root)),
        refuse_sync_(std::move(refuse_sync)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    previous_ = rommsync::io::SetDisk(this);
  }

  ~Recorder() override { rommsync::io::SetDisk(previous_); }

  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  std::FILE* Open(const std::string& path, const char* mode) override {
    std::lock_guard<std::mutex> hold(mutex_);
    std::FILE* file = Inner().Open(path, mode);
    handles_.erase(file);
    std::string relative;
    if (file != nullptr && Under(path, &relative)) {
      Op op;
      op.kind = Op::Kind::kOpen;
      op.path = relative;
      op.truncate = mode != nullptr && mode[0] == 'w';
      op.handle = ++next_handle_;
      handles_[file] = op.handle;
      log_.push_back(std::move(op));
    }
    return file;
  }

  std::size_t Write(std::FILE* file, const void* data, std::size_t size) override {
    std::lock_guard<std::mutex> hold(mutex_);
    const auto known = handles_.find(file);
    const long offset = known != handles_.end() ? std::ftell(file) : 0;
    const std::size_t wrote = Inner().Write(file, data, size);
    if (known != handles_.end() && wrote > 0) {
      if (offset < 0) {
        unmodelled_ = "ftell failed on a recorded handle";
      }
      Op op;
      op.kind = Op::Kind::kWrite;
      op.handle = known->second;
      op.offset = offset < 0 ? 0 : static_cast<std::uint64_t>(offset);
      op.bytes.assign(static_cast<const char*>(data), wrote);
      log_.push_back(std::move(op));
    }
    return wrote;
  }

  int Rename(const std::string& from, const std::string& to) override {
    std::lock_guard<std::mutex> hold(mutex_);
    const int answer = Inner().Rename(from, to);
    std::string from_relative;
    std::string to_relative;
    const bool from_under = Under(from, &from_relative);
    const bool to_under = Under(to, &to_relative);
    if (answer == 0 && from_under != to_under) {
      unmodelled_ = "a rename with one end outside the root: " + from + " -> " + to;
    }
    if (answer == 0 && from_under && to_under) {
      Op op;
      op.kind = Op::Kind::kRename;
      op.path = from_relative;
      op.to = to_relative;
      log_.push_back(std::move(op));
    }
    return answer;
  }

  int Remove(const std::string& path) override {
    std::lock_guard<std::mutex> hold(mutex_);
    const int answer = Inner().Remove(path);
    std::string relative;
    if (answer == 0 && Under(path, &relative)) {
      Op op;
      op.kind = Op::Kind::kRemove;
      op.path = relative;
      log_.push_back(std::move(op));
    }
    return answer;
  }

  bool Sync(const std::string& path) override {
    std::lock_guard<std::mutex> hold(mutex_);
    std::string relative;
    const bool under = Under(path, &relative);
    if (under && refuse_sync_.count("/" + relative) != 0) {
      ++refused_;
      errno = EIO;
      return false;
    }
    const bool synced = Inner().Sync(path);
    if (synced && under) {
      Op op;
      op.kind = Op::Kind::kSync;
      op.path = relative;
      log_.push_back(std::move(op));
    }
    return synced;
  }

  const Snapshot& snapshot() const { return snapshot_; }
  std::vector<Op> log() const {
    std::lock_guard<std::mutex> hold(mutex_);
    return log_;
  }
  int refused() const {
    std::lock_guard<std::mutex> hold(mutex_);
    return refused_;
  }
  /// Non-empty when the recorder saw something it cannot replay faithfully.
  std::string unmodelled() const {
    std::lock_guard<std::mutex> hold(mutex_);
    return unmodelled_;
  }

 private:
  rommsync::io::Disk& Inner() const {
    return previous_ != nullptr ? *previous_ : rommsync::io::StdioDisk();
  }

  bool Under(const std::string& path, std::string* relative) const {
    const std::string normal = std::filesystem::path(path).lexically_normal().generic_string();
    if (normal == root_) {
      relative->clear();
      return true;
    }
    if (normal.size() > root_.size() && normal.compare(0, root_.size(), root_) == 0 &&
        normal[root_.size()] == '/') {
      *relative = normal.substr(root_.size() + 1);
      while (!relative->empty() && relative->back() == '/') relative->pop_back();
      return true;
    }
    return false;
  }

  std::string root_;
  Snapshot snapshot_;
  std::set<std::string> refuse_sync_;
  rommsync::io::Disk* previous_ = nullptr;
  mutable std::mutex mutex_;
  std::map<std::FILE*, int> handles_;
  int next_handle_ = 0;
  std::vector<Op> log_;
  int refused_ = 0;
  std::string unmodelled_;
};

// --- the card the log describes ----------------------------------------------

namespace model {

struct Pending {
  bool truncate = false;
  std::uint64_t offset = 0;
  std::string bytes;
};

inline std::string Apply(std::string content, const std::vector<Pending>& pending,
                         std::uint64_t budget) {
  for (const Pending& write : pending) {
    // Before the truncate check: a truncate after the budget is spent is a
    // later call landing when an earlier one did not, which a prefix is not.
    if (budget == 0) break;
    if (write.truncate) {
      content.clear();
      continue;
    }
    const std::size_t take =
        static_cast<std::size_t>(std::min<std::uint64_t>(budget, write.bytes.size()));
    budget -= take;
    const std::size_t end = static_cast<std::size_t>(write.offset) + take;
    if (content.size() < end) content.resize(end, '\0');
    content.replace(static_cast<std::size_t>(write.offset), take, write.bytes, 0, take);
  }
  return content;
}

struct Inode {
  std::string durable;
  std::vector<Pending> pending;  ///< since the last successful sync

  std::string Current() const { return Apply(durable, pending, UINT64_MAX); }

  /// Every content a power cut may leave, deduplicated.
  std::set<std::string> Survivors() const {
    std::set<std::string> out = {durable, Current()};
    std::uint64_t total = 0;
    for (const Pending& write : pending) total += write.bytes.size();
    out.insert(Apply(durable, pending, total / 2));  // torn
    for (std::size_t last = pending.size(); last-- > 0;) {
      if (pending[last].truncate) {
        // On FAT a file's size is directory metadata, and it can land before
        // any of the data written after it.
        out.insert(std::string());  // truncated
        continue;
      }
      out.insert(Apply(durable, pending, total - (pending[last].bytes.size() + 1) / 2));  // short
      out.insert(Apply(durable, {pending[last]}, UINT64_MAX));  // out of order
      break;
    }
    return out;
  }
};

struct Meta {
  enum class Kind { kLink, kUnlink, kRename };
  Kind kind = Kind::kLink;
  std::string name;
  std::string to;
  int inode = 0;
};

using Entries = std::map<std::string, int>;

inline Entries Play(Entries entries, const std::vector<Meta>& pending, std::size_t count) {
  for (std::size_t i = 0; i < count && i < pending.size(); ++i) {
    const Meta& meta = pending[i];
    switch (meta.kind) {
      case Meta::Kind::kLink:
        entries[meta.name] = meta.inode;
        break;
      case Meta::Kind::kUnlink:
        entries.erase(meta.name);
        break;
      case Meta::Kind::kRename: {
        const auto from = entries.find(meta.name);
        if (from != entries.end()) {
          const int inode = from->second;
          entries.erase(from);
          entries[meta.to] = inode;
        }
        break;
      }
    }
  }
  return entries;
}

struct Dir {
  Entries durable;
  std::vector<Meta> pending;
  Entries Current() const { return Play(durable, pending, pending.size()); }
};

class Card {
 public:
  explicit Card(const Snapshot& snapshot) {
    for (const std::string& dir : snapshot.dirs) dirs_[dir];
    for (const auto& [path, bytes] : snapshot.files) {
      const int inode = ++next_inode_;
      inodes_[inode].durable = bytes;
      dirs_[DirOf(path)].durable[LeafOf(path)] = inode;
    }
  }

  /// Apply one recorded call. Empty, or why the model cannot follow it.
  std::string Step(const Op& op) {
    switch (op.kind) {
      case Op::Kind::kOpen: {
        const auto dir = dirs_.find(DirOf(op.path));
        if (dir == dirs_.end()) return "a file opened in a directory the snapshot did not hold";
        const Entries current = dir->second.Current();
        const auto named = current.find(LeafOf(op.path));
        int inode = 0;
        if (named != current.end()) {
          inode = named->second;
          if (op.truncate) inodes_[inode].pending.push_back({true, 0, {}});
        } else {
          if (!op.truncate) return "a file opened for update that the model does not hold";
          inode = ++next_inode_;
          inodes_[inode];
          dir->second.pending.push_back({Meta::Kind::kLink, LeafOf(op.path), {}, inode});
        }
        handles_[op.handle] = inode;
        return {};
      }
      case Op::Kind::kWrite: {
        const auto handle = handles_.find(op.handle);
        if (handle == handles_.end()) return "a write on a handle the model never saw opened";
        inodes_[handle->second].pending.push_back({false, op.offset, op.bytes});
        return {};
      }
      case Op::Kind::kRename: {
        if (DirOf(op.path) != DirOf(op.to)) return "a rename across directories: " + Describe(op);
        dirs_[DirOf(op.path)].pending.push_back(
            {Meta::Kind::kRename, LeafOf(op.path), LeafOf(op.to), 0});
        return {};
      }
      case Op::Kind::kRemove:
        dirs_[DirOf(op.path)].pending.push_back({Meta::Kind::kUnlink, LeafOf(op.path), {}, 0});
        return {};
      case Op::Kind::kSync: {
        const auto dir = dirs_.find(op.path);
        if (dir != dirs_.end()) {
          dir->second.durable = dir->second.Current();
          dir->second.pending.clear();
          return {};
        }
        const auto parent = dirs_.find(DirOf(op.path));
        if (parent == dirs_.end()) return {};
        const Entries current = parent->second.Current();
        const auto named = current.find(LeafOf(op.path));
        if (named != current.end()) {
          Inode& inode = inodes_[named->second];
          inode.durable = inode.Current();
          inode.pending.clear();
        }
        return {};
      }
    }
    return {};
  }

  /// Every card a power cut right now may leave. `watched` are the directories
  /// whose losses are enumerated; every other one is taken as fully landed.
  std::set<Tree> Survivors(const std::set<std::string>& watched, std::size_t cap,
                           bool* capped) const {
    // The choices: how many of each watched directory's pending names landed.
    std::vector<std::pair<std::string, std::size_t>> axes;
    for (const auto& [name, dir] : dirs_) {
      if (watched.count(name) != 0 && !dir.pending.empty()) {
        axes.push_back({name, dir.pending.size()});
      }
    }
    std::set<Tree> out;
    std::vector<std::size_t> pick(axes.size(), 0);
    for (;;) {
      std::map<std::string, Entries> listing;
      for (const auto& [name, dir] : dirs_) listing[name] = dir.Current();
      for (std::size_t a = 0; a < axes.size(); ++a) {
        const Dir& dir = dirs_.at(axes[a].first);
        listing[axes[a].first] = Play(dir.durable, dir.pending, pick[a]);
      }
      Expand(listing, watched, cap, &out, capped);
      std::size_t a = 0;
      for (; a < axes.size(); ++a) {
        if (++pick[a] <= axes[a].second) break;
        pick[a] = 0;
      }
      if (a == axes.size() || *capped) break;
    }
    return out;
  }

  const std::map<std::string, Dir>& dirs() const { return dirs_; }

 private:
  void Expand(const std::map<std::string, Entries>& listing, const std::set<std::string>& watched,
              std::size_t cap, std::set<Tree>* out, bool* capped) const {
    // Each linked file's possible contents; the product of them is the cards.
    std::vector<std::pair<std::string, std::vector<std::string>>> files;
    for (const auto& [dir, entries] : listing) {
      for (const auto& [leaf, inode] : entries) {
        const std::string path = dir.empty() ? leaf : dir + "/" + leaf;
        const Inode& node = inodes_.at(inode);
        if (watched.count(dir) != 0) {
          const std::set<std::string> survivors = node.Survivors();
          files.push_back({path, {survivors.begin(), survivors.end()}});
        } else {
          files.push_back({path, {node.Current()}});
        }
      }
    }
    std::vector<std::size_t> pick(files.size(), 0);
    for (;;) {
      Tree tree;
      for (std::size_t f = 0; f < files.size(); ++f) tree[files[f].first] = files[f].second[pick[f]];
      out->insert(std::move(tree));
      if (out->size() > cap) {
        *capped = true;
        return;
      }
      std::size_t f = 0;
      for (; f < files.size(); ++f) {
        if (++pick[f] < files[f].second.size()) break;
        pick[f] = 0;
      }
      if (f == files.size()) return;
    }
  }

  std::map<std::string, Dir> dirs_;
  std::map<int, Inode> inodes_;
  std::map<int, int> handles_;
  int next_inode_ = 0;
};

}  // namespace model

// --- the replay ---------------------------------------------------------------

/// What the judge is shown: the card after the boot sweep, and what was on it
/// before the recorder was installed.
using Judge = std::function<std::string(const Tree& after, const Snapshot& before)>;

struct Verdict {
  std::size_t points = 0;    ///< places the log was cut: every call, and before the first
  std::size_t states = 0;    ///< distinct cards those cuts can leave, each swept and judged
  std::size_t failures = 0;  ///< cards the judge refused
  std::string first;         ///< the first refusal, with the cut that produced it
  std::string unmodelled;    ///< non-empty: the log holds something this model cannot follow
};

/// Cut the log everywhere, sweep and judge every card that can leave.
///
/// `watched` and `recover` are SD paths: the directories whose losses are
/// enumerated, and the ones `sync::RecoverStaging` sweeps, as a tick would on
/// the next boot.
inline Verdict Replay(const Recorder& recorder, const std::set<std::string>& watched,
                      const std::vector<std::string>& recover, const std::string& scratch,
                      const Judge& judge) {
  Verdict verdict;
  verdict.unmodelled = recorder.unmodelled();
  const std::vector<Op> log = recorder.log();
  std::set<std::string> watched_relative;
  for (const std::string& dir : watched) watched_relative.insert(RelativeOf(dir));

  model::Card card(recorder.snapshot());
  std::set<Tree> judged;
  for (std::size_t cut = 0; cut <= log.size(); ++cut) {
    if (cut > 0) {
      const std::string why = card.Step(log[cut - 1]);
      if (!why.empty() && verdict.unmodelled.empty()) verdict.unmodelled = why;
    }
    ++verdict.points;
    bool capped = false;
    const std::set<Tree> cards = card.Survivors(watched_relative, 4096, &capped);
    if (capped && verdict.unmodelled.empty()) {
      verdict.unmodelled = "more than 4096 cards at one cut; narrow `watched`";
    }
    for (const Tree& tree : cards) {
      if (!judged.insert(tree).second) continue;
      ++verdict.states;

      std::error_code error;
      std::filesystem::remove_all(scratch, error);
      for (const auto& [dir, unused] : card.dirs()) {
        std::filesystem::create_directories(std::filesystem::path(scratch) / dir, error);
      }
      for (const auto& [path, bytes] : tree) {
        std::ofstream(std::filesystem::path(scratch) / path, std::ios::binary) << bytes;
      }
      const std::unique_ptr<rommsync::fs::FileSystem> files =
          rommsync::host::MakeNativeFileSystem(scratch);
      rommsync::sync::RecoverStaging(*files, recover);

      const std::string refused = judge(Read(scratch).files, recorder.snapshot());
      if (refused.empty()) continue;
      ++verdict.failures;
      if (verdict.first.empty()) {
        std::ostringstream where;
        where << refused << " -- power cut after " << cut << " of " << log.size() << " calls";
        if (cut > 0) where << " (the last: " << Describe(log[cut - 1]) << ")";
        where << "; the card held:";
        for (const auto& [path, bytes] : tree) where << " " << path << "=" << bytes.size() << "B";
        verdict.first = where.str();
      }
    }
  }
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  return verdict;
}

/// The invariant hard rule 2 is about, for one overwrite of one save.
struct Overwrite {
  std::string save;        ///< SD path
  std::string old_bytes;   ///< what it held before
  std::string new_bytes;   ///< what the overwrite puts there
  std::string backup_dir;  ///< SD path
};

/// Every new file under `expected.backup_dir` holds the old bytes in full, and
/// `*backed_up` says whether there is one. A refusal, or empty.
inline std::string BackupsHoldOld(const Tree& after, const Snapshot& before,
                                  const Overwrite& expected, bool* backed_up) {
  const std::string prefix = RelativeOf(expected.backup_dir) + "/";
  *backed_up = false;
  for (const auto& [path, bytes] : after) {
    if (path.compare(0, prefix.size(), prefix) != 0) continue;
    if (path.find('/', prefix.size()) != std::string::npos) continue;
    const auto was = before.files.find(path);
    if (was != before.files.end() && was->second == bytes) continue;
    // A `.tmp` is staging nothing will ever read as a backup.
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".tmp") == 0) continue;
    if (bytes != expected.old_bytes) {
      return "the backup " + path + " reads as present and does not hold the old bytes (" +
             std::to_string(bytes.size()) + " bytes)";
    }
    *backed_up = true;
  }
  return {};
}

/// After any power cut and the boot sweep: the save is its old bytes or its new
/// ones; every backup is either one that was already there, or holds the old
/// bytes in full; and a save holding its new bytes has a backup holding the old.
inline Judge OldOrNew(const Overwrite& expected) {
  return [expected](const Tree& after, const Snapshot& before) -> std::string {
    const auto save = after.find(RelativeOf(expected.save));
    if (save == after.end()) return "the save is missing";
    const bool is_old = save->second == expected.old_bytes;
    const bool is_new = save->second == expected.new_bytes;
    if (!is_old && !is_new) {
      return "the save is neither its old bytes nor its new ones (" +
             std::to_string(save->second.size()) + " bytes)";
    }
    bool backed_up = false;
    const std::string refused = BackupsHoldOld(after, before, expected, &backed_up);
    if (!refused.empty()) return refused;
    if (is_new && !is_old && !backed_up) {
      return "the save holds its new bytes and no backup under " + expected.backup_dir +
             " holds the old ones";
    }
    return {};
  };
}

/// The same invariant for a removal (#233's `delete`), where the "new" state is
/// no file at all: the save is its old bytes or absent, and absent only with a
/// backup holding the old. `new_bytes` is not read.
inline Judge OldOrGone(const Overwrite& expected) {
  return [expected](const Tree& after, const Snapshot& before) -> std::string {
    const auto save = after.find(RelativeOf(expected.save));
    if (save != after.end() && save->second != expected.old_bytes) {
      return "the save is not its old bytes (" + std::to_string(save->second.size()) +
             " bytes)";
    }
    bool backed_up = false;
    const std::string refused = BackupsHoldOld(after, before, expected, &backed_up);
    if (!refused.empty()) return refused;
    if (save == after.end() && !backed_up) {
      return "the save is gone and no backup under " + expected.backup_dir +
             " holds its bytes";
    }
    return {};
  };
}

/// Replay `recorder`'s log against `judge` -- `OldOrNew(expected)` unless told
/// otherwise -- and report into `c`.
///
/// Also refuses a vacuous pass: a log with no successful sync in it, or one the
/// model could not follow, proves nothing about durability.
inline Verdict ExpectSurvives(checks::Checks& c, const Recorder& recorder,
                              const Overwrite& expected, const std::string& scratch,
                              const std::string& what, const Judge& judge = nullptr) {
  // `DirOf` answers "" for a file at the root, which is "/" as an SD path.
  const std::string parent = DirOf(expected.save);
  const std::string save_dir = parent.empty() ? std::string("/") : parent;
  const Verdict verdict = Replay(recorder, {save_dir, expected.backup_dir}, {save_dir}, scratch,
                                 judge != nullptr ? judge : OldOrNew(expected));
  std::cout << "  " << what << ": " << verdict.points << " cuts, " << verdict.states
            << " cards, " << verdict.failures << " lost\n";
  c.Expect(verdict.unmodelled.empty(), what + ": the replay followed every call: " +
                                           verdict.unmodelled);
  c.Expect(verdict.states > 1, what + ": the replay saw more than one card");
  c.ExpectEq(verdict.failures, std::size_t{0}, what + ": no power cut loses the save -- " +
                                                   verdict.first);
  return verdict;
}

/// The invariant for a placement: a save written where there was none (#237).
struct Created {
  std::string save;       ///< SD path; nothing is there before
  std::string new_bytes;  ///< what the placement puts there
};

/// After any power cut and the boot sweep: the save is absent or holds its new
/// bytes in full -- never a part of them, which the emulator would load as a
/// corrupt save and the next tick would upload -- and every file that was on
/// the card before is still there, unchanged.
inline Judge AbsentOrNew(const Created& expected) {
  return [expected](const Tree& after, const Snapshot& before) -> std::string {
    const auto save = after.find(RelativeOf(expected.save));
    if (save != after.end() && save->second != expected.new_bytes) {
      return "the placed save is there and is not its new bytes (" +
             std::to_string(save->second.size()) + " bytes)";
    }
    for (const auto& [path, bytes] : before.files) {
      const auto now = after.find(path);
      if (now == after.end()) return "the placement cost " + path;
      if (now->second != bytes) return "the placement changed " + path;
    }
    return {};
  };
}

/// Replay `recorder`'s log against `AbsentOrNew(expected)` and report into `c`,
/// refusing a vacuous pass as `ExpectSurvives` does.
inline Verdict ExpectCreated(checks::Checks& c, const Recorder& recorder, const Created& expected,
                             const std::string& scratch, const std::string& what) {
  const std::string parent = DirOf(expected.save);
  const std::string save_dir = parent.empty() ? std::string("/") : parent;
  const Verdict verdict =
      Replay(recorder, {save_dir}, {save_dir}, scratch, AbsentOrNew(expected));
  std::cout << "  " << what << ": " << verdict.points << " cuts, " << verdict.states
            << " cards, " << verdict.failures << " lost\n";
  c.Expect(verdict.unmodelled.empty(), what + ": the replay followed every call: " +
                                           verdict.unmodelled);
  c.Expect(verdict.states > 1, what + ": the replay saw more than one card");
  c.ExpectEq(verdict.failures, std::size_t{0},
             what + ": no power cut leaves a part of the save -- " + verdict.first);
  return verdict;
}

}  // namespace crash

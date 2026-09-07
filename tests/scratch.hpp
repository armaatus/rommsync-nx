// The scratch directory a test binary writes into -- one per process.
//
// `ROMMSYNC_TEST_SCRATCH` is per build tree, which keeps three worktrees apart
// but not two `ctest` invocations against ONE of them: the local verification
// loop and the automated review both run `ctest`, and an agent may be running a
// third (#151). RUN_SERIAL orders tests within one invocation and says nothing
// about a second one (#118), so destinations under fixed names -- the
// `download.bin` in test_http_native.cpp is the standing example -- had two
// processes removing and renaming each other's files.
//
// A leaf named for the pid is what `harness::Sandbox` already did for the
// engine-level tests; this is the same rule one level down, for everything that
// reaches for the scratch directory directly.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>

#include <signal.h>   // kill(pid, 0): is the owner of that leaf still running?
#include <unistd.h>   // getpid

namespace scratch {

/// The `pid-` in `<build>/tests/scratch/pid-4213`.
inline constexpr const char* kLeafPrefix = "pid-";

/// The directory the leaves sit in: one per build tree, and therefore one
/// shared by every `ctest` invocation against that tree. That sharing is the
/// hazard this file exists for (#151) and, read the other way, the only place
/// two runs against one worktree can see each other at all -- which is what
/// `rig::sessions` attributes a live sync session through (#174).
inline std::filesystem::path Root() { return ROMMSYNC_TEST_SCRATCH; }

/// The digits of `text` as a number no larger than `limit`, or 0 for anything
/// that is not one.
///
/// Names carry the numbers here -- a leaf is `pid-4213`, a session claim is
/// `session-815-pid-4213` -- so every reader has to take one apart, and a name
/// this suite did not write must read as 0 rather than as something plausible.
inline long long Digits(const std::string& text, long long limit) {
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
    return 0;
  }
  errno = 0;
  const long long value = std::strtoll(text.c_str(), nullptr, 10);
  if (errno == ERANGE || value > limit) {
    return 0;
  }
  return value;
}

/// The same, bounded to what a `pid_t` can hold.
///
/// A number no `pid_t` can hold is not one this suite wrote, whatever it looks
/// like, and casting it would hand `kill` a truncated or saturated value -- 0 is
/// this process's group and -1 is every process the user can signal, and both
/// answer "running".
inline long long Pid(const std::string& text) {
  return Digits(text, static_cast<long long>(std::numeric_limits<pid_t>::max()));
}

/// Is `pid` a process that still exists?
///
/// `ESRCH` is the only answer that means gone. `EPERM` is a live process this
/// user does not own, which is still a reason to leave what it owns alone.
///
/// `Sweep` retires a leaf by this, and `rig::sessions` retires a claim on a sync
/// session by it: in both, what a run holds lapses when the run does, so nothing
/// has to survive a crash in order to clean up after it.
inline bool Running(long long pid) {
  if (pid <= 0) {
    return false;
  }
  errno = 0;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

/// The name of the leaf a process of this pid owns: `pid-4213`.
///
/// Assembled here rather than at each site, because `LeafOwner` below has to
/// take it apart again and test_harness.cpp has to recognise one.
inline std::string LeafName(long long pid) { return kLeafPrefix + std::to_string(pid); }

namespace detail {

/// The pid a leaf is named for, or 0 for a name that is not one of ours.
inline long long LeafOwner(const std::string& name) {
  const std::string prefix = kLeafPrefix;
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
    return 0;
  }
  return Pid(name.substr(prefix.size()));
}

/// Set by `scratch::Keep()`, and read once the process is on its way out.
inline bool& Kept() {
  static bool kept = false;
  return kept;
}

/// Has anything asked for this process's leaf to survive?
inline bool Keeping() {
  const char* keep = std::getenv("ROMMSYNC_KEEP_SCRATCH");
  return Kept() || (keep != nullptr && *keep != '\0');
}

/// The file inside a leaf that says the leaf is being looked at.
///
/// Removal at exit is only half of what would take a kept leaf away: the NEXT
/// test binary in the same `ctest` run sweeps it, because by then its owner is
/// gone. So the request has to outlive the process that made it, and a file in
/// the directory is the only place it can live.
inline constexpr const char* kKeepMarker = ".keep";

/// Remove the leaf `path` unless something asked for it to stay, in which case
/// print where it is -- the only way to look at what a red run actually wrote.
inline void Discard(const std::filesystem::path& path) {
  std::error_code error;
  if (Keeping()) {
    // The marker is the whole of the request -- without it the next process's
    // sweep takes the leaf. Announcing a path that was not marked would be a
    // lie told in exactly the debugging session this exists for.
    std::ofstream marker(path / kKeepMarker);
    if (marker) {
      std::cerr << "  scratch kept at " << path.string() << "\n";
    } else {
      std::cerr << "  could not keep " << path.string() << ": it is already gone\n";
    }
    return;
  }
  std::filesystem::remove_all(path, error);
}

/// Owns this process's leaf for as long as the process lives.
class Leaf {
 public:
  explicit Leaf(std::filesystem::path path) : path_(std::move(path)) {}
  ~Leaf() { Discard(path_); }

  Leaf(const Leaf&) = delete;
  Leaf& operator=(const Leaf&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace detail

/// Keep this process's leaf instead of removing it on the way out.
///
/// `ROMMSYNC_KEEP_SCRATCH` is the switch for that; this is the same request made
/// in code, so that a debugging switch on something living INSIDE the leaf --
/// `ROMMSYNC_KEEP_SANDBOX`, in tests/harness.hpp -- does not keep a directory
/// this then deletes out from under it.
///
/// A leaf kept this way is kept until somebody removes it by hand: `Sweep` obeys
/// the marker it leaves behind, and only the next process to be given the same
/// pid takes it away.
inline void Keep() { detail::Kept() = true; }

/// Remove the leaves of runs that are no longer running.
///
/// `Dir()` calls this once, before it creates its own; a process CTest killed on
/// TIMEOUT never got to remove its own, and without this those accumulate one
/// per killed test forever. Exposed rather than hidden because it deletes
/// directories, and test_harness.cpp holds it to which ones.
///
/// `mine` is left alone whatever it holds -- the caller is about to replace it,
/// and its pid is by definition running.
inline void Sweep(const std::filesystem::path& root, const std::filesystem::path& mine) {
  std::error_code error;
  std::filesystem::directory_iterator entry(root, error);
  const std::filesystem::directory_iterator end;
  while (!error && entry != end) {
    const std::filesystem::path leaf = entry->path();
    // Stepped before the body, and reporting rather than throwing: this walks a
    // directory it is removing entries from, and a second process sweeping the
    // same root removes more. A `filesystem_error` here would escape `Dir()`'s
    // static initializer and end the process with no test result at all.
    entry.increment(error);

    if (leaf == mine) {
      continue;
    }
    const long long owner = detail::LeafOwner(leaf.filename().string());
    if (owner == 0 || Running(owner)) {
      continue;
    }
    std::error_code ignored;
    if (std::filesystem::exists(leaf / detail::kKeepMarker, ignored)) {
      continue;  // somebody is reading it; see detail::kKeepMarker
    }
    std::filesystem::remove_all(leaf, ignored);
  }
}

/// This process's scratch directory, created on the first call and removed when
/// the process exits.
///
/// A pid is reused, so an existing leaf under this one's number belongs to a run
/// that is over: it is removed rather than adopted -- kept or not, since a leaf
/// this process is about to write into is no longer the one that was kept --
/// which is what makes "a partial file left from an earlier run" mean an earlier
/// run of *this* process.
///
/// If that leaf cannot be removed, this process **stops**, with `exit(2)` and the
/// reason. There is nothing else honest to do: a binary that could not get a
/// scratch directory of its own cannot produce a result anyone should read, and
/// carrying on means running the assertions against another run's leftovers.
/// 2 is what the test mains already return for a setup they cannot complete.
inline const std::string& Dir() {
  static const std::string dir = [] {
    const std::filesystem::path root = Root();
    const std::filesystem::path mine = root / LeafName(static_cast<long long>(::getpid()));

    std::error_code made_root;
    std::filesystem::create_directories(root, made_root);
    Sweep(root, mine);

    // An `error_code` each, because the next call clears the last one's -- and
    // the failure is acted on rather than only announced. A `remove_all` that
    // failed, followed by a `create_directories` that returns cleanly for "it is
    // already there", is how this process would ADOPT a dead run's leaf: the one
    // case in which a `.part` from another run can still be sitting in what a
    // scenario believes is a fresh scratch directory.
    std::error_code removed;
    std::filesystem::remove_all(mine, removed);
    if (removed) {
      std::cerr << "could not clear " << mine.string() << ": " << removed.message()
                << "\n  it belongs to a run that is over, and this one will not write"
                   " into it\n  remove it by hand and run again\n";
      std::exit(2);
    }
    std::error_code made;
    std::filesystem::create_directories(mine, made);
    if (made) {
      std::cerr << "could not create " << mine.string() << ": " << made.message() << "\n";
      std::exit(2);
    }

    static const detail::Leaf leaf(mine);
    return leaf.path().string();
  }();
  return dir;
}

}  // namespace scratch

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
#include <iostream>
#include <string>
#include <system_error>

#include <signal.h>   // kill(pid, 0): is the owner of that leaf still running?
#include <unistd.h>   // getpid

namespace scratch {

/// The `pid-` in `<build>/tests/scratch/pid-4213`.
inline constexpr const char* kLeafPrefix = "pid-";

namespace detail {

/// Is `pid` a process that still exists?
///
/// `ESRCH` is the only answer that means gone. `EPERM` is a live process this
/// user does not own, which is still a reason to leave its leaf alone.
inline bool Running(long long pid) {
  if (pid <= 0) {
    return false;
  }
  errno = 0;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
}

/// The pid a leaf is named for, or 0 for a name that is not one of ours.
inline long long LeafOwner(const std::string& name) {
  const std::string prefix = kLeafPrefix;
  if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
    return 0;
  }
  const std::string digits = name.substr(prefix.size());
  if (digits.find_first_not_of("0123456789") != std::string::npos) {
    return 0;
  }
  return std::strtoll(digits.c_str(), nullptr, 10);
}

/// Remove the leaf `path` unless `ROMMSYNC_KEEP_SCRATCH` is set, in which case
/// print where it is -- the only way to look at what a red run actually wrote,
/// the way `ROMMSYNC_KEEP_SANDBOX` is for `harness::Sandbox`.
inline void Discard(const std::filesystem::path& path) {
  if (const char* keep = std::getenv("ROMMSYNC_KEEP_SCRATCH"); keep != nullptr && *keep != '\0') {
    std::cerr << "  scratch kept at " << path.string() << "\n";
    return;
  }
  std::error_code error;
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
  std::filesystem::directory_iterator entries(root, error);
  if (error) {
    return;
  }
  for (const std::filesystem::directory_entry& entry : entries) {
    if (entry.path() == mine) {
      continue;
    }
    const long long owner = detail::LeafOwner(entry.path().filename().string());
    if (owner == 0 || detail::Running(owner)) {
      continue;
    }
    std::error_code removal;
    std::filesystem::remove_all(entry.path(), removal);
  }
}

/// This process's scratch directory, created on the first call and removed when
/// the process exits.
///
/// A pid is reused, so an existing leaf under this one's number belongs to a run
/// that is over: it is removed rather than adopted, which is what makes
/// "a partial file left from an earlier run" mean an earlier run of *this*
/// process.
inline const std::string& Dir() {
  static const std::string dir = [] {
    const std::filesystem::path root = ROMMSYNC_TEST_SCRATCH;
    const std::filesystem::path mine =
        root / (std::string(kLeafPrefix) + std::to_string(static_cast<long long>(::getpid())));

    std::error_code error;
    std::filesystem::create_directories(root, error);
    Sweep(root, mine);
    std::filesystem::remove_all(mine, error);
    std::filesystem::create_directories(mine, error);
    if (error) {
      std::cerr << "could not create " << mine.string() << ": " << error.message() << "\n";
    }

    static const detail::Leaf leaf(mine);
    return leaf.path().string();
  }();
  return dir;
}

}  // namespace scratch

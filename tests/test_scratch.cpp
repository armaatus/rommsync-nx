// The one promise in tests/scratch.hpp that no other test can put to it: a
// process which cannot clear its own leaf STOPS, rather than adopting a dead
// run's directory and running its assertions against the leftovers (#169).
//
// **Its own binary on purpose.** `scratch::Dir()` memoizes in a function-local
// static, so a process that has already called it hands a forked child the
// PARENT's leaf instead of computing the child's -- and `test_harness.cpp`'s
// `main` calls it before it dispatches to any scenario. Nothing here may touch
// `scratch::Dir()` before the fork, which is why this is a translation unit of
// its own rather than one more scenario over there.
//
// No server, and no root: 0500 on a directory is something an ordinary user can
// do to their own, and it is enough to make the file inside unremovable.

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "checks.hpp"
#include "scratch.hpp"

namespace {

/// The leaf `scratch::Dir()` would choose for `pid` -- named without calling it,
/// because calling it here is the one thing this file may not do.
std::filesystem::path LeafFor(long long pid) {
  return std::filesystem::path(ROMMSYNC_TEST_SCRATCH) / scratch::LeafName(pid);
}

/// A directory holding one file, which its owner cannot empty.
///
/// `owner_read | owner_exec` -- listable and searchable, but not writable, so
/// the unlink `remove_all` needs is refused. The file is the thing a run must
/// never inherit: a partial download under the standing fixed name.
bool PlantUnclearableLeaf(const std::filesystem::path& leaf) {
  std::error_code ignored;
  std::filesystem::remove_all(leaf, ignored);
  std::filesystem::create_directories(leaf, ignored);
  {
    std::ofstream stale(leaf / "download.bin.part");
    stale << "bytes from a run that is over";
  }
  std::error_code sealed;
  std::filesystem::permissions(
      leaf, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::replace, sealed);
  return !sealed;
}

void Unseal(const std::filesystem::path& leaf) {
  std::error_code ignored;
  std::filesystem::permissions(leaf, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ignored);
  std::filesystem::remove_all(leaf, ignored);
}

}  // namespace

int main() {
  checks::Checks checks;

  std::error_code ignored;
  std::filesystem::create_directories(ROMMSYNC_TEST_SCRATCH, ignored);

  // The child may not compute its leaf until the parent has planted it, and the
  // parent cannot plant it until it knows the child's pid. One byte settles it.
  int ready[2];
  if (::pipe(ready) != 0) {
    std::cerr << "  FAIL: could not open a pipe: " << std::strerror(errno) << "\n";
    return 1;
  }

  std::fflush(nullptr);  // nothing of ours may be flushed twice by the child
  const pid_t child = ::fork();
  if (child < 0) {
    // Without a child there is nothing to reap, and every assertion below would
    // report on a fork that never happened rather than on the leaf.
    checks.Expect(false, "the test can fork");
    return 1;
  }

  if (child == 0) {
    ::close(ready[1]);
    char go = 0;
    while (::read(ready[0], &go, 1) < 0 && errno == EINTR) {
      // a signal, not an answer
    }
    ::close(ready[0]);

    // The first `scratch::Dir()` in this process, and the last thing it does.
    const std::string dir = scratch::Dir();
    // Reached only if it did not refuse, which is the failure the parent names.
    std::cerr << "  scratch::Dir() handed back " << dir << " instead of refusing\n";
    ::_exit(0);
  }

  ::close(ready[0]);
  const std::filesystem::path leaf = LeafFor(child);
  const bool planted = PlantUnclearableLeaf(leaf);
  checks.Expect(planted, "a leaf the child cannot empty is planted under its pid");

  const char go = 1;
  const ssize_t sent = ::write(ready[1], &go, 1);
  ::close(ready[1]);  // the child is released here, or by the EOF this causes
  checks.Expect(sent == 1, "the child is told the leaf is there");

  int status = 0;
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    // a signal, not an exit
  }

  // Read before the cleanup below can disturb it: the point is that the child
  // did not take the file away, or write beside it.
  const bool stale_survived = std::filesystem::exists(leaf / "download.bin.part");
  Unseal(leaf);

  checks.Expect(WIFEXITED(status), "the child exited rather than dying on a signal");
  if (WIFEXITED(status)) {
    checks.ExpectEq(WEXITSTATUS(status), 2,
                    "and it refused with 2 rather than adopting the leaf");
  }
  checks.Expect(stale_survived, "the partial file it refused to inherit is still there");

  if (checks.failures() == 0) {
    std::cout << "scratch.refuses ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

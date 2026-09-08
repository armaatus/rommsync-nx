// Unit test with no external dependencies. Its job is to prove the CMake/CTest
// wiring itself works, so a red `ctest` always means something real.
#include <cstring>
#include <iostream>

#include "rommsync/core.hpp"

int main() {
  int failures = 0;

  if (std::strlen(rommsync::version()) == 0) {
    std::cerr << "version() is empty\n";
    ++failures;
  }
  if (std::strncmp(rommsync::kUserAgent, "rommsync-nx/", 12) != 0) {
    std::cerr << "unexpected user agent: " << rommsync::kUserAgent << "\n";
    ++failures;
  }

  // Verbatim, suffix and all: a release candidate reports `1.0.0-rc1`, not the
  // `1.0.0` CMake's own `project()` was given. See the root CMakeLists.txt.
  if (std::strcmp(rommsync::version(), ROMMSYNC_VERSION_EXPECTED) != 0) {
    std::cerr << "version() is " << rommsync::version() << ", VERSION says "
              << ROMMSYNC_VERSION_EXPECTED << "\n";
    ++failures;
  }

  return failures == 0 ? 0 : 1;
}

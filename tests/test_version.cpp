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

  // What the binary reports is the VERSION file verbatim, suffix and all.
  // `project(VERSION)` takes numbers only, so a release candidate is stripped
  // to `1.0.0` for CMake's own use -- and substituting PROJECT_VERSION into
  // version.hpp.in would ship that stripped copy on the host while switch.mk
  // sed'd the whole string into the Switch build. Nothing compares the two
  // builds, so that drift would be silent, and the shared VERSION file exists
  // to make it impossible.
  if (std::strcmp(rommsync::version(), ROMMSYNC_VERSION_EXPECTED) != 0) {
    std::cerr << "version() is " << rommsync::version() << ", VERSION says "
              << ROMMSYNC_VERSION_EXPECTED << "\n";
    ++failures;
  }

  return failures == 0 ? 0 : 1;
}

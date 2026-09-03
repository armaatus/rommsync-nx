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

  return failures == 0 ? 0 : 1;
}

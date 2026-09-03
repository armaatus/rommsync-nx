// The compiled-in version string, so a build in the wild can say what it is.
// kVersion itself comes from CMake via version.hpp.in.
#include "rommsync/version.hpp"

namespace rommsync {

const char* version() { return kVersion; }

}  // namespace rommsync

// The compiled-in version string, so a build in the wild can say what it is.
// kVersion itself comes from CMake via version.hpp.in.
#include "rommsync/version.hpp"

namespace rommsync {

// The marker's tail, not kVersion: see kBuildMarker for why.
const char* version() { return kBuildMarker + sizeof(kBuildMarkerPrefix) - 1; }

}  // namespace rommsync

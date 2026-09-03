// Placeholder translation unit so the core library has something to compile
// before M0-2 lands the HttpClient interface. Replace, do not accumulate.
#include "rommsync/version.hpp"

namespace rommsync {

const char* version() { return kVersion; }

}  // namespace rommsync

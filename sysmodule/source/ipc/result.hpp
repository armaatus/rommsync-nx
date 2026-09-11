// The one thing the two halves of the IPC boundary have to agree about that
// `core/` cannot state: an `ipc::Error` as a Horizon `Result`.
//
// Split out of `service.hpp` by M9-7 (#198) so it can be compiled by a host
// compiler. Everything else in `service.cpp` unpacks a real `cmif` message and
// cannot leave a console; this is four lines of arithmetic that the *overlay*
// inverts (`overlay::DecodeError`), and until it was on its own the host side of
// that pair was tested against a second copy of the mapping -- which is exactly
// the drift the pair exists to prevent.
//
// `core/` may not name a `Result` (hard rule 4), which is why the mapping lives
// on this side of the boundary rather than in the header both halves include.
#pragma once

#include <switch.h>

#include "rommsync/ipc.hpp"

namespace rommsync::sysmodule {

/// The Horizon result module these errors are reported under.
///
/// The number itself is `ipc::kResultModule`, in `core/`, because the overlay
/// needs it too: it maps a failing `Result` back to the `ipc::Error` this side
/// mapped it from, and a second copy of the number here is the two halves
/// disagreeing about a wire constant. Kept as a name because this is where it
/// is *used*, and because a `u32` is the type this side of the boundary speaks.
inline constexpr u32 kResultModule = ipc::kResultModule;

/// `error` as a Horizon `Result`. `kOk` is `0`; everything else is
/// `MAKERESULT(kResultModule, <the enum's ordinal>)`, so a description of `N`
/// reads straight off `ipc::Error` and a new error cannot silently reuse one.
Result ToResult(ipc::Error error);

}  // namespace rommsync::sysmodule

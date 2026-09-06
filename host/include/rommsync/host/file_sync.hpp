// The host half of `io::FileSync`: `fsync`, behind the seam `core/` may not
// reach through.
//
// `core/include/rommsync/atomic_file.hpp` says what the hook buys -- a backup
// whose *bytes* are on the card before the save it protects is overwritten,
// rather than only its name. This says how that happens on a laptop and in CI.
// Nothing under `core/` may include this header; the Horizon equivalent lives
// in `sysmodule/source/main.cpp`, where it is the same two calls over libnx's
// devoptab.
#pragma once

namespace rommsync::host {

/// Install `fsync(fileno(...))` as the process's `io::FileSync`.
///
/// Call once from `main`, before anything writes. A process that does not call
/// it keeps the weaker promise the `io` module makes on its own: no reader ever
/// sees half a file, and a power cut may still cost the bytes.
void InstallPosixFileSync();

}  // namespace rommsync::host

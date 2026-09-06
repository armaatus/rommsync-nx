// The Horizon half of `fs::FileSystem`: reading a directory on the SD card.
//
// `core/include/rommsync/file_system.hpp` states the interface and why it
// exists -- reading a directory is the second thing after HTTP that Horizon and
// the host do differently. `host/src/native_file_system.cpp` is the desktop
// backend and this is the console one, over the `fsdev` devoptab libnx mounts at
// `sdmc:`.
//
// **Nothing above this line learns the prefix.** `core/` produces SD-root
// absolute paths (`/retroarch/saves/Game.srm`) and `Resolve` is what turns one
// into something `io::ReadFile` and `state::HashFile` can `fopen` -- which is
// the whole reason a save the scanner names can be opened by anything else.
//
// It is written against `<dirent.h>` and `<sys/stat.h>` rather than libnx's
// `fsFsOpenDirectory`, because `fsdev` already mounts the card as a devoptab and
// the standard calls go straight through it. That also keeps this file's shape
// the same as the host backend's, which is worth more here than a syscall saved:
// the two have to agree about `.` and `..`, about what a truncated listing
// keeps, and about what an unreadable mtime means, and two files written the
// same way are two files a reader can diff.
//
// Nothing here has ever run: it is Horizon-side and is exercised in Ryujinx
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).
#pragma once

#include <memory>

#include "rommsync/file_system.hpp"

namespace rommsync::sysmodule {

/// The console's SD card, behind the interface `core/` reads directories with.
///
/// One per process. It holds no handle and no state -- every call opens what it
/// needs and closes it -- so it is safe to share between the IPC thread and the
/// worker, which is what `ipc::Engine` requires of anything both touch.
std::unique_ptr<fs::FileSystem> MakeSdCard();

}  // namespace rommsync::sysmodule

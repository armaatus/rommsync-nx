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
// Nothing here has run on a console yet (sysmodule/AGENTS.md), but since M9-13
// (#202) all of it runs on the host: it names no libnx type, so `test_card`
// compiles this file unchanged and roots it at a scratch directory through
// `MakeSdCardAt`.
#pragma once

#include <memory>
#include <string>

#include "rommsync/file_system.hpp"

namespace rommsync::sysmodule {

/// The console's SD card, behind the interface `core/` reads directories with.
///
/// One per process. It holds no handle and no state -- every call opens what it
/// needs and closes it -- so it is safe to share between the IPC thread and the
/// worker, which is what `ipc::Engine` requires of anything both touch.
///
/// `format` is what `main.cpp` could learn about the card from libnx, which is
/// the one thing this file cannot learn through a devoptab: `VolumeOf` reports it
/// and decides the per-file ceiling from it.
std::unique_ptr<fs::FileSystem> MakeSdCard(fs::Format format);

/// The same card, mounted at `mount` instead of `kSdRoot`. For `test_card`: a
/// host directory standing in for `sdmc:`, so the file that ships is the file
/// under test.
std::unique_ptr<fs::FileSystem> MakeSdCardAt(std::string mount, fs::Format format);

}  // namespace rommsync::sysmodule

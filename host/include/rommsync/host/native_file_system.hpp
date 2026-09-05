// The native `fs::FileSystem` backend, built with the host toolchain only.
//
// `core/rommsync/file_system.hpp` says the engine needs the names, kinds, sizes
// and mtimes in one directory; this says how that happens on a laptop and in
// CI. Nothing under `core/` may include this header -- the engine knows only
// the interface, and this is the one place `<filesystem>` is named on the save
// scanner's behalf. The Horizon equivalent will live in `sysmodule/`.
#pragma once

#include <memory>
#include <string>

#include "rommsync/file_system.hpp"

namespace rommsync::host {

/// A `FileSystem` rooted at `root`, the way Horizon's is rooted at `sdmc:`.
///
/// SD-root paths are resolved under `root`, so `/retroarch/saves` reads
/// `<root>/retroarch/saves`. That is what lets a test point the engine at a
/// `harness::Sandbox` and lets the desktop build point it at a real card.
///
/// A path that escapes `root` -- one carrying a `..` segment or a NUL -- is
/// refused as `ListError::kMissing` rather than resolved. `config` already
/// refuses `..` on the way in (`NormalizeSdPath`), so a path that has one here
/// did not come from the folder map, and resolving it would let a caller read
/// outside the card it was handed. Never returns null.
std::unique_ptr<fs::FileSystem> MakeNativeFileSystem(std::string root);

}  // namespace rommsync::host

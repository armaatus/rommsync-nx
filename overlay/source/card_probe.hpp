// What the overlay reads off the SD card when the sysmodule does not answer.
//
// M6-2 (#33). There are two switches: ovl-sysmodules' boot toggle decides
// whether `sys-rommsync` exists as a process, and `[sync] enabled` decides
// whether a resident one syncs. When IPC connect fails there is no process to
// ask, and "not running" and "disabled" are one sentence apart with entirely
// different remedies -- so the overlay looks at the card and says which it is
// (../docs/DEVELOPMENT.md#the-two-switches).
//
// The decision about those facts is `overlay::RenderUnreachable(link, card)` in
// `core/`, where `ctest -R overlay.status` reaches it. This file is only the
// peeks: `core/` may name neither an `sdmc:` path nor a title id (hard
// rule 4), and the platform side is where both live.
//
// **Read-only, all of it.** The sysmodule owns every write to `config.ini`, and
// `boot2.flag` is ovl-sysmodules': a second overlay creating it is how the two
// come to disagree about what is on. Nothing here opens a file for writing.
#pragma once

#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// The sysmodule's Atmosphère program id, as the directory name under
/// `atmosphere/contents/`.
///
/// The same value as `title_id` in `sysmodule/sys-rommsync.json`, which is where
/// `scripts/package.sh` reads it from and where it will move from when the id is
/// confirmed against the installed homebrew set (`sysmodule/README.md`).
/// `tests/test_package.sh layout` holds the two against each other, because a
/// stale copy here is an overlay that reports a correctly installed sysmodule as
/// missing.
inline constexpr const char* kProgramIdHex = "4200000000524D53";

/// Look at the card. Never fails: every field is a question with a `false`
/// answer, and a card that cannot be read is one that says nothing.
CardState ProbeCard();

}  // namespace rommsync::overlay

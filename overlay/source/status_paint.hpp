// The status screen's layout, with no renderer in it.
//
// M9-7 (#198) split `StatusScreen::Draw` in two. What is left in
// `status_screen.cpp` is a `tsl::elm::CustomDrawer` callback and an adapter that
// replays each command into `tsl::gfx::Renderer`; the row positions, the clip
// against the panel's foot and the progress bar's arithmetic are here, where
// `ctest -R overlay.draw` can record what was asked for and assert on it.
//
// This is the second half of the split `overlay/AGENTS.md` already describes.
// A screen's *decisions* are a view model in `core/` (`StatusView`); its layout
// was the part that had never run anywhere. Both halves are now testable and
// what is left on a console is one adapter per screen.
//
// The geometry constants stay with this function rather than moving to
// `screen_frame.hpp`, because a layout is the one thing M8-2 (#44) adjusts
// against a real panel and a person doing that should have one block to look at.
#pragma once

#include <cstdint>

#include "draw_list.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// Draw `view` into the bounds a `CustomDrawer` was handed.
///
/// `const` in everything but `out`: the screen's state is the view model, and a
/// layout that mutated anything would be a decision drawn in the wrong half.
void PaintStatus(const StatusView& view, const Palette& palette, DrawList& out, std::int32_t x,
                 std::int32_t y, std::int32_t width, std::int32_t height);

}  // namespace rommsync::overlay

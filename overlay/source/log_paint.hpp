// The log screen's layout, with no renderer in it (M9-6, #209).
//
// `status_paint.hpp`'s split, for the log: `log_screen.cpp` is a
// `CustomDrawer` and an adapter, and where the lines go -- newest at the
// bottom, the oldest dropped when the panel is short, every string held to the
// panel's width -- is here, where `ctest -R overlay.log` records it.
#pragma once

#include <cstdint>

#include "draw_list.hpp"
#include "rommsync/overlay_log_view.hpp"

namespace rommsync::overlay {

/// Draw `view` into the bounds a `CustomDrawer` was handed.
void PaintLog(const LogView& view, const Palette& palette, DrawList& out, std::int32_t x,
              std::int32_t y, std::int32_t width, std::int32_t height);

}  // namespace rommsync::overlay

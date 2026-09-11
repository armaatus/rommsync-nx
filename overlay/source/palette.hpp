// Where `core/`'s colour vocabulary and libultrahand's theme meet, and the only
// file in this directory that holds both.
//
// Split out of `screen_frame.hpp` by M9-7 (#198). The rest of that header -- the
// session handshake and the "not running" / "unreachable" decision -- names no
// libultrahand type and now compiles on a host, which is what lets
// `ctest -R 'overlay.(version|errors)'` assert a version mismatch end to end. A screen that
// needs a colour includes this; a screen that only needs the handshake does not.
#pragma once

#include <tesla.hpp>

#include <string>

#include "draw_list.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// The renderer's palette for a `Tone`. `core/` names no colour (hard rule 4),
/// so this is where the two vocabularies meet -- and it uses libultrahand's
/// theme variables rather than literals so a user's theme still applies.
///
/// Through `Renderer::a`, which folds in the overlay's fade animation alpha.
/// Without it the frame's chrome fades on open and close while everything a
/// screen draws stays fully opaque and pops (libtesla's own convention).
tsl::Color ColorFor(Tone tone);

/// The colour a label, a hint or a caption is drawn in -- the quiet half of
/// every row, and not a `Tone`: it is a role rather than a judgement.
tsl::Color MutedColor();

/// Every colour a frame is drawn in, as the raw RGBA4444 words a `DrawList`
/// speaks.
///
/// The type is `draw_list.hpp`'s rather than this file's, and has to be: a
/// painter takes a `Palette` and must not include a header that names
/// `tsl::Color`. This file is where one is *filled in*, which is the half that
/// needs libultrahand.
///
/// Resolved once at the top of a frame rather than per row: `Renderer::a` folds
/// in the fade-animation alpha, so each of these answers differently on every
/// frame of an open or close, and a painter that is meant to run on a host
/// cannot call it at all.
Palette CurrentPalette();

/// `drawString`, refusing a `max_width` of zero.
///
/// **Every screen in this directory computes its column widths as
/// `width > K ? width - K : 0`, meaning "no room" -- and libtesla reads
/// `drawString`'s `maxWidth = 0` as *no limit*.** Handed straight over, that is
/// the opposite of what the subtraction is for, at exactly the width where
/// bounding matters most: a 256-byte `fs_name` off a RomM library, or a
/// 512-byte `verification_url`, painted across the console.
///
/// M9-7 (#198) found it in the status screen, which by then had a layout a test
/// could reach. The other four screens have the same arithmetic and no test, so
/// the guard is here rather than repeated in each: one place to be right, and
/// `ctest -R overlay.portable` greps this directory so nothing calls
/// `drawString` around it. A screen that genuinely wants no bound has to say so
/// with a width; none of them does.
///
/// Nothing else about it differs from `drawString`. `drawRect` needs no twin --
/// libtesla already declines a non-positive width.
void DrawBounded(tsl::gfx::Renderer* renderer, const std::string& text, s32 x, s32 y,
                 s32 font_size, tsl::Color color, s32 max_width);

}  // namespace rommsync::overlay

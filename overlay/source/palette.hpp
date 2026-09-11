// Where `core/`'s colour vocabulary and libultrahand's theme meet, and the only
// file in this directory that holds both.
//
// Split out of `screen_frame.hpp` by M9-7 (#198). The rest of that header -- the
// session handshake and the "not running" / "unreachable" decision -- names no
// libultrahand type and now compiles on a host, which is what lets
// `ctest -R overlay.link` assert a version mismatch end to end. A screen that
// needs a colour includes this; a screen that only needs the handshake does not.
#pragma once

#include <tesla.hpp>

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
/// speaks (`draw_list.hpp`).
///
/// Resolved once at the top of a frame rather than per row: `Renderer::a` folds
/// in the fade-animation alpha, so each of these answers differently on every
/// frame of an open or close, and a painter that is meant to run on a host
/// cannot call it at all.
Palette CurrentPalette();

}  // namespace rommsync::overlay

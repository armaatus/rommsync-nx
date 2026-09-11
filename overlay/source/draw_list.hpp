// The half of drawing that needs no renderer: the primitives a screen asks for
// and the palette it asks for them in.
//
// M9-7 (#198). Until this existed a screen's layout was `tsl::gfx::Renderer`
// calls inline in its `Draw`, and `tsl::gfx::Renderer` is `final` with
// non-virtual inline methods -- there is no fake to pass it, so every row
// position, every clip and every bar width was unreachable from any test. The
// seam is a pure-data command and a sink: on a console `RendererDrawList`
// (`status_screen.cpp`) replays each command into the real renderer, and in a
// host test a recorder keeps them and the test asserts on what was asked for.
//
// This is the shape `masagrator/Status-Monitor-Deux` uses between its logic and
// libtesla, one step earlier than ours -- our per-screen *decisions* are already
// view models in `core/` (overlay/AGENTS.md), so what is left on this side is
// layout, and layout is what this makes testable.
//
// **Nothing here may name a libnx or libultrahand type.** That is the whole
// point: a `DrawList` compiles on a host, and `ctest -R overlay.portable` greps
// this file and its neighbours to keep it that way. Colours arrive as the
// 16-bit RGBA4444 word libtesla stores, resolved from the user's theme by the
// one file that is allowed to know about themes (`palette.hpp`).
#pragma once

#include <cstdint>
#include <string>

#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// A colour as libtesla stores one: RGBA4444 packed into 16 bits, which is what
/// `tsl::Color::rgba` is. Carried as the raw word so this header names no
/// libultrahand type.
using Rgba4444 = std::uint16_t;

/// The colours one frame is drawn in, resolved once from the user's theme.
///
/// A struct rather than calls into `ColorFor`, because those fold in the
/// overlay's fade-animation alpha and so answer differently on every frame of an
/// open or close -- which a host has no animation to run. Resolving them at the
/// top of a frame is also one theme read per frame instead of one per row.
struct Palette {
  Rgba4444 neutral = 0;
  Rgba4444 good = 0;
  Rgba4444 warn = 0;
  Rgba4444 bad = 0;

  /// The quiet half of every row -- a label, a hint, a caption. A role rather
  /// than a `Tone`.
  Rgba4444 muted = 0;

  Rgba4444 track_empty = 0;
  Rgba4444 track_full = 0;

  /// The colour a `Tone` is drawn in. The one place `core/`'s vocabulary and a
  /// palette meet, and the host-side twin of `ColorFor`.
  Rgba4444 For(Tone tone) const {
    switch (tone) {
      case Tone::kGood:
        return good;
      case Tone::kWarn:
        return warn;
      case Tone::kBad:
        return bad;
      case Tone::kNeutral:
        break;
    }
    return neutral;
  }
};

/// One primitive a screen asked for, as data.
struct DrawCommand {
  enum class Kind { kString, kRect };

  Kind kind = Kind::kString;

  /// `kString` only.
  std::string text;

  std::int32_t x = 0;
  std::int32_t y = 0;

  /// `kRect`: the rectangle's size. `kString`: `width` is the wrap limit and
  /// `height` is unused.
  std::int32_t width = 0;
  std::int32_t height = 0;

  /// `kString` only.
  std::int32_t font_size = 0;

  Rgba4444 color = 0;
};

/// Where a screen's primitives go.
///
/// Deliberately narrow: two calls, because two is what every screen in this
/// directory uses. A screen that needs a third belongs behind a third method
/// here rather than behind a renderer it reached for directly -- the moment one
/// screen draws through `tsl::gfx::Renderer` again, its layout leaves the reach
/// of every test.
class DrawList {
 public:
  virtual ~DrawList() = default;

  /// `wrap_width` is the width the text is held to. **Zero means zero**, not
  /// "unbounded": libtesla's `drawString` takes `maxWidth = 0` as no limit, and
  /// a screen computing a width that came out zero meant the opposite of that.
  /// An implementation that cannot express "no room" must draw nothing.
  virtual void String(const std::string& text, std::int32_t x, std::int32_t y,
                      std::int32_t font_size, Rgba4444 color, std::int32_t wrap_width) = 0;

  virtual void Rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                    Rgba4444 color) = 0;
};

}  // namespace rommsync::overlay

#include "palette.hpp"

#include <tesla.hpp>

#include "draw_list.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

tsl::Color ColorFor(Tone tone) {
  switch (tone) {
    case Tone::kGood:
      return tsl::gfx::Renderer::a(tsl::healthyRamTextColor);
    case Tone::kWarn:
      return tsl::gfx::Renderer::a(tsl::warningTextColor);
    case Tone::kBad:
      return tsl::gfx::Renderer::a(tsl::badRamTextColor);
    case Tone::kNeutral:
      break;
  }
  return tsl::gfx::Renderer::a(tsl::defaultTextColor);
}

tsl::Color MutedColor() { return tsl::gfx::Renderer::a(tsl::infoTextColor); }

Palette CurrentPalette() {
  Palette palette;
  palette.neutral = ColorFor(Tone::kNeutral).rgba;
  palette.good = ColorFor(Tone::kGood).rgba;
  palette.warn = ColorFor(Tone::kWarn).rgba;
  palette.bad = ColorFor(Tone::kBad).rgba;
  palette.muted = MutedColor().rgba;
  palette.track_empty = tsl::gfx::Renderer::a(tsl::trackBarEmptyColor).rgba;
  palette.track_full = tsl::gfx::Renderer::a(tsl::trackBarFullColor).rgba;
  return palette;
}

}  // namespace rommsync::overlay

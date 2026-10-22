#include "log_paint.hpp"

#include <cstddef>
#include <cstdint>

#include "draw_list.hpp"
#include "prompts.hpp"
#include "rommsync/overlay_log_view.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us, and
// in one block for `status_paint.cpp`'s reason: M8-2 (#44) adjusts it against
// a real panel.
constexpr std::int32_t kHeadlineFont = 20;
constexpr std::int32_t kBodyFont = 19;
constexpr std::int32_t kLineFont = 15;
constexpr std::int32_t kRowHeight = 26;
constexpr std::int32_t kHeadlineHeight = 28;
constexpr std::int32_t kHintHeight = 22;
constexpr std::int32_t kLineHeight = 19;
/// How far short of the drawer's right edge a line stops.
constexpr std::int32_t kInset = 8;

}  // namespace

void PaintLog(const LogView& view, const Palette& palette, DrawList& out, std::int32_t x,
              std::int32_t y, std::int32_t width, std::int32_t height) {
  // **A width of zero is "no room"** and is never handed on (`draw_list.hpp`):
  // a log line carries a save's path, which is as long as a card lets it be.
  const std::int32_t full_width = width > kInset ? width - kInset : 0;
  if (full_width <= 0) {
    return;
  }

  // The way out, reserved first: the lines are longer than any panel, and a
  // prompt drawn after them is the first thing to fall off it.
  const std::int32_t prompt = y + height - kRowHeight;
  out.String(Prompt(kGlyphB, "Back"), x, prompt, kBodyFont, palette.muted, full_width);
  const std::int32_t bottom = prompt - kRowHeight / 2;

  std::int32_t row = y;
  if (row + kHeadlineHeight > bottom) {
    return;
  }
  out.String(view.headline, x, row, kHeadlineFont, palette.For(view.tone), full_width);
  row += kHeadlineHeight;
  if (!view.hint.empty() && row + kHintHeight <= bottom) {
    out.String(view.hint, x, row, kLineFont, palette.muted, full_width);
    row += kHintHeight;
  }
  row += kLineHeight / 2;

  // **The newest lines are the ones that fit**, drawn oldest to newest down the
  // panel: the end of a log says why the last tick failed, and a panel that
  // kept the start of a tail would keep the part nobody came to read.
  if (row + kLineHeight > bottom) {
    return;
  }
  const std::size_t room = static_cast<std::size_t>((bottom - row) / kLineHeight);
  const std::size_t first = view.lines.size() > room ? view.lines.size() - room : 0;
  for (std::size_t at = first; at < view.lines.size(); ++at) {
    out.String(view.lines[at], x, row, kLineFont, palette.neutral, full_width);
    row += kLineHeight;
  }
}

}  // namespace rommsync::overlay

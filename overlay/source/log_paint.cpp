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

std::size_t PaintLog(const LogView& view, const Palette& palette, DrawList& out, std::int32_t x,
                     std::int32_t y, std::int32_t width, std::int32_t height) {
  // **A width of zero is "no room"** and is never handed on (`draw_list.hpp`):
  // a log line carries a save's path, which is as long as a card lets it be.
  const std::int32_t full_width = width > kInset ? width - kInset : 0;
  if (full_width <= 0) {
    return 0;
  }

  // The way out, reserved first: the lines are longer than any panel, and a
  // prompt drawn after them is the first thing to fall off it.
  const std::int32_t prompt = y + height - kRowHeight;
  out.String(Prompt(kGlyphB, "Back"), x, prompt, kBodyFont, palette.muted, full_width);
  std::int32_t bottom = prompt - kRowHeight / 2;
  // The bundle a bug report wants (#304), above the way out and only where the
  // sysmodule is there to write it.
  if (view.link == Link::kOk && prompt - kRowHeight >= y) {
    out.String(Prompt(kGlyphY, "Write diagnostics"), x, prompt - kRowHeight, kBodyFont,
               palette.neutral, full_width);
    bottom -= kRowHeight;
  }

  std::int32_t row = y;
  if (row + kHeadlineHeight > bottom) {
    return 0;
  }
  out.String(view.headline, x, row, kHeadlineFont, palette.For(view.tone), full_width);
  row += kHeadlineHeight;
  if (!view.hint.empty() && row + kHintHeight <= bottom) {
    out.String(view.hint, x, row, kLineFont, palette.muted, full_width);
    row += kHintHeight;
  }
  if (!view.notice.empty() && row + kHintHeight <= bottom) {
    out.String(view.notice, x, row, kLineFont, palette.For(view.notice_tone), full_width);
    row += kHintHeight;
  }
  row += kLineHeight / 2;

  // **The newest lines are the ones that fit**, drawn oldest to newest down the
  // panel: the end of a log says why the last tick failed, and a panel that
  // kept the start of a tail would keep the part nobody came to read.
  if (row + kLineHeight > bottom) {
    return 0;
  }
  const std::size_t room = static_cast<std::size_t>((bottom - row) / kLineHeight);
  // Scrolled (#304): `back` of the newest are below the window, clamped so the
  // panel stays full -- `ScrollLog`'s rule, applied again here because a poll
  // can shorten the tail under a screen that had scrolled.
  const std::size_t most = view.lines.size() > room ? view.lines.size() - room : 0;
  const std::size_t end = view.lines.size() - (view.back > most ? most : view.back);
  const std::size_t first = end > room ? end - room : 0;
  for (std::size_t at = first; at < end; ++at) {
    out.String(view.lines[at], x, row, kLineFont, palette.neutral, full_width);
    row += kLineHeight;
  }
  return room;
}

}  // namespace rommsync::overlay

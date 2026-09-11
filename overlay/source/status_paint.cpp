#include "status_paint.hpp"

#include <cstdint>

#include "draw_list.hpp"
#include "prompts.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us. Named
// rather than sprinkled through `PaintStatus`, because a layout is the one thing
// here that will be adjusted against a real panel in M8-2 (#44) and a person
// doing that should have one place to look.
constexpr std::int32_t kHeadlineFont = 23;
constexpr std::int32_t kBodyFont = 19;
constexpr std::int32_t kRowHeight = 26;
constexpr std::int32_t kHeadlineHeight = 34;
constexpr std::int32_t kHintHeight = 26;
constexpr std::int32_t kBarHeight = 12;
constexpr std::int32_t kValueColumn = 160;
/// How far short of the drawer's right edge the progress track stops.
constexpr std::int32_t kBarInset = 8;

}  // namespace

void PaintStatus(const StatusView& view, const Palette& palette, DrawList& out, std::int32_t x,
                 std::int32_t y, std::int32_t width, std::int32_t height) {
  // Nothing is drawn past the bounds `CustomDrawer` handed us. The row count is
  // bounded and the panel is not, so this only ever fires on a layout that has
  // to be adjusted in M8-2 (#44) -- but a row painted over the frame's chrome is
  // the kind of thing that reads as a corrupted overlay rather than as a
  // too-long list.
  const Rgba4444 muted = palette.muted;
  // Nothing runs off the right edge either. A value is not ours to bound:
  // `fs_name` comes off a RomM library and `ipc::kMaxNameBytes` is 256, so a
  // routine `Some Game (USA) (Rev 1) [!].gba` draws past a ~448px panel.
  //
  // **A width of zero is "no room", and it is never handed to a renderer.**
  // libtesla reads `drawString`'s `maxWidth = 0` as *no limit*, so a panel too
  // narrow for the value column used to draw every value unbounded -- the exact
  // thing the subtraction above exists to prevent, at exactly the width where it
  // matters most. The rows that have no width are dropped instead, here, where a
  // test can see it: `DrawList` takes zero as zero (`draw_list.hpp`) and
  // `RendererDrawList` refuses to pass one on.
  const std::int32_t value_width =
      width > kValueColumn + kBarInset ? width - kValueColumn - kBarInset : 0;
  const std::int32_t full_width = width > kBarInset ? width - kBarInset : 0;
  if (full_width <= 0) {
    // Not a panel at all. Drawing the headline across whatever is to the right
    // of it is worse than drawing nothing.
    return;
  }

  // The one control this screen has, drawn at the foot of the panel and
  // reserved before anything else: the rows below the headline grow with what
  // is downloading, so a prompt drawn after them is the first thing to fall off
  // a full screen -- and a control nobody can see is a menu this overlay does
  // not have (#26).
  const std::int32_t prompt = y + height - kRowHeight;
  out.String(Prompt(kGlyphY, "Settings"), x, prompt, kBodyFont, muted, full_width);
  const std::int32_t bottom = prompt - kRowHeight / 2;

  std::int32_t row = y;
  out.String(view.headline, x, row, kHeadlineFont, palette.For(view.tone), full_width);
  row += kHeadlineHeight;
  if (!view.hint.empty()) {
    out.String(view.hint, x, row, kBodyFont, muted, full_width);
    row += kHintHeight;
  }
  row += kRowHeight / 2;

  // A row is a label *and* a value, so a panel with no room for the value column
  // has no room for the row: half a row is a label with nothing beside it, which
  // reads as a value that failed to load rather than as a screen that is too
  // narrow. The rest of the screen -- headline, hint, prompt and bar -- is full
  // width and still drawn.
  if (value_width > 0) {
    for (const Line& line : view.lines) {
      if (row + kRowHeight > bottom) {
        return;
      }
      out.String(line.label, x, row, kBodyFont, muted, kValueColumn);
      out.String(line.value, x + kValueColumn, row, kBodyFont, palette.For(line.tone),
                 value_width);
      row += kRowHeight;
    }
  }

  if (view.progress.kind == Progress::Kind::kNone) {
    return;
  }
  row += kRowHeight / 2;
  if (row + kRowHeight + kBarHeight > bottom) {
    return;
  }
  out.String(view.progress.caption, x, row, kBodyFont, muted, full_width);
  row += kRowHeight;

  const std::int32_t track = full_width;
  out.Rect(x, row, track, kBarHeight, palette.track_empty);
  if (view.progress.kind == Progress::Kind::kFraction) {
    // Integer maths on the per mille the view model already clamped, so a bar
    // cannot be drawn past its own track by a server that under-declared a
    // length (`overlay_status_view.hpp`).
    const std::int32_t filled =
        static_cast<std::int32_t>(static_cast<std::int64_t>(track) * view.progress.permille / 1000);
    out.Rect(x, row, filled, kBarHeight, palette.track_full);
  }
}

}  // namespace rommsync::overlay

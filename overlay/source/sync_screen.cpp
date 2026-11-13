#include "sync_screen.hpp"

#include <string>

#include "ipc_client.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/overlay_sync_actions.hpp"
#include "screen_frame.hpp"
#include "sync_pump.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us. Named
// rather than sprinkled through `Draw` for the same reason `status_screen.cpp`
// names its own: a layout is the one thing here that will be adjusted against a
// real panel in M8-2 (#44), and a person doing that should have one place to
// look.
constexpr s32 kHeadlineFont = 23;
constexpr s32 kBodyFont = 19;
constexpr s32 kHeadlineHeight = 34;
constexpr s32 kHintHeight = 26;
constexpr s32 kRowHeight = 26;
constexpr s32 kValueColumn = 160;
/// How far short of the drawer's right edge a line stops.
constexpr s32 kInset = 8;

}  // namespace

SyncScreen::SyncScreen(IpcClient& client) : client_(client) {}

tsl::elm::Element* SyncScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer rather than a `List` of `ListItem`s: the two controls are
  // buttons rather than a selection, and a list would have to be torn down and
  // rebuilt on every poll to move one value.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void SyncScreen::update() { pump_.Frame(); }

bool SyncScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                             HidAnalogStickState) {
  if ((keys_down & HidNpadButton_A) != 0) {
    if (pump_.view().sync_now.state == ControlState::kInert) {
      // There is no sysmodule to ask. Not handled, so B still leaves the screen
      // and Tesla keeps its own bindings -- and the headline is already saying
      // why, so there is nothing to add.
      return false;
    }
    pump_.PressSyncNow();
    return true;
  }
  if ((keys_down & HidNpadButton_X) != 0) {
    if (pump_.view().toggle.state == ControlState::kInert) {
      return false;
    }
    pump_.PressToggle();
    return true;
  }
  return false;
}

void SyncScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                      s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: a refusal is a sentence rather than a word. Every string
  // goes through `DrawBounded`, because a width that came out `0` here means
  // "no room" and `drawString` would read it as "no limit" (`palette.hpp`).
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  const s32 value_width = width > kValueColumn + kInset ? width - kValueColumn - kInset : 0;
  const tsl::Color muted = MutedColor();
  const SyncActionsView& view = pump_.view();

  s32 row = y;
  // Once one element has been dropped for want of room, nothing below it is
  // drawn either. Testing each element on its own would let a short line take
  // the place of a taller one that did not fit -- the hint without its
  // headline, a remedy with no problem above it.
  bool clipped = false;
  const auto fits = [&](s32 needed) { return !clipped && row + needed <= bottom; };
  const auto line = [&](const std::string& text, s32 font, tsl::Color color, s32 advance) {
    if (text.empty()) {
      return;
    }
    if (!fits(advance)) {
      clipped = true;
      return;
    }
    DrawBounded(renderer, text, x, row, font, color, line_width);
    row += advance;
  };

  line(view.headline, kHeadlineFont, ColorFor(view.tone), kHeadlineHeight);
  line(view.hint, kBodyFont, muted, kHintHeight);

  row += kRowHeight / 2;
  for (const Line& value : view.lines) {
    if (!fits(kRowHeight)) {
      return;
    }
    DrawBounded(renderer, value.label, x, row, kBodyFont, muted, kValueColumn);
    DrawBounded(renderer, value.value, x + kValueColumn, row, kBodyFont,
                         ColorFor(value.tone), value_width);
    row += kRowHeight;
  }

  // What the last press did, above the controls rather than below them: it is
  // the answer to the thing the user just pressed, and a sentence under a
  // button reads as a caption for it.
  if (!view.notice.empty()) {
    row += kRowHeight / 2;
    line(view.notice, kBodyFont, ColorFor(view.notice_tone), kRowHeight);
  }

  // The two prompts. A control that would be *refused* is still drawn -- greyed,
  // with its sentence under it -- because a button that vanishes is a button a
  // user goes looking for. A `kInert` one is not drawn at all: there is nothing
  // on the far side to press against, and the headline is already saying so.
  const auto control = [&](const char* glyph, const Button& button) {
    if (button.state == ControlState::kInert) {
      // Its `label` is still set, so that every `Button` carries one in every
      // state (`overlay_sync_actions.hpp`); it is simply never drawn.
      return;
    }
    const tsl::Color color = button.state == ControlState::kLive
                                 ? tsl::gfx::Renderer::a(tsl::defaultTextColor)
                                 : muted;
    line(Prompt(glyph, button.label), kBodyFont, color, kRowHeight);
    line(button.refusal, kBodyFont, ColorFor(button.refusal_tone), kRowHeight);
  };

  row += kRowHeight / 2;
  control(kGlyphA, view.sync_now);
  control(kGlyphX, view.toggle);
}

}  // namespace rommsync::overlay

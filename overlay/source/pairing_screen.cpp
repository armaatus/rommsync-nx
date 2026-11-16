#include "pairing_screen.hpp"

#include <string>

#include "ipc_client.hpp"
#include "pairing_pump.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/overlay_pairing_view.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us. Named
// rather than sprinkled through `Draw` for the same reason `status_screen.cpp`
// names its own: a layout is the one thing here that will be adjusted against a
// real panel in M8-2 (#44), and a person doing that should have one place to
// look.
constexpr s32 kHeadlineFont = 23;
constexpr s32 kBodyFont = 19;
/// The code's own size. It is the one thing on this screen a person reads off a
/// television across a room, so it is drawn larger than anything else -- and
/// M8-2 is where that number meets an actual panel.
constexpr s32 kCodeFont = 44;
constexpr s32 kHeadlineHeight = 34;
constexpr s32 kHintHeight = 26;
constexpr s32 kRowHeight = 26;
constexpr s32 kCodeHeight = 58;
/// How far short of the drawer's right edge a line stops.
constexpr s32 kInset = 8;

}  // namespace

PairingScreen::PairingScreen(IpcClient& client) : client_(client) {}

tsl::elm::Element* PairingScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer rather than a `List`: there is nothing on this screen to
  // select, the one action is a button, and a list would have to be torn down
  // and rebuilt on every poll to move a countdown by a second.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void PairingScreen::update() { pump_.Frame(); }

bool PairingScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                                HidAnalogStickState) {
  if ((keys_down & HidNpadButton_A) == 0) {
    return false;
  }
  if (!pump_.view().start && !pump_.view().start_over) {
    // Nothing is offered: an attempt is in flight, or there is no sysmodule to
    // ask. Not handled, so B still leaves the screen and Tesla keeps its own
    // bindings.
    return false;
  }
  pump_.PressStart();
  return true;
}

void PairingScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                         s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: a `verification_url` is the user's own origin --
  // `ipc::kMaxServerUrlBytes` is 512, so a routine
  // `http://romm.homelab.example:8080/pair/device` draws past a ~448px panel.
  // Every string goes through `DrawBounded`, because a width that came out `0`
  // here means "no room" and `drawString` would read it as "no limit"
  // (`palette.hpp`).
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  const tsl::Color muted = MutedColor();
  const PairingView& view = pump_.view();

  s32 row = y;
  // Once one element has been dropped for want of room, nothing below it is
  // drawn either. Testing each element on its own would let a short line take
  // the place of a taller one that did not fit -- the countdown without the
  // code it counts down, which is worse than neither.
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

  if (!view.code.empty()) {
    row += kRowHeight / 2;
    // The address above the code: a user reads the two in that order, and the
    // address is the half they need before the code means anything.
    line(view.url, kBodyFont, tsl::gfx::Renderer::a(tsl::defaultTextColor), kRowHeight);
    // Verbatim, and never re-grouped -- RomM's alphabet already excludes the
    // confusable characters, so there is nothing here to help with
    // (docs/AUTH.md).
    line(view.code, kCodeFont, tsl::gfx::Renderer::a(tsl::defaultTextColor), kCodeHeight);
    line(view.countdown, kBodyFont, muted, kRowHeight);
  }

  if (!view.detail.empty()) {
    row += kRowHeight / 2;
    line(view.detail, kBodyFont, muted, kRowHeight);
  }

  // Last, and only when there is something the button does. A prompt for an
  // action the screen would decline is worse than no prompt at all.
  const std::string action = view.start ? Prompt(kGlyphA, "Pair")
                             : view.start_over ? Prompt(kGlyphA, "Start over")
                                                : std::string();
  if (!action.empty() && fits(kRowHeight * 2)) {
    row += kRowHeight / 2;
    DrawBounded(renderer, action, x, row, kBodyFont,
                         tsl::gfx::Renderer::a(tsl::defaultTextColor), line_width);
  }
}

}  // namespace rommsync::overlay

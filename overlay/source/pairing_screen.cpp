#include "pairing_screen.hpp"

#include <cstdint>
#include <string>

#include "ipc_client.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_pairing_view.hpp"
#include "rommsync/pairing.hpp"
#include "screen_frame.hpp"

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

void PairingScreen::update() { Poll(); }

bool PairingScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                                HidAnalogStickState) {
  if ((keys_down & HidNpadButton_A) == 0) {
    return false;
  }
  if (!view_.start && !view_.start_over) {
    // Nothing is offered: an attempt is in flight, or there is no sysmodule to
    // ask. Not handled, so B still leaves the screen and Tesla keeps its own
    // bindings.
    return false;
  }
  Start();
  return true;
}

bool PairingScreen::Ready() {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link link = frame_.Ready();
  if (link == Link::kOk) {
    return true;
  }
  view_ = RenderPairingUnreachable(link, frame_.sysmodule_interface());
  return false;
}

void PairingScreen::Poll() {
  if (!Ready()) {
    blocked_ = false;
    return;
  }

  auth::PairingStatus status;
  const Result rc = client_.GetPairState(&status);
  if (R_FAILED(rc)) {
    blocked_ = false;
    // A payload this build cannot read is distinct from a sysmodule that is
    // gone, and distinct from a half-parsed status: the screen says so instead
    // of drawing defaulted fields as though they were an answer. Which of the
    // two it is is `Diagnose`'s to say.
    view_ = RenderPairingUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }

  if (blocked_) {
    // The refusal stands until the user presses the button again. `StartPair`
    // refuses *before* `PairingSession::Begin()` runs (`ipc::ServiceCore`), so
    // the session still reports whatever the last attempt left behind --
    // `kIdle` on a first Pair, `kExpired` or `kDenied` on a Start over -- and
    // every one of those would redraw over the reason one frame after the
    // press. Nothing is polling behind a refusal to keep saying it, and a
    // button that visibly does nothing is what this whole screen exists to
    // avoid. `GetPairState` is still called every frame above, so a sysmodule
    // that goes away while the refusal is up is still reported.
    return;
  }
  view_ = RenderPairing(status);
}

void PairingScreen::Start() {
  blocked_ = false;
  if (!Ready()) {
    return;
  }

  auth::PairingStatus status;
  const Result rc = client_.StartPair(&status);
  if (R_SUCCEEDED(rc)) {
    // `kStarting`, almost always: the sysmodule hands the init to its own
    // thread and answers without waiting for it (`ipc::Engine::StartPairing`).
    view_ = RenderPairing(status);
    return;
  }
  if (rc == MalformedResponse()) {
    view_ = RenderPairingUnreachable(Link::kUnreadable);
    return;
  }

  // It refused, or the session died. `GetStatus` never fails, so one that does
  // is the transport -- and one that does not already says whether there is a
  // server to pair with. See the note on `Start` in the header for why the
  // `Result` itself is not decoded here.
  ipc::Status console;
  const Result status_rc = client_.GetStatus(&console);
  if (R_FAILED(status_rc)) {
    view_ = RenderPairingUnreachable(frame_.Diagnose(status_rc), frame_.sysmodule_interface());
    return;
  }
  view_ = RenderPairingBlocked(console.configured ? PairBlock::kRefused : PairBlock::kNoServer);
  blocked_ = true;
}

void PairingScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                         s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: `drawString`'s `maxWidth` defaults to "no limit", and a
  // `verification_url` is the user's own origin -- `ipc::kMaxServerUrlBytes` is
  // 512, so a routine `http://romm.homelab.example:8080/pair/device` draws past
  // a ~448px panel.
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  const tsl::Color muted = MutedColor();

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
    renderer->drawString(text, false, x, row, font, color, line_width);
    row += advance;
  };

  line(view_.headline, kHeadlineFont, ColorFor(view_.tone), kHeadlineHeight);
  line(view_.hint, kBodyFont, muted, kHintHeight);

  if (!view_.code.empty()) {
    row += kRowHeight / 2;
    // The address above the code: a user reads the two in that order, and the
    // address is the half they need before the code means anything.
    line(view_.url, kBodyFont, tsl::gfx::Renderer::a(tsl::defaultTextColor), kRowHeight);
    // Verbatim, and never re-grouped -- RomM's alphabet already excludes the
    // confusable characters, so there is nothing here to help with
    // (docs/AUTH.md).
    line(view_.code, kCodeFont, tsl::gfx::Renderer::a(tsl::defaultTextColor), kCodeHeight);
    line(view_.countdown, kBodyFont, muted, kRowHeight);
  }

  if (!view_.detail.empty()) {
    row += kRowHeight / 2;
    line(view_.detail, kBodyFont, muted, kRowHeight);
  }

  // Last, and only when there is something the button does. A prompt for an
  // action the screen would decline is worse than no prompt at all.
  const std::string action = view_.start ? Prompt(kGlyphA, "Pair")
                             : view_.start_over ? Prompt(kGlyphA, "Start over")
                                                : std::string();
  if (!action.empty() && fits(kRowHeight * 2)) {
    row += kRowHeight / 2;
    renderer->drawString(action, false, x, row, kBodyFont,
                         tsl::gfx::Renderer::a(tsl::defaultTextColor), line_width);
  }
}

}  // namespace rommsync::overlay

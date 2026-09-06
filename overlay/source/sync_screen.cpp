#include "sync_screen.hpp"

#include <string>

#include "ipc_client.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_sync_actions.hpp"
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
constexpr s32 kHeadlineHeight = 34;
constexpr s32 kHintHeight = 26;
constexpr s32 kRowHeight = 26;
constexpr s32 kValueColumn = 160;
/// How far short of the drawer's right edge a line stops.
constexpr s32 kInset = 8;

/// The button glyphs libtesla draws from the Switch's own font, the same way
/// `pairing_screen.cpp` names its one.
constexpr const char* kGlyphA = "\uE0E0";
constexpr const char* kGlyphX = "\uE0E2";

/// A control's prompt: the glyph, two spaces, and what pressing it does.
std::string Prompt(const char* glyph, const std::string& label) {
  return std::string(glyph) + "  " + label;
}

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

void SyncScreen::update() { Poll(); }

bool SyncScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                             HidAnalogStickState) {
  if ((keys_down & HidNpadButton_A) != 0) {
    if (view_.sync_now.state == ControlState::kInert) {
      // There is no sysmodule to ask. Not handled, so B still leaves the screen
      // and Tesla keeps its own bindings -- and the headline is already saying
      // why, so there is nothing to add.
      return false;
    }
    PressSyncNow();
    return true;
  }
  if ((keys_down & HidNpadButton_X) != 0) {
    if (view_.toggle.state == ControlState::kInert) {
      return false;
    }
    PressToggle();
    return true;
  }
  return false;
}

void SyncScreen::Poll() {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    // Dropped rather than kept: `last_` is the answer to a press on a session
    // that is now gone, and a notice about it over "sys-rommsync is not
    // running" reads as two problems.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(link, frame_.sysmodule_interface());
    return;
  }

  ipc::Status status;
  const Result rc = client_.GetStatus(&status);
  if (R_FAILED(rc)) {
    // `GetStatus` is documented never to fail, so a failure is the transport or
    // a payload this build cannot read -- and which of the two is `Diagnose`'s
    // to say, not this screen's.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }

  status_ = status;
  // The one thing a `Status` cannot say on its own, observed here because it is
  // a transition rather than a state: a tick that has not started yet and one
  // that has finished both report `sync_in_progress == false`, so "Sync
  // started" would come back when the sync ended. What to *do* about it is
  // still the view model's (`overlay_sync_actions.hpp`); this only records that
  // it happened.
  if (last_.kind == LastCommand::Kind::kSyncNow &&
      last_.sync == ipc::SyncOutcome::kAccepted && status_.sync_in_progress) {
    last_.sync_seen_running = true;
  }
  Refresh();
}

void SyncScreen::PressSyncNow() {
  if (view_.sync_now.state == ControlState::kBlocked) {
    // Refused here rather than sent to be refused there: the screen already
    // knows which refusal it would get, and `overlay.sync_actions` holds that
    // prediction against `ipc::ServiceCore::SyncNow` itself.
    //
    // Nothing is written to `last_`, because the sentence is already on screen
    // and was before the press -- `Draw` paints `sync_now.refusal` under the
    // greyed prompt whenever the button is blocked. Recording it as a notice
    // too would draw the same words twice. This is not the silent no-op #24
    // forbids; the refusal is permanent rather than momentary.
    return;
  }

  ipc::SyncOutcome outcome = ipc::SyncOutcome::kAccepted;
  const Result rc = client_.SyncNow(&outcome);
  if (R_FAILED(rc)) {
    // Dropped, not left: the previous press's answer would otherwise survive
    // the failure and be put back by the next successful poll, describing a
    // command two actions ago.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }
  // Whatever it says, including `kAccepted`: the command returns as soon as the
  // tick is queued, so the next `Status` may not have `sync_in_progress` set
  // yet, and a press with nothing on screen for two frames is a press a user
  // repeats.
  last_ = LastCommand::SyncNow(outcome);
  Refresh();
}

void SyncScreen::PressToggle() {
  // The view model never blocks this control -- `SetEnabled` refuses no
  // boolean, so there is no refusal to predict (`overlay_sync_actions.hpp`) --
  // and `kInert` was handled by the caller. Anything else is live.
  ipc::EnabledResult result;
  const Result rc = client_.SetEnabled(!view_.enabled, &result);
  if (R_FAILED(rc)) {
    // A failing `Result` means the call did not happen. A refused *write* is a
    // successful call carrying `kWriteFailed`, which is the whole reason the
    // outcome rides in the payload (`ipc::WriteOutcome`). `last_` goes for the
    // reason it goes in `PressSyncNow`.
    last_ = LastCommand{};
    view_ = RenderSyncActionsUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }

  // The state that took, read back off the new configuration -- never the state
  // that was asked for. Written into `status_` so the switch moves on this
  // frame rather than on the next poll; the next `GetStatus` overwrites it with
  // the same fact.
  status_.enabled = result.enabled;
  last_ = LastCommand::SetEnabled(result.outcome, status_.enabled);
  Refresh();
}

void SyncScreen::Refresh() { view_ = RenderSyncActions(status_, last_); }

void SyncScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                      s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: `drawString`'s `maxWidth` defaults to "no limit", and a
  // refusal is a sentence rather than a word.
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  const s32 value_width = width > kValueColumn + kInset ? width - kValueColumn - kInset : 0;
  const tsl::Color muted = MutedColor();

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
    renderer->drawString(text, false, x, row, font, color, line_width);
    row += advance;
  };

  line(view_.headline, kHeadlineFont, ColorFor(view_.tone), kHeadlineHeight);
  line(view_.hint, kBodyFont, muted, kHintHeight);

  row += kRowHeight / 2;
  for (const Line& value : view_.lines) {
    if (!fits(kRowHeight)) {
      return;
    }
    renderer->drawString(value.label, false, x, row, kBodyFont, muted, kValueColumn);
    renderer->drawString(value.value, false, x + kValueColumn, row, kBodyFont,
                         ColorFor(value.tone), value_width);
    row += kRowHeight;
  }

  // What the last press did, above the controls rather than below them: it is
  // the answer to the thing the user just pressed, and a sentence under a
  // button reads as a caption for it.
  if (!view_.notice.empty()) {
    row += kRowHeight / 2;
    line(view_.notice, kBodyFont, ColorFor(view_.notice_tone), kRowHeight);
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
  control(kGlyphA, view_.sync_now);
  control(kGlyphX, view_.toggle);
}

}  // namespace rommsync::overlay

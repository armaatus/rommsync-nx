#include "log_screen.hpp"

#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "log_paint.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "rommsync/overlay_scroll.hpp"
#include "screen_frame.hpp"
#include "scroll_keys.hpp"

namespace rommsync::overlay {
namespace {

/// Frames between two `GetLog`s: about a second at 60 Hz. See `update()`.
constexpr int kPollFrames = 60;

}  // namespace

LogScreen::LogScreen(IpcClient& client) : client_(client), cadence_(kPollFrames) {}

tsl::elm::Element* LogScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer, for the status screen's reason: nothing on it is
  // selectable, and the lines change under it on every poll.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void LogScreen::update() {
  if (cadence_.Due()) {
    Poll();
  }
}

bool LogScreen::handleInput(u64 keys_down, u64 keys_held, const HidTouchState&,
                            HidAnalogStickState, HidAnalogStickState) {
  // Every frame, so a key released while the link was down starts again from
  // one row (#298).
  const ScrollKey key = ScrollKeyOf(keys_down, keys_held);
  const int rows = repeat_.Update(key, ScrollRepeat::Clock::now());
  if (view_.link != Link::kOk) {
    return false;
  }
  if (key != ScrollKey::kNone) {
    if (rows != 0) {
      back_ = ScrollLog(back_, rows, view_.lines.size(), room_);
      view_.back = back_;
    }
    return true;
  }
  if ((keys_down & HidNpadButton_Y) != 0) {
    WriteDiagnostics();
    return true;
  }
  return false;
}

void LogScreen::WriteDiagnostics() {
  ipc::DiagnosticsResult result;
  const Result rc = client_.WriteDiagnostics(&result);
  ipc::Error refusal = ipc::Error::kOk;
  DiagnosticsOutcome outcome = DiagnosticsOutcome::kNoAnswer;
  if (R_SUCCEEDED(rc)) {
    outcome = DiagnosticsOutcome::kWritten;
  } else if (DecodeError(rc, &refusal) && refusal == ipc::Error::kWriteFailed) {
    outcome = DiagnosticsOutcome::kWriteFailed;
  } else if (DecodeError(rc, &refusal) && refusal == ipc::Error::kUnknownCommand) {
    outcome = DiagnosticsOutcome::kOutdated;
  }
  notice_ = RenderDiagnostics(outcome, result.sd_path);
  view_.notice = notice_.text;
  view_.notice_tone = notice_.tone;
}

void LogScreen::Poll() {
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    view_ = RenderLogUnreachable(link, frame_.sysmodule_interface());
    return;
  }
  ipc::LogTail tail;
  const Result rc = client_.GetLog(ipc::kMaxLogLines, &tail);
  if (R_FAILED(rc)) {
    // `GetLog` is documented never to fail, so a failure is the transport or a
    // payload this build cannot read, and which is `Diagnose`'s to say.
    view_ = RenderLogUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }
  view_ = RenderLog(tail);
  view_.back = back_;
  view_.notice = notice_.text;
  view_.notice_tone = notice_.tone;
}

void LogScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const {
  RendererDrawList out(renderer);
  room_ = PaintLog(view_, CurrentPalette(), out, x, y, width, height);
}

}  // namespace rommsync::overlay

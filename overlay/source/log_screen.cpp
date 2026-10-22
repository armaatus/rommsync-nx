#include "log_screen.hpp"

#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "log_paint.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "screen_frame.hpp"

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
}

void LogScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const {
  RendererDrawList out(renderer);
  PaintLog(view_, CurrentPalette(), out, x, y, width, height);
}

}  // namespace rommsync::overlay

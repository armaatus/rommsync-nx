#include "log_screen.hpp"

#include "ipc_client.hpp"
#include "log_paint.hpp"
#include "log_pump.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "rommsync/overlay_scroll.hpp"
#include "scroll_keys.hpp"

namespace rommsync::overlay {

LogScreen::LogScreen(IpcClient& client) : pump_(client) {}

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

void LogScreen::update() { pump_.Frame(); }

bool LogScreen::handleInput(u64 keys_down, u64 keys_held, const HidTouchState&,
                            HidAnalogStickState, HidAnalogStickState) {
  // Every frame, so a key released while the link was down starts again from
  // one row (#298).
  const ScrollKey key = ScrollKeyOf(keys_down, keys_held);
  const int rows = repeat_.Update(key, ScrollRepeat::Clock::now());
  if (pump_.view().link != Link::kOk) {
    return false;
  }
  if (key != ScrollKey::kNone) {
    if (rows != 0) {
      pump_.Scroll(rows, room_);
    }
    return true;
  }
  if ((keys_down & HidNpadButton_Y) != 0) {
    // Answered on a later frame (`log_pump.hpp`, #329).
    pump_.PressDiagnostics();
    return true;
  }
  return false;
}

void LogScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const {
  RendererDrawList out(renderer);
  room_ = PaintLog(pump_.view(), CurrentPalette(), out, x, y, width, height);
}

}  // namespace rommsync::overlay

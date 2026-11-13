#include "status_screen.hpp"

#include "draw_list.hpp"
#include "ipc_client.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "settings_screen.hpp"
#include "status_paint.hpp"
#include "status_pump.hpp"

namespace rommsync::overlay {

StatusScreen::StatusScreen(IpcClient& client) : client_(client) {}

tsl::elm::Element* StatusScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer rather than a `List` of `ListItem`s: the screen has nothing
  // to select, and a list would have to be torn down and rebuilt on every poll
  // to change a value. Nothing here is interactive, which is what
  // `CustomDrawer` is for.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void StatusScreen::update() { pump_.Frame(); }

bool StatusScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                               HidAnalogStickState) {
  if ((keys_down & HidNpadButton_Y) == 0) {
    return false;
  }
  // Offered whatever the link is doing. The settings screen draws the same
  // "sys-rommsync is not running" this one is drawing, and a way in that
  // disappeared with the sysmodule would be a menu a user cannot reach on the
  // console that most needs reading.
  tsl::changeTo<SettingsScreen>(client_);
  return true;
}

void StatusScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                        s32 height) const {
  RendererDrawList out(renderer);
  PaintStatus(pump_.view(), CurrentPalette(), out, x, y, width, height);
}

}  // namespace rommsync::overlay

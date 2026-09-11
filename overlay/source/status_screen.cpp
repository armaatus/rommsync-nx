#include "status_screen.hpp"

#include <cstdint>
#include <ctime>
#include <string>

#include "card_probe.hpp"
#include "draw_list.hpp"
#include "ipc_client.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"
#include "settings_screen.hpp"
#include "status_paint.hpp"

namespace rommsync::overlay {
namespace {

/// `PaintStatus`'s commands, replayed into the real renderer.
///
/// The whole of what is left on this side of the seam: no geometry, no
/// conditionals, nothing a test would want to reach (`draw_list.hpp`). Adding
/// anything here is moving layout back out of `status_paint.cpp`, where it can
/// be asserted, and into the half that has never run.
class RendererDrawList : public DrawList {
 public:
  explicit RendererDrawList(tsl::gfx::Renderer* renderer) : renderer_(renderer) {}

  /// Through `DrawBounded`, which is what makes `DrawList`'s "zero means zero"
  /// true on a console: libtesla reads `drawString`'s `maxWidth = 0` as *no
  /// limit* (`palette.hpp`). Every other screen goes through the same function.
  void String(const std::string& text, std::int32_t x, std::int32_t y, std::int32_t font_size,
              Rgba4444 color, std::int32_t wrap_width) override {
    DrawBounded(renderer_, text, static_cast<s32>(x), static_cast<s32>(y),
                static_cast<s32>(font_size), tsl::Color(color),
                static_cast<s32>(wrap_width));
  }

  void Rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
            Rgba4444 color) override {
    renderer_->drawRect(static_cast<s32>(x), static_cast<s32>(y), static_cast<s32>(width),
                        static_cast<s32>(height), tsl::Color(color));
  }

 private:
  tsl::gfx::Renderer* renderer_;
};

/// Polls between two looks at the card, while the sysmodule is not answering.
///
/// `update()` runs once a frame, and three `stat`s a frame on an SD card is a
/// cost for a screen that is not going to change until the user leaves the
/// overlay and turns the sysmodule on. Sixty polls is about a second, which is
/// faster than they can do that.
constexpr int kPollsBetweenProbes = 60;

}  // namespace

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

void StatusScreen::update() { Poll(); }

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

void StatusScreen::Poll() {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    view_ = RenderUnreachable(link, CardThisPoll(link), frame_.sysmodule_interface());
    return;
  }

  ipc::Status status;
  const Result rc = client_.GetStatus(&status);
  if (R_SUCCEEDED(rc)) {
    // `std::time` rather than a tick count: `Status::last_sync_at` is whole Unix
    // seconds off the sysmodule's clock, and a relative time is only meaningful
    // against the same one.
    view_ = Render(status, static_cast<std::int64_t>(std::time(nullptr)));
    return;
  }

  // `GetStatus` is documented never to fail, so a failure is the transport or a
  // payload this build cannot read -- and which of the two is `Diagnose`'s to
  // say, not this screen's.
  const Link diagnosed = frame_.Diagnose(rc);
  view_ = RenderUnreachable(diagnosed, CardThisPoll(diagnosed), frame_.sysmodule_interface());
}

const CardState& StatusScreen::CardThisPoll(Link link) {
  if (link != Link::kNotRunning) {
    // Three `stat`s a second for a value the renderer throws away. The other two
    // link states are answered by the session rather than by the card
    // (`card_probe.hpp`), so this hands back whatever was last read and looks at
    // nothing.
    return card_;
  }
  // Re-read on the first look and then only every `kPollsBetweenProbes` polls: the card
  // is the one thing here a *user* changes while this screen is up -- they leave
  // for ovl-sysmodules, turn the toggle on, and come back -- so it cannot be
  // read once and kept, and it must not be read every frame either.
  if (probe_countdown_ <= 0) {
    card_ = ProbeCard();
    probe_countdown_ = kPollsBetweenProbes;
  }
  --probe_countdown_;
  return card_;
}

void StatusScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                        s32 height) const {
  RendererDrawList out(renderer);
  PaintStatus(view_, CurrentPalette(), out, x, y, width, height);
}

}  // namespace rommsync::overlay

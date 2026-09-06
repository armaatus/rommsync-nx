#include "status_screen.hpp"

#include <ctime>
#include <string>

#include "ipc_client.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"
#include "settings_screen.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us. Named
// rather than sprinkled through `Draw`, because a layout is the one thing here
// that will be adjusted against a real panel in M8-2 (#44) and a person doing
// that should have one place to look.
constexpr s32 kHeadlineFont = 23;
constexpr s32 kBodyFont = 19;
constexpr s32 kRowHeight = 26;
constexpr s32 kHeadlineHeight = 34;
constexpr s32 kHintHeight = 26;
constexpr s32 kBarHeight = 12;
constexpr s32 kValueColumn = 160;
/// How far short of the drawer's right edge the progress track stops.
constexpr s32 kBarInset = 8;

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
    view_ = RenderUnreachable(link, frame_.sysmodule_interface());
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
  view_ = RenderUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
}

void StatusScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                        s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us. The row count is
  // bounded and the panel is not, so this only ever fires on a layout that has
  // to be adjusted in M8-2 (#44) -- but a row painted over the frame's chrome is
  // the kind of thing that reads as a corrupted overlay rather than as a
  // too-long list.
  const tsl::Color muted = MutedColor();
  // Nothing runs off the right edge either. `drawString`'s `maxWidth` defaults
  // to "no limit", and a value is not ours to bound: `fs_name` comes off a RomM
  // library and `ipc::kMaxNameBytes` is 256, so a routine
  // `Some Game (USA) (Rev 1) [!].gba` draws past a ~448px panel.
  const s32 value_width = width > kValueColumn + kBarInset ? width - kValueColumn - kBarInset : 0;
  const s32 full_width = width > kBarInset ? width - kBarInset : 0;

  // The one control this screen has, drawn at the foot of the panel and
  // reserved before anything else: the rows below the headline grow with what
  // is downloading, so a prompt drawn after them is the first thing to fall off
  // a full screen -- and a control nobody can see is a menu this overlay does
  // not have (#26).
  const s32 prompt = y + height - kRowHeight;
  renderer->drawString(Prompt(kGlyphY, "Settings"), false, x, prompt, kBodyFont, muted,
                       full_width);
  const s32 bottom = prompt - kRowHeight / 2;

  s32 row = y;
  renderer->drawString(view_.headline, false, x, row, kHeadlineFont, ColorFor(view_.tone),
                       full_width);
  row += kHeadlineHeight;
  if (!view_.hint.empty()) {
    renderer->drawString(view_.hint, false, x, row, kBodyFont, muted, full_width);
    row += kHintHeight;
  }
  row += kRowHeight / 2;

  for (const Line& line : view_.lines) {
    if (row + kRowHeight > bottom) {
      return;
    }
    renderer->drawString(line.label, false, x, row, kBodyFont, muted, kValueColumn);
    renderer->drawString(line.value, false, x + kValueColumn, row, kBodyFont,
                         ColorFor(line.tone), value_width);
    row += kRowHeight;
  }

  if (view_.progress.kind == Progress::Kind::kNone) {
    return;
  }
  row += kRowHeight / 2;
  if (row + kRowHeight + kBarHeight > bottom) {
    return;
  }
  renderer->drawString(view_.progress.caption, false, x, row, kBodyFont, muted, full_width);
  row += kRowHeight;

  const s32 track = full_width;
  renderer->drawRect(x, row, track, kBarHeight,
                     tsl::gfx::Renderer::a(tsl::trackBarEmptyColor));
  if (view_.progress.kind == Progress::Kind::kFraction) {
    // Integer maths on the per mille the view model already clamped, so a bar
    // cannot be drawn past its own track by a server that under-declared a
    // length (`overlay_status_view.hpp`).
    const s32 filled = static_cast<s32>(static_cast<std::int64_t>(track) *
                                        view_.progress.permille / 1000);
    renderer->drawRect(x, row, filled, kBarHeight,
                       tsl::gfx::Renderer::a(tsl::trackBarFullColor));
  }
}

}  // namespace rommsync::overlay

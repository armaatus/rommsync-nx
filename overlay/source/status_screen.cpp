#include "status_screen.hpp"

#include <ctime>
#include <string>

#include "ipc_client.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

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

/// The renderer's palette for a `Tone`. `core/` names no colour (hard rule 4),
/// so this is the only place the two vocabularies meet -- and it uses
/// libultrahand's theme variables rather than literals so a user's theme still
/// applies.
///
/// Through `Renderer::a`, which folds in the overlay's fade animation alpha.
/// Without it the frame's chrome fades on open and close while everything this
/// file draws stays fully opaque and pops (libtesla's own convention).
tsl::Color ColorFor(Tone tone) {
  switch (tone) {
    case Tone::kGood:
      return tsl::gfx::Renderer::a(tsl::healthyRamTextColor);
    case Tone::kWarn:
      return tsl::gfx::Renderer::a(tsl::warningTextColor);
    case Tone::kBad:
      return tsl::gfx::Renderer::a(tsl::badRamTextColor);
    case Tone::kNeutral:
      break;
  }
  return tsl::gfx::Renderer::a(tsl::defaultTextColor);
}

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

void StatusScreen::Poll() {
  if (!client_.open()) {
    if (R_FAILED(client_.Open())) {
      // No `rommsync` port. Not installed, not enabled, or it aborted at boot --
      // and it is the state a user who forgot to switch it on is in, so it is
      // drawn as a screen rather than reported as an error.
      view_ = RenderUnreachable(Link::kNotRunning);
      return;
    }
    version_checked_ = false;
  }

  if (!version_checked_) {
    // Command 0 first, always: its encoding is frozen, so it is the only call
    // that is safe to make before knowing whether this build can decode the
    // others (`ipc::Command`). A mismatch is "update the sysmodule", not a
    // decode failure, and telling those apart is the whole reason it exists.
    std::uint32_t sysmodule_interface = 0;
    const Result version_rc = client_.GetInterfaceVersion(&sysmodule_interface);
    if (R_FAILED(version_rc)) {
      // Not "not running": the port answered, so something is there. Which of
      // the two sentences is true is decided the same way a failed `GetStatus`
      // decides it, below -- by whether the port is still openable.
      view_ = Reopen();
      return;
    }
    if (sysmodule_interface != ipc::kVersion) {
      // Left unchecked, so this is re-derived on every frame rather than
      // latched: a user who exits the overlay, updates the sysmodule and comes
      // back gets a working screen without rebooting.
      view_ = RenderUnreachable(Link::kIncompatible, sysmodule_interface);
      return;
    }
    version_checked_ = true;
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

  if (rc == MalformedResponse()) {
    // It answered, and this build could not read the answer. Distinct from a
    // sysmodule that is gone, and distinct from a half-parsed `Status`: the
    // screen says so instead of drawing defaulted fields as though they were
    // numbers (`overlay_status_view.hpp`).
    view_ = RenderUnreachable(Link::kUnreadable);
    return;
  }

  // Anything else is the transport, not the command -- `GetStatus` is
  // documented never to fail.
  view_ = Reopen();
}

StatusView StatusScreen::Reopen() {
  // The session is not trusted after a transport failure, so it is dropped and
  // re-opened here rather than on the next frame: whether the port is still
  // there is exactly what decides which of the two sentences the user gets, and
  // deferring it would draw one of them for a frame on no evidence.
  client_.Close();
  version_checked_ = false;
  return R_SUCCEEDED(client_.Open()) ? RenderUnreachable(Link::kUnreadable)
                                     : RenderUnreachable(Link::kNotRunning);
}

void StatusScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                        s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us. The row count is
  // bounded and the panel is not, so this only ever fires on a layout that has
  // to be adjusted in M8-2 (#44) -- but a row painted over the frame's chrome is
  // the kind of thing that reads as a corrupted overlay rather than as a
  // too-long list.
  const s32 bottom = y + height;
  const tsl::Color muted = tsl::gfx::Renderer::a(tsl::infoTextColor);
  // Nothing runs off the right edge either. `drawString`'s `maxWidth` defaults
  // to "no limit", and a value is not ours to bound: `fs_name` comes off a RomM
  // library and `ipc::kMaxNameBytes` is 256, so a routine
  // `Some Game (USA) (Rev 1) [!].gba` draws past a ~448px panel.
  const s32 value_width = width > kValueColumn + kBarInset ? width - kValueColumn - kBarInset : 0;
  const s32 full_width = width > kBarInset ? width - kBarInset : 0;

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

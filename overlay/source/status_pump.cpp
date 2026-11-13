#include "status_pump.hpp"

#include <cstdint>
#include <ctime>

#include "card_probe.hpp"
#include "ipc_client.hpp"
#include "lifecycle.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

/// Polls between two looks at the card, while the sysmodule is not answering.
///
/// Three `stat`s a poll on an SD card is a cost for a screen that is not going
/// to change until the user leaves the overlay and turns the sysmodule on.
/// Sixty polls is about a second, which is faster than they can do that.
constexpr int kPollsBetweenProbes = 60;

}  // namespace

StatusPump::StatusPump(IpcClient& client, Probe probe)
    : client_(client), probe_(probe), probe_cadence_(kPollsBetweenProbes) {}

void StatusPump::Frame() {
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

const CardState& StatusPump::CardThisPoll(Link link) {
  if (link != Link::kNotRunning) {
    // Three `stat`s for a value the renderer throws away. The other link states
    // are answered by the session rather than by the card (`card_probe.hpp`),
    // so this hands back whatever was last read and looks at nothing.
    return card_;
  }
  // Re-read on the first look and then only every `kPollsBetweenProbes` polls:
  // the card is the one thing here a *user* changes while this screen is up --
  // they leave for ovl-sysmodules, turn the toggle on, and come back -- so it
  // cannot be read once and kept, and it must not be read every frame either.
  // And on the first poll of every showing, which a count of this screen's own
  // would miss: nothing counts while the overlay is hidden (`lifecycle.hpp`).
  if (probe_cadence_.Due() && probe_ != nullptr) {
    card_ = probe_();
  }
  return card_;
}

}  // namespace rommsync::overlay

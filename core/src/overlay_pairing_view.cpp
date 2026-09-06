#include "rommsync/overlay_pairing_view.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "rommsync/overlay_status_view.hpp"
#include "rommsync/pairing.hpp"

namespace rommsync::overlay {
namespace {

using namespace std::chrono_literals;

/// `message` when the session left one, and a sentence of our own when it did
/// not. Never both, and never neither: a terminal state with a blank line under
/// its headline reads as a screen that failed to load the reason.
std::string Detail(std::string message, const char* fallback) {
  return message.empty() ? std::string(fallback) : message;
}

/// A `kPending` that cannot be drawn as a live code, and why.
enum class Unusable {
  kNo,  ///< there is a code, an address, and time left to type them

  /// The countdown reached zero. Not terminal -- the sysmodule's next `Poll()`
  /// moves it to `kExpired` -- but the code is already spent, and the seconds
  /// between the two are exactly when a user would start retyping it.
  kSpent,

  /// A code with no address to type it into, or an address with no code.
  ///
  /// Reachable, and not by a bug: `ipc::ServiceCore::Bounded` withholds a
  /// `verification_url` longer than `ipc::kMaxVerificationUrlBytes` and leaves
  /// the rest of the status alone, because `GetPairState` is documented never
  /// to fail. What comes across is a live code and a blank address -- which
  /// drawn as a live code is the one screen state a user cannot act on.
  kUnactionable,
};

Unusable WhyUnusable(const auth::PairingStatus& status) {
  if (status.expires_in <= 0s) {
    return Unusable::kSpent;
  }
  if (status.user_code.empty() || status.verification_url.empty()) {
    return Unusable::kUnactionable;
  }
  return Unusable::kNo;
}

}  // namespace

const char* ToString(PairBlock block) {
  switch (block) {
    case PairBlock::kNoServer:
      return "no_server";
    case PairBlock::kRefused:
      return "refused";
  }
  return "unknown";
}

std::string FormatCountdown(std::chrono::seconds remaining) {
  const std::int64_t seconds = remaining.count() > 0 ? remaining.count() : 0;
  const std::int64_t left = seconds % 60;
  // Minutes are not wrapped at sixty. A ten-minute code never reaches an hour,
  // and "1:05:00" would be a format this screen has no second place to show.
  return std::to_string(seconds / 60) + ":" + (left < 10 ? "0" : "") + std::to_string(left);
}

PairingView RenderPairing(auth::PairingStatus status) {
  PairingView view;
  view.state = status.state;

  switch (status.state) {
    case auth::PairingState::kIdle:
      view.headline = "Not paired";
      view.hint = "Press Pair to get a code";
      view.tone = Tone::kWarn;
      view.start = true;
      return view;

    case auth::PairingState::kStarting:
      // Not "Not paired": `Begin()`'s request may take the whole
      // `request_timeout`, and a screen still saying nothing has started is
      // indistinguishable from a button that did not register (pairing.hpp).
      view.headline = "Contacting the server";
      view.hint = "Getting a code for this console";
      view.tone = Tone::kNeutral;
      return view;

    case auth::PairingState::kPending: {
      const Unusable why = WhyUnusable(status);
      if (why == Unusable::kSpent) {
        break;  // drawn as expired, below
      }
      if (why == Unusable::kUnactionable) {
        // Its own sentence rather than "expired": the code has time left on it
        // and starting over is still worth a try, but there is nothing on this
        // screen a user can act on, and saying so names what the server did.
        view.headline = "Cannot show the pairing address";
        view.hint = "Press Start over to try again";
        view.detail = Detail(std::move(status.message),
                             "The server answered an address this console cannot show");
        view.tone = Tone::kBad;
        view.start_over = true;
        return view;
      }
      view.headline = "Pair this console";
      view.hint = "Go to this address on a phone or computer and enter the code";
      view.tone = Tone::kNeutral;
      // Verbatim. RomM's alphabet already excludes the confusable characters,
      // so there is nothing to group and nothing to correct (docs/AUTH.md).
      view.code = std::move(status.user_code);
      view.url = std::move(status.verification_url);
      view.qr_payload = std::move(status.verification_url_complete);
      view.countdown = "Expires in " + FormatCountdown(status.expires_in);
      // Empty while the polls are going normally; the reason the countdown
      // looks stuck when they are not.
      view.detail = std::move(status.message);
      return view;
    }

    case auth::PairingState::kApproved:
      view.headline = "Paired";
      view.detail = "This console can now sync";
      view.tone = Tone::kGood;
      return view;

    case auth::PairingState::kDenied:
      view.headline = "Pairing was refused";
      view.hint = "Press Start over and approve the new code";
      view.detail = Detail(std::move(status.message), "The code was refused in the web interface");
      view.tone = Tone::kBad;
      view.start_over = true;
      return view;

    case auth::PairingState::kExpired:
      break;  // one branch, shared with a pending code that ran out

    case auth::PairingState::kFailed:
      view.headline = "Pairing failed";
      // The one state worth a bug report, and the only hint that says so:
      // denied and expired are things the user can fix by starting over, and
      // three sentences that all read "report this" is how the one that
      // matters stops being read (docs/AUTH.md).
      view.hint = "Press Start over to try again; if it keeps happening, report it";
      view.detail = Detail(std::move(status.message), "The server did not complete the pairing");
      view.tone = Tone::kBad;
      view.start_over = true;
      return view;
  }

  // `kExpired`, and the `kPending` that has run out of time. Both draw as
  // expired, and both offer the only thing left to do.
  view.state = auth::PairingState::kExpired;
  view.headline = "The code expired";
  view.hint = "Press Start over for a new code";
  view.detail = Detail(std::move(status.message), "Nobody entered the code before it ran out");
  view.tone = Tone::kWarn;
  view.start_over = true;
  return view;
}

PairingView RenderPairingUnreachable(Link link, std::uint32_t sysmodule_interface) {
  // The status screen's own wording, taken rather than restated: it is the same
  // diagnosis about the same sysmodule, and two screens disagreeing about which
  // sentence a missing service gets is the kind of thing only a console shows.
  const StatusView status = RenderUnreachable(link, sysmodule_interface);
  PairingView view;
  // `kIdle` is the state of a pairing nobody could ask about. Nothing is
  // offered with it: neither Pair nor Start over can reach a sysmodule that is
  // not answering.
  view.state = auth::PairingState::kIdle;
  view.headline = status.headline;
  view.hint = status.hint;
  view.tone = status.tone;
  return view;
}

PairingView RenderPairingBlocked(PairBlock block) {
  PairingView view;
  view.state = auth::PairingState::kIdle;
  switch (block) {
    case PairBlock::kNoServer:
      // The status screen's sentence for the same console, for the same reason
      // `RenderPairingUnreachable` borrows its wording.
      view.headline = "No server set";
      // Pair is offered, and the hint puts it second. A refusal is latched by
      // the screen -- nothing polls behind one to keep saying it -- so a state
      // with no action left is a screen that stays wrong after the user has
      // fixed `config.ini`, until they close the overlay and open it again.
      // Asking again is the one thing that can be right here, even though the
      // answer is the same until the file changes.
      view.hint = "Set server.url in config.ini, then press Pair";
      view.tone = Tone::kWarn;
      view.start = true;
      return view;
    case PairBlock::kRefused:
      view.headline = "Could not start pairing";
      view.hint = "Press Pair to try again";
      view.detail = "The sysmodule would not begin a pairing attempt";
      view.tone = Tone::kBad;
      view.start = true;
      return view;
  }
  // Not reachable through the enum, and not an abort: a screen with the wrong
  // sentence on it is recoverable on a console with no debugger, and a crash
  // taking the overlay down is not.
  view.headline = "Could not start pairing";
  view.tone = Tone::kBad;
  view.start = true;
  return view;
}

}  // namespace rommsync::overlay

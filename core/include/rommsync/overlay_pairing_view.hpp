// The overlay's pairing screen, decided here and drawn over there.
//
// The same split as `overlay_status_view.hpp`, for the same reason: what the
// screen *says* is a decision, and a decision put inside a `tsl::Gui` is
// untestable until someone has a console. So `overlay/` takes a `PairingView`
// and turns it into libultrahand elements, and everything that could be wrong
// about the screen is `ctest -R overlay.pairing`.
//
// What is a decision here is which of four sentences a dead pairing gets. A
// human who refused the code, a code nobody typed in time, and a server that
// answered something this client cannot act on are three different things to be
// told, and only the third is worth a bug report -- collapsing them into
// "pairing failed" is the support thread docs/AUTH.md was written to prevent.
// The fourth is `kStarting`, which exists so that thirty seconds of an init in
// flight does not read as a button that did not register.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// libultrahand type. What crosses is strings and enums.
//
// **The screen never polls the network.** It renders a `PairingStatus` that
// `GetPairState` answered off a snapshot, and nothing here names a
// `PairingSession` method -- the sysmodule owns the poll interval, and an
// overlay that drove `Poll()` on its draw thread would undercut the server's
// `interval`, earn `slow_down` on every later poll and wedge the pairing until
// the code expired (pairing.hpp). Taking the status **by value** is what makes
// that a compile-time fact rather than a comment.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "rommsync/overlay_status_view.hpp"
#include "rommsync/pairing.hpp"

namespace rommsync::overlay {

/// Why a pairing attempt could not be started at all.
///
/// Distinct from every `PairingState`, because none of them happened: the
/// sysmodule refused the command, so there is no attempt to report the state
/// of. `Link` covers the case where the sysmodule could not be reached; this
/// covers the case where it answered and said no.
enum class PairBlock {
  /// `ipc::Error::kNotConfigured` -- there is no `server.url` to pair with.
  /// Established by reading `Status::configured` back rather than by decoding
  /// the Horizon `Result`, so the overlay does not carry a second copy of the
  /// sysmodule's result module (`sysmodule/source/ipc/service.hpp`).
  kNoServer,

  /// It refused for some other reason. `kUnavailable` is the one this build
  /// will actually meet until the console has an `HttpClient` to reach a server
  /// with -- M1-6 (#123) built the engine half, and #43's gate still wants the
  /// Horizon `ssl` backend under it.
  kRefused,
};
const char* ToString(PairBlock block);

/// Everything the pairing screen draws, and nothing about how.
///
/// Every string is either non-empty or deliberately absent: `code`, `url` and
/// `countdown` are empty together, and only when there is no live code to show.
/// `qr_payload` is never set without them and may be empty beside them, because
/// the QR is optional. `headline` is never empty in any state.
struct PairingView {
  /// The state **as drawn**, which is not always the state that was reported: a
  /// `kPending` whose countdown has reached zero draws as `kExpired`, because a
  /// code the server will refuse is not a code to put in front of a human.
  auth::PairingState state = auth::PairingState::kIdle;

  /// The one sentence at the top. Never empty.
  std::string headline;

  /// What the user does next, or empty when there is nothing to do.
  std::string hint;

  /// The headline's tone. The renderer maps it to a colour; `core/` names none.
  Tone tone = Tone::kNeutral;

  /// The eight characters, exactly as RomM issued them.
  ///
  /// Never grouped, never re-spaced, never case-folded: someone is retyping
  /// this. RomM's alphabet already excludes `I`, `L`, `O`, `0` and `1`
  /// (docs/AUTH.md), so there is nothing here for a renderer to "correct" --
  /// and a `ABCD-1234` that the website will not accept is the failure this
  /// note exists to prevent.
  std::string code;

  /// The absolute URL a human types, already joined with the configured origin
  /// by `DeviceInitResponse::VerificationUrl()`. Not re-joined here: doing it
  /// twice is how `//pair/device` gets shipped.
  std::string url;

  /// The same URL with `?user_code=` on it, for a QR code. Optional and
  /// rendering-side, and nothing draws one yet: `pairing_screen.cpp` shows the
  /// address and the code, which is the version a person can act on with no
  /// second device. Carried here so a QR is a drawing change in M8-2 (#44)
  /// rather than a change to what crosses IPC.
  std::string qr_payload;

  /// "Expires in 9:12", or empty when there is no live code.
  std::string countdown;

  /// The `PairingStatus::message`, or the sentence that stands in for one.
  /// Log-safe and user-safe by construction -- it is a status code and a
  /// reason, never a body and never a credential (pairing.hpp).
  std::string detail;

  /// Whether to offer "Start over".
  ///
  /// `IsTerminal` is the shorthand and not the rule. Three of the four terminal
  /// states offer it; `kApproved` does not, because a console that just paired
  /// has nothing to start over and re-pairing is the settings screen's action
  /// (#26). A `kPending` whose code has run out *does* offer it despite not
  /// being terminal -- the sysmodule's next `Poll()` will make it so, and until
  /// then the user has nothing left to do with the code and no way out of the
  /// screen but to leave it.
  bool start_over = false;

  /// Whether to offer "Pair". Only `kIdle`, where nothing has been started.
  bool start = false;
};

/// The screen for a `PairingStatus` the sysmodule answered with.
///
/// By value, on purpose: see the header note. The whole input is a snapshot
/// that has already crossed IPC, so there is no session to reach back into and
/// no poll for this call to drive.
PairingView RenderPairing(auth::PairingStatus status);

/// The screen for a sysmodule that could not be asked, or could not be
/// understood. `link` must not be `kOk`; the wording is the status screen's,
/// because it is the same diagnosis.
PairingView RenderPairingUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

/// The screen for a `StartPair` the sysmodule refused.
PairingView RenderPairingBlocked(PairBlock block);

/// "9:12" -- whole minutes and seconds, zero-padded, counting the seconds the
/// code has left. `0` and anything negative render "0:00".
///
/// Published because it is the piece with the arithmetic in it, and because a
/// second screen showing the same countdown must not round it differently.
std::string FormatCountdown(std::chrono::seconds remaining);

}  // namespace rommsync::overlay

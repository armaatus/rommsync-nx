// The overlay's pairing screen, decided over a decoded `auth::PairingStatus`.
//
// M4-5. The screen itself cannot be checked before the M8-1 gate -- nothing in
// this repo draws a frame -- so everything about it that is a *decision* lives
// in `rommsync::overlay::PairingView`, and this is what holds it. What is under
// test is which of four sentences a dead pairing gets, that a code is drawn
// exactly as RomM issued it, and that a countdown at zero never leaves a spent
// code in front of a human.
//
// Every case goes through `SerializePairingStatus` and `ParsePairingStatus`
// rather than rendering a hand-built struct: the overlay never sees a status
// that did not come off the wire, and a field that stopped surviving the codec
// would otherwise render fine here and wrongly on a console.
//
// No server, no console, no emulator -- pure decisions over pure data, so this
// never skips.
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "checks.hpp"
#include "rommsync/overlay_pairing_view.hpp"
#include "rommsync/pairing.hpp"

namespace {

namespace auth = rommsync::auth;
namespace overlay = rommsync::overlay;

using checks::Checks;
using namespace std::chrono_literals;

/// The compile-time half of "the screen never polls the network".
///
/// `RenderPairing` takes its status **by value**, so what it holds is a
/// snapshot that has already crossed IPC and there is no session behind it to
/// call `Poll()` on. A signature taking a `PairingSession&` -- or a reference
/// into one -- would make driving the interval off the overlay's draw thread
/// something an author could do without noticing, and polling faster than the
/// server's interval earns `slow_down` on every later poll and wedges the
/// pairing until the code expires (pairing.hpp).
static_assert(std::is_same_v<decltype(overlay::RenderPairing),
                             overlay::PairingView(auth::PairingStatus)>,
              "RenderPairing takes a PairingStatus by value");

/// The characters RomM issues a `user_code` from (docs/AUTH.md). `I`, `L`, `O`,
/// `0` and `1` are already excluded, which is why nothing downstream may
/// "correct" one.
constexpr const char* kAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

/// A live code, mid-pairing. Every scenario below takes this and changes one
/// thing, so what a scenario is about is the field it touches.
auth::PairingStatus Pending() {
  auth::PairingStatus status;
  status.state = auth::PairingState::kPending;
  status.user_code = "ABCD2345";
  status.verification_url = "http://romm.lan:8080/pair/device";
  status.verification_url_complete = "http://romm.lan:8080/pair/device?user_code=ABCD2345";
  status.expires_in = 552s;
  status.polls = 3;
  return status;
}

/// Render `status` the way the overlay does: serialise it, parse it, draw that.
///
/// The parse is asserted rather than assumed -- a status this suite could not
/// put on the wire is a bug in the payload, not in the screen, and it must not
/// be reported as a rendering failure.
overlay::PairingView RenderOverWire(Checks& checks, const auth::PairingStatus& status) {
  const auth::Parsed<auth::PairingStatus> parsed =
      auth::ParsePairingStatus(auth::SerializePairingStatus(status));
  checks.Expect(parsed.ok(), "the status survives the wire: " + parsed.error.Describe());
  if (!parsed.ok()) {
    return overlay::RenderPairingUnreachable(overlay::Link::kUnreadable);
  }
  return overlay::RenderPairing(parsed.value);
}

/// Every string the screen would draw, joined. What a "the screen never shows
/// X" assertion is made against.
std::string AllText(const overlay::PairingView& view) {
  return view.headline + "\n" + view.hint + "\n" + view.code + "\n" + view.url + "\n" +
         view.qr_payload + "\n" + view.countdown + "\n" + view.detail;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// Nothing a view draws may be blank where a sentence belongs, and a code, its
/// URL and its countdown are all-or-nothing: a code with no address to type it
/// into is a screen a user cannot act on.
void ExpectWellFormed(Checks& checks, const overlay::PairingView& view, const std::string& what) {
  checks.Expect(!view.headline.empty(), what + ": the headline is never empty");
  const bool shows_code = !view.code.empty();
  checks.ExpectEq(view.url.empty(), !shows_code, what + ": a code comes with an address");
  checks.ExpectEq(view.countdown.empty(), !shows_code, what + ": ...and with a countdown");
  checks.ExpectEq(view.qr_payload.empty(), !shows_code, what + ": ...and with a QR payload");
}

// --- the seven states --------------------------------------------------------

/// All seven render, each says something, and no two of them say the same
/// thing. The last part is the acceptance criterion with teeth: four terminal
/// states collapsing into one "failed" is the support thread docs/AUTH.md was
/// written to prevent.
void CheckEveryState(Checks& checks) {
  const auth::PairingState kStates[] = {
      auth::PairingState::kIdle,     auth::PairingState::kStarting,
      auth::PairingState::kPending,  auth::PairingState::kApproved,
      auth::PairingState::kDenied,   auth::PairingState::kExpired,
      auth::PairingState::kFailed,
  };
  std::vector<std::string> headlines;
  for (const auth::PairingState state : kStates) {
    auth::PairingStatus status = Pending();
    status.state = state;
    const overlay::PairingView view = RenderOverWire(checks, status);
    const std::string what = std::string("state ") + auth::ToString(state);
    ExpectWellFormed(checks, view, what);
    for (const std::string& seen : headlines) {
      checks.Expect(seen != view.headline, what + " reads differently from every other state");
    }
    headlines.push_back(view.headline);
  }
  checks.ExpectEq(headlines.size(), std::size_t{7}, "all seven states were rendered");
}

/// The three dead ends, which are three different things to be told: someone
/// refused it, nobody typed it in time, and the server answered something this
/// client cannot act on. Only the last is worth a bug report, and only the last
/// says so.
void CheckTerminalStatesDiffer(Checks& checks) {
  auth::PairingStatus base = Pending();
  base.message.clear();

  base.state = auth::PairingState::kDenied;
  const overlay::PairingView denied = RenderOverWire(checks, base);
  base.state = auth::PairingState::kExpired;
  const overlay::PairingView expired = RenderOverWire(checks, base);
  base.state = auth::PairingState::kFailed;
  const overlay::PairingView failed = RenderOverWire(checks, base);

  checks.Expect(denied.headline != expired.headline, "denied does not read as expired");
  checks.Expect(denied.headline != failed.headline, "denied does not read as failed");
  checks.Expect(expired.headline != failed.headline, "expired does not read as failed");
  checks.Expect(denied.detail != expired.detail, "...and neither do their explanations");
  checks.Expect(denied.detail != failed.detail, "...");
  checks.Expect(expired.detail != failed.detail, "...");

  for (const overlay::PairingView& view : {denied, expired, failed}) {
    checks.Expect(view.start_over, "every dead end offers a way to start over");
    checks.Expect(!view.start, "...and none of them offers Pair, which would be a second button");
    checks.Expect(!view.detail.empty(), "...and none of them explains itself with a blank line");
  }
  checks.Expect(Contains(failed.hint, "report"),
                "only a failure is worth reporting, and its hint says so");
  checks.Expect(!Contains(denied.hint, "report"), "a refusal is not a bug report");
  checks.Expect(!Contains(expired.hint, "report"), "nor is a code nobody typed");

  // `message` is the session's own reason and outranks the stand-in sentence:
  // "the server kept rejecting the poll with HTTP 401" is the whole diagnosis,
  // and a screen that discarded it for a generic line would send the reader to
  // the wrong place.
  base.state = auth::PairingState::kFailed;
  base.message = "the server answered something this client does not understand: HTTP 418";
  const overlay::PairingView reasoned = RenderOverWire(checks, base);
  checks.ExpectEq(reasoned.detail, base.message, "the session's own reason is what is shown");
}

/// `kStarting` is the state that exists so a thirty-second init does not read as
/// a button that did not register.
void CheckStarting(Checks& checks) {
  auth::PairingStatus status = Pending();
  status.state = auth::PairingState::kStarting;
  const overlay::PairingView view = RenderOverWire(checks, status);
  std::string lowered = view.headline;
  for (char& c : lowered) {
    c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  }
  checks.Expect(Contains(lowered, "contacting the server"),
                "starting says the server is being contacted, got: " + view.headline);
  checks.Expect(view.code.empty(), "...and shows no code, because there is not one yet");
  checks.Expect(!view.start, "...and offers no Pair button over an attempt already running");
  checks.Expect(!view.start_over, "...nor a Start over, which would cancel it");

  const overlay::PairingView idle = RenderOverWire(checks, auth::PairingStatus{});
  checks.Expect(idle.start, "idle is the one state that offers Pair");
  checks.Expect(idle.headline != view.headline, "and it does not read as an attempt in flight");
}

/// The live code: what is drawn, and what is offered beside it.
void CheckPending(Checks& checks) {
  const auth::PairingStatus status = Pending();
  const overlay::PairingView view = RenderOverWire(checks, status);
  checks.ExpectEq(view.code, status.user_code, "the code is shown");
  checks.ExpectEq(view.url, status.verification_url, "the address is the absolute one");
  checks.ExpectEq(view.qr_payload, status.verification_url_complete,
                  "and the QR payload is the one with the code on it");
  checks.Expect(!view.start, "a live code offers no Pair button");
  checks.Expect(!view.start_over, "...and no Start over while there is still time");
  checks.ExpectEq(std::string(auth::ToString(view.state)),
                  std::string(auth::ToString(auth::PairingState::kPending)),
                  "and it draws as what it is");

  // The URL is joined by `DeviceInitResponse::VerificationUrl()` before it ever
  // reaches here. A screen that joined it again is how `//pair/device` ships.
  checks.Expect(!Contains(view.url, "//pair"), "the address was not joined a second time");

  // Retries during a pending poll are why a countdown can look stuck. The
  // session's message is the only place that says so.
  auth::PairingStatus retrying = status;
  retrying.message = "the server is unwell (HTTP 503); retrying";
  checks.ExpectEq(RenderOverWire(checks, retrying).detail, retrying.message,
                  "a stalling poll explains itself under the code");
}

// --- the countdown -----------------------------------------------------------

/// It counts down, it is formatted the same way every time, and zero is not a
/// number this screen prints beside a live code.
void CheckCountdown(Checks& checks) {
  checks.ExpectEq(overlay::FormatCountdown(552s), std::string("9:12"), "minutes and seconds");
  checks.ExpectEq(overlay::FormatCountdown(600s), std::string("10:00"), "a whole ten minutes");
  checks.ExpectEq(overlay::FormatCountdown(65s), std::string("1:05"), "seconds are zero-padded");
  checks.ExpectEq(overlay::FormatCountdown(9s), std::string("0:09"), "...under a minute too");
  checks.ExpectEq(overlay::FormatCountdown(0s), std::string("0:00"), "and zero is zero");
  // `ParsePairingStatus` refuses a negative countdown, so this is only reachable
  // by a caller with a hand-built status -- and "-1:-1" is not a thing a screen
  // may draw.
  checks.ExpectEq(overlay::FormatCountdown(-5s), std::string("0:00"), "so is anything below it");

  auth::PairingStatus status = Pending();
  status.expires_in = 552s;
  checks.ExpectEq(RenderOverWire(checks, status).countdown, std::string("Expires in 9:12"),
                  "the screen counts the seconds the status carried");
  status.expires_in = 61s;
  checks.ExpectEq(RenderOverWire(checks, status).countdown, std::string("Expires in 1:01"),
                  "...and it goes down as they do");
}

/// A pending code whose time ran out. The state machine has not caught up --
/// only the sysmodule's next `Poll()` does that -- and the seconds in between
/// are exactly when a user would start retyping a code the server will refuse.
void CheckExpiredCode(Checks& checks) {
  auth::PairingStatus status = Pending();
  status.expires_in = 0s;
  const overlay::PairingView view = RenderOverWire(checks, status);
  checks.Expect(view.code.empty(), "a spent code is not put in front of a human");
  checks.Expect(view.url.empty(), "...nor an address to type it into");
  checks.Expect(view.qr_payload.empty(), "...nor a QR code that carries it");
  checks.Expect(view.countdown.empty(), "...nor a countdown reading zero");
  checks.Expect(view.start_over, "and starting over is the one thing left to do");

  auth::PairingStatus expired = status;
  expired.state = auth::PairingState::kExpired;
  const overlay::PairingView settled = RenderOverWire(checks, expired);
  checks.ExpectEq(view.headline, settled.headline,
                  "a code that ran out reads the same before and after the state catches up");
  checks.ExpectEq(std::string(auth::ToString(view.state)),
                  std::string(auth::ToString(auth::PairingState::kExpired)),
                  "and it draws as expired");
}

// --- the code itself ---------------------------------------------------------

/// Drawn verbatim. No dash, no space, no case fold, no character swapped for one
/// that looks like it -- someone is retyping this into a website that will
/// compare it byte for byte (docs/AUTH.md).
void CheckCodeIsVerbatim(Checks& checks) {
  const std::string alphabet = kAlphabet;
  for (std::size_t start = 0; start < alphabet.size(); ++start) {
    std::string code;
    for (std::size_t i = 0; i < 8; ++i) {
      code += alphabet[(start + i) % alphabet.size()];
    }
    auth::PairingStatus status = Pending();
    status.user_code = code;
    status.verification_url_complete = status.verification_url + "?user_code=" + code;
    const overlay::PairingView view = RenderOverWire(checks, status);
    checks.ExpectEq(view.code, code, "the code is drawn exactly as it arrived");
    checks.Expect(!Contains(view.code, "-"), "no separator is inserted into " + code);
    checks.Expect(!Contains(view.code, " "), "and no space either");
    checks.ExpectEq(view.code.size(), std::size_t{8}, "and it stays eight characters");
  }

  // The alphabet the codes come from, asserted here because it is what makes
  // "never correct a character" safe to promise: there is no `O`/`0` pair left
  // for a renderer to disambiguate.
  for (const char excluded : {'I', 'L', 'O', '0', '1'}) {
    checks.Expect(alphabet.find(excluded) == std::string::npos,
                  std::string("the alphabet excludes ") + excluded);
  }
}

// --- what never reaches the screen -------------------------------------------

/// `PairingStatus` carries neither the `device_code` nor the token, and the
/// screen cannot draw what it was not given. The interesting half is a payload
/// written by some *other* build that does carry them: whatever the decoder does
/// with the extra fields, none of it may end up on the panel.
void CheckNoSecretsReachTheScreen(Checks& checks) {
  const std::string device_code(64, 'a');
  const std::string access_token = "rmm_" + std::string(64, 'b');

  const auth::PairingStatus status = Pending();
  const std::string payload = auth::SerializePairingStatus(status);
  checks.Expect(!Contains(payload, "device_code"), "the payload names no device_code");
  checks.Expect(!Contains(payload, "access_token"), "...and no access_token");

  const std::string smuggled =
      R"({"state":"pending","user_code":"ABCD2345",)"
      R"("verification_url":"http://romm.lan:8080/pair/device",)"
      R"("verification_url_complete":"http://romm.lan:8080/pair/device?user_code=ABCD2345",)"
      R"("expires_in":552,"polls":3,"message":"",)"
      R"("device_code":")" + device_code + R"(","access_token":")" + access_token + R"("})";
  const auth::Parsed<auth::PairingStatus> parsed = auth::ParsePairingStatus(smuggled);
  if (parsed.ok()) {
    // The decoder is free to ignore fields it does not know; the screen is not
    // free to draw them.
    const std::string drawn = AllText(overlay::RenderPairing(parsed.value));
    checks.Expect(!Contains(drawn, device_code), "no device_code reaches the screen");
    checks.Expect(!Contains(drawn, access_token), "and no token does either");
  }

  // And nothing invents one out of a message, which is the only free-text field
  // that crosses.
  auth::PairingStatus failed = status;
  failed.state = auth::PairingState::kFailed;
  failed.message = "the server is unwell (HTTP 503); retrying";
  const std::string drawn = AllText(RenderOverWire(checks, failed));
  checks.Expect(!Contains(drawn, "rmm_"), "and no token-shaped string appears from nowhere");
}

// --- the screen with no answer to render -------------------------------------

/// A sysmodule that could not be asked, and one that answered and said no.
/// Neither is a `PairingState`, because in neither case did an attempt happen.
void CheckUnreachableAndBlocked(Checks& checks) {
  for (const overlay::Link link :
       {overlay::Link::kNotRunning, overlay::Link::kUnreadable, overlay::Link::kIncompatible}) {
    const overlay::PairingView view = overlay::RenderPairingUnreachable(link, 0);
    ExpectWellFormed(checks, view, std::string("link ") + overlay::ToString(link));
    checks.Expect(!view.start && !view.start_over,
                  "nothing is offered against a sysmodule that is not answering");
    // The same sentence the status screen gives for the same console: two
    // screens disagreeing about a missing service is a thing only a console
    // would ever show.
    checks.ExpectEq(view.headline, overlay::RenderUnreachable(link, 0).headline,
                    "the diagnosis reads the same on both screens");
  }
  const overlay::PairingView incompatible =
      overlay::RenderPairingUnreachable(overlay::Link::kIncompatible, 3);
  checks.Expect(Contains(incompatible.hint, "3"),
                "a version mismatch names the version the sysmodule speaks");

  const overlay::PairingView no_server = overlay::RenderPairingBlocked(overlay::PairBlock::kNoServer);
  ExpectWellFormed(checks, no_server, "blocked: no server");
  checks.Expect(!no_server.start, "there is no Pair button when there is nothing to pair with");
  checks.Expect(Contains(no_server.hint, "server.url"),
                "and the hint names the setting that is missing");

  const overlay::PairingView refused = overlay::RenderPairingBlocked(overlay::PairBlock::kRefused);
  ExpectWellFormed(checks, refused, "blocked: refused");
  checks.Expect(refused.headline != no_server.headline,
                "a refusal does not read as a missing server");
  checks.Expect(refused.start, "and a refusal can be tried again");
}

/// The other half of the same rule, which the compiler cannot check: the view
/// model must not *name* a `PairingSession` method at all. One that reached for
/// the session would compile fine and hand the overlay the poll interval the
/// sysmodule owns. Comment lines are skipped -- the header explains the rule in
/// prose, and explaining it is not breaking it.
void CheckNamesNoSession(Checks& checks) {
  for (const char* path : {ROMMSYNC_PAIRING_VIEW_HDR, ROMMSYNC_PAIRING_VIEW_SRC}) {
    std::ifstream file(path);
    checks.Expect(file.good(), std::string("the view model is readable: ") + path);
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
      ++number;
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
        continue;
      }
      checks.Expect(!Contains(line, "PairingSession"),
                    std::string(path) + ":" + std::to_string(number) +
                        " names PairingSession; the view renders a snapshot and owns no poll");
    }
  }
}

}  // namespace

int main() {
  Checks checks;
  CheckEveryState(checks);
  CheckTerminalStatesDiffer(checks);
  CheckStarting(checks);
  CheckPending(checks);
  CheckCountdown(checks);
  CheckExpiredCode(checks);
  CheckCodeIsVerbatim(checks);
  CheckNoSecretsReachTheScreen(checks);
  CheckUnreachableAndBlocked(checks);
  CheckNamesNoSession(checks);

  if (checks.failures() > 0) {
    std::cerr << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "ok: the pairing screen renders every state, and never a spent code\n";
  return 0;
}

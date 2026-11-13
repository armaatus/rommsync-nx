// Whether this console's clock can be believed (M11-11, #295).
//
// RomM arbitrates a save it has no sync history for on timestamps alone: the
// client's `updated_at` against the server's copy (docs/API_CONTRACT.md, the
// reason table). The client's side of that is the card's mtime, which is the
// console's clock at the moment the save was written -- and a Switch's clock is
// whatever its RTC says. A drained battery or a NAND restore puts it in the
// past; a user who set it by hand can put it anywhere. Only an mtime at the
// epoch was refused before this (`kMinTimestampSeconds`), so a console a year
// ahead won every first sync it entered, uploading over a newer server save.
//
// So the clock is checked against two references the console does not set:
//
//   - **the network clock** -- Horizon's `time:s` network system clock, which
//     nifm synchronises from Nintendo's servers. Read by the sysmodule
//     (`SdEngine::UseNetworkClock`) and absent on a console that has never
//     synchronised it, and on the host.
//   - **the server's `Date` header**, off the negotiation that produced the
//     plan. The more relevant of the two: RomM compares the client's timestamp
//     with ones *it* stamped, so a skew against the server is the skew the
//     arbitration sees.
//
// And against a floor no correct clock is below: the year this check was
// written. Neither reference is needed for that one, which is what catches the
// commonest case -- an RTC reset to 2000-something on a console offline since.
//
// **What a clock that fails costs** (`ExecuteOptions::clock`): the uploads RomM
// decided on the timestamp alone -- `Reason::kClientNewerNoHistory` -- are held,
// failed with `OperationError::kClockUntrusted`, and nothing is sent. Every
// other operation runs. A download still backs up the local save first (hard
// rule 2), so a clock in the past that loses an arbitration it should have won
// costs a restore from `.backup/`, never the save; a clock in the future that
// won one it should have lost would cost the *server's* save, which nothing on
// the console backed up. An upload decided by digest
// (`Reason::kClientNewer`, with a baseline) or into an empty slot
// (`Reason::kClientOnly`) does not depend on the clock, and a player's Keep
// local is their choice, not the clock's.
//
// Hard rule 4: standard headers and `rommsync/` only. Nothing here reads a
// clock: every instant is the caller's, so a test can put a console anywhere.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "rommsync/http.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::sync {

/// How far the console may be from a reference before its timestamps stop being
/// believed. A day: wide enough for any time zone mistaken for UTC (UTC+14 is
/// the widest) and for a console that sat on a shelf with a slightly wrong RTC,
/// narrow enough to catch a clock set to the wrong day -- which is the error
/// that decides a first sync.
inline constexpr std::chrono::hours kMaxClockSkew{24};

/// `2026-01-01T00:00:00Z`. No correct clock running this build is before it --
/// this check did not exist until then -- and an RTC that reset is usually
/// decades before it. Seconds, for `kMinTimestampSeconds`'s reason.
inline constexpr std::int64_t kClockFloorSeconds = 1767225600;

/// What the check concluded.
enum class ClockVerdict {
  /// No reference disagrees by more than `kMaxClockSkew`, and the clock is past
  /// the floor. Also the answer with no reference at all and a clock past the
  /// floor: an unknown is not a finding.
  kTrusted,

  /// The console's clock is before `kClockFloorSeconds`: unset, or reset.
  kBeforeFloor,

  /// More than `kMaxClockSkew` behind a reference.
  kBehind,

  /// More than `kMaxClockSkew` ahead of a reference. The dangerous direction:
  /// a save stamped by it wins a timestamp arbitration it should lose.
  kAhead,
};

/// `trusted` / `before_floor` / `behind` / `ahead`. Never null.
const char* ToString(ClockVerdict verdict);

/// Which reference a verdict was reached against.
enum class ClockReference { kNone, kNetwork, kServer };

/// `none` / `network clock` / `server`. Never null.
const char* ToString(ClockReference reference);

/// Every instant the check reads, taken by the caller at about the same moment.
struct ClockReading {
  /// What the console says the time is -- the clock a save's mtime comes from.
  Timestamp local{};

  /// The network system clock, when the console has one synchronised.
  std::optional<Timestamp> network;

  /// The server's `Date` header (`ParseHttpDate`), when it sent one.
  std::optional<Timestamp> server;
};

/// One check's conclusion.
struct ClockCheck {
  ClockVerdict verdict = ClockVerdict::kTrusted;

  /// What `skew_seconds` is measured against. `kNone` for a `kBeforeFloor`
  /// reached with no reference to hand, and for a `kTrusted` with none.
  ClockReference reference = ClockReference::kNone;

  /// The console minus the reference, in whole seconds: positive is ahead. Zero
  /// when `reference` is `kNone`.
  std::int64_t skew_seconds = 0;

  bool trusted() const { return verdict == ClockVerdict::kTrusted; }

  /// One sentence for the log and for a held upload's message: which way, by
  /// how much, against what. Says the clock is fine when it is.
  std::string Describe() const;
};

/// The check. Pure: `reading` is every input.
///
/// The floor first, because it needs no reference; then the network clock,
/// then the server. The first reference past `kMaxClockSkew` decides, and a
/// floor verdict still names a reference's skew when there is one.
ClockCheck CheckClock(const ClockReading& reading);

/// `Sun, 06 Nov 1994 08:49:37 GMT` -- RFC 9110's IMF-fixdate, the one form a
/// server is required to send `Date` in -- as a `Timestamp`. Empty for any
/// other shape, including the two obsolete forms RFC 9110 lets a recipient
/// accept: no server in front of RomM sends them, and a reference misread is
/// worse than none.
std::optional<Timestamp> ParseHttpDate(std::string_view text);

/// The `Date` header of `headers`, parsed, or empty.
std::optional<Timestamp> ServerDate(const http::Headers& headers);

/// Whether `operation` is an upload the server decided on the client's clock
/// alone: `Reason::kClientNewerNoHistory`. The one an untrusted clock holds.
bool DecidedByClock(const SyncOperation& operation);

/// Whether a TLS failure is likely the clock's: a certificate is valid between
/// two dates, and a console outside them refuses every certificate. True when
/// `transport` is `http::Error::kTls` and either the clock check failed or the
/// backend's own message says the certificate is expired or not yet valid.
/// The check passed here is the one without a server reference -- a handshake
/// that failed has no `Date` header to offer.
bool TlsClockFault(http::Error transport, std::string_view detail, const ClockCheck& clock);

}  // namespace rommsync::sync

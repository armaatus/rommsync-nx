// The one surface `ovl-rommsync` talks to `sys-rommsync` through.
//
// The overlay owns no sync, download or auth logic (overlay/AGENTS.md): it
// renders what the sysmodule reports and asks it to do things. This header is
// that boundary written down once -- the command ids, the payloads, the error
// model -- so the four M4 screens, M3-5 and M5-3/M5-4 are all coded against the
// same contract instead of three worktrees each inventing one.
//
// ## Where the halves live
//
// Everything here is portable: hard rule 4 says `core/` names no libnx type, so
// there is no `Result` and no `Service` below. The payloads are POD-ish and are
// serialised with `rommsync::json`, which is not a new decision --
// `pairing.hpp` already ships `SerializePairingStatus`/`ParsePairingStatus` and
// calls itself "the IPC payload behind `GetPairState`". This follows it.
//
//   `core/`     -- this header: ids, payloads, codecs, `ServiceCore`, `Dispatch`.
//   `sysmodule/source/ipc/` -- the cmif binding: buffers in, buffers out, and
//                  `Error` mapped to a Horizon `Result`. No logic.
//   `overlay/source/ipc_client.*` -- `smGetService("rommsync")` and the *same*
//                  codecs, so the two halves cannot disagree about a field.
//
// ## The framing, and why it is uniform
//
// Every command is the same shape: a JSON object in, a JSON object out. A
// command that takes nothing sends `{}`; one that answers nothing answers `{}`.
// That is what lets the sysmodule side be a pure dispatch -- one entry point
// taking a command id and two buffers -- rather than fourteen hand-marshalled
// stubs, each of which is a place to get a length wrong on a device with no
// debugger attached.
//
// It costs a JSON parse per call. That is affordable precisely because of the
// next rule.
//
// ## The bounds
//
// `kMaxPayloadBytes` caps **every** single request and response. The sysmodule's
// inner heap is `0xC0000` with ~650 KiB left after the trimmed bsd transfer
// memory (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision), and that
// budget already owes a download buffer and the `state.db` baseline. Nothing
// here may grow with the size of the library: lists page (`ListBegin` /
// `ListNext` / `ListEnd`, M5-4), and the one payload that can legitimately grow
// with a *user's file* -- the folder map in `GetConfig` -- is truncated with a
// flag rather than refused, because `GetConfig` is documented never to fail.
//
// ## What never crosses this boundary
//
// No `device_code` and no bearer token, ever (docs/SECURITY.md, "never
// logged"). `ipc.secrets` asserts it over every command rather than leaving it
// reviewed.
//
// The `server.url` is a separate and weaker rule, and it is this header's
// rather than SECURITY.md's: `NormalizeServerUrl` refuses `user:password@`
// outright, so a configured URL carries no credential and the settings screen
// (#26) exists to show and edit it. So `GetConfig` carries it, the pairing
// payload carries the two URLs a human has to type, and nothing else does --
// not `Status`, which reports `configured` instead, and not a `Diagnostic`
// message, which is the rule `config::Diagnostic` already keeps because a
// diagnostic goes to a log.
//
// `GetLog` (M7-3, #38) is the third and last carrier, and it is the exception
// that shows what the rule is for. The line it carries is
// `auth::DescribeStoredToken`'s -- which server, which device, which scopes --
// because "which server is this console paired to" is the first question of
// every bug report, and #38 asks for that line by name. What keeps it safe is
// not the boundary but `log::Redact`, which runs inside `log::Write` and takes
// out a bearer token, a `device_code` and a `user:password@` whatever the call
// site wrote. `ipc.secrets` asserts both halves over this command.
//
// ## What never blocks
//
// No command waits on the network. `SyncNow` and `StartPair` hand work to the
// engine thread and return; the overlay polls. That is the same contract
// `PairingSession::status()` already documents, and the reason it exists: an
// overlay redrawing at 60 Hz cannot be parked on a socket, and a sysmodule may
// not park a thread at all when the rule is that nothing blocks boot.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/json.hpp"
#include "rommsync/log.hpp"
#include "rommsync/pairing.hpp"

namespace rommsync::ipc {

/// The service the sysmodule registers and the overlay opens. Eight bytes at
/// most, which is what an `SmServiceName` holds; already declared as
/// `service_host` in `sysmodule/sys-rommsync.json`.
inline constexpr const char* kServiceName = "rommsync";

/// The version of *this contract*, answered by `GetInterfaceVersion`.
///
/// Bumped on any incompatible change: a renamed field, a re-typed one, a
/// command whose meaning moved. Adding a command, or adding a field a decoder
/// tolerates missing, is not incompatible and does not bump it.
///
/// It exists because the two halves ship as separate files and a user will put
/// a new one next to an old one on an SD card. An overlay that can ask "which
/// contract are you?" can say "update the sysmodule"; one that cannot decodes
/// garbage and blames the server.
///
/// **2** since M4-1 (#23), which added `sync_in_progress` and
/// `config_error_count` to `Status`. Adding a field only escapes a bump when a
/// decoder tolerates it missing, and these decoders tolerate nothing missing by
/// design (see the codec note below) -- so a v2 overlay reading a v1
/// sysmodule's `Status` fails to decode. Without the bump that arrives as
/// "sysmodule unreachable", which is the sentence this constant exists to
/// replace with "update the sysmodule".
inline constexpr std::uint32_t kVersion = 2;

/// The ceiling on one encoded request and on one encoded response.
///
/// A few KiB, sized against the sysmodule heap rather than against a round
/// number -- see the header note. Both halves check it: the sysmodule so a
/// buffer it was handed cannot be overrun, the overlay so a sysmodule that
/// answered something enormous is a named refusal rather than an allocation.
inline constexpr std::size_t kMaxPayloadBytes = 8 * 1024;

/// The Horizon result module `Error` is reported under, and the one number on
/// this wire that `core/` states without being able to name its type.
///
/// It is here rather than in `sysmodule/source/ipc/service.hpp`, where it was
/// written, because **both** halves need it: the sysmodule maps an `Error` onto
/// `MAKERESULT(kResultModule, <the enum's ordinal>)`, and the overlay has to map
/// it back -- a `kDuplicate` and a sysmodule that is not running arrive at a
/// screen as two failing `Result`s and nothing else (`overlay/ipc_client.hpp`,
/// `DecodeError`). Two copies of the number is the two halves disagreeing about
/// a wire constant, which is what this header exists to prevent.
///
/// Arbitrary, and ours: nothing allocates module numbers to homebrew, so what
/// matters is only that it is not one a caller would confuse for somebody
/// else's. Deliberately not `Module_Libnx` (345), which is what a libnx call
/// itself failing reports.
inline constexpr std::uint32_t kResultModule = 420;

// --- the command set ----------------------------------------------------------

/// The commands, by id.
///
/// **Ids are stable and are never renumbered.** A removed command leaves a hole:
/// the overlay and the sysmodule are separate downloads and a user will pair an
/// old one with a new one, so an id that changes meaning is a call that does
/// something other than what the caller asked for. Adding a command means taking
/// the next number.
///
/// `kGetInterfaceVersion` is 0 and **its encoding is frozen forever** --
/// `{"interface":<u32>}`, in that shape, from every build there will ever be.
/// It is the one call an overlay makes before it knows whether it can decode
/// anything else (`ipc.version` fails if its bytes change).
enum class Command : std::uint32_t {
  kGetInterfaceVersion = 0,
  kGetStatus = 1,
  kGetConfig = 2,
  kSetConfig = 3,
  kSetEnabled = 4,
  kSyncNow = 5,
  kStartPair = 6,
  kGetPairState = 7,
  kUnpair = 8,
  kEnqueue = 9,
  kDequeue = 10,
  kListBegin = 11,
  kListNext = 12,
  kListEnd = 13,
  kListConflicts = 14,
  kRestoreBackup = 15,
  kGetLog = 16,
};

/// Every command, in id order. The dispatch table and the documentation are both
/// checked against this rather than against a second copy of the list, and the
/// size is deduced so adding one is a single edit.
inline constexpr std::array kAllCommands = {
    Command::kGetInterfaceVersion, Command::kGetStatus,    Command::kGetConfig,
    Command::kSetConfig,           Command::kSetEnabled,   Command::kSyncNow,
    Command::kStartPair,           Command::kGetPairState, Command::kUnpair,
    Command::kEnqueue,             Command::kDequeue,      Command::kListBegin,
    Command::kListNext,            Command::kListEnd,      Command::kListConflicts,
    Command::kRestoreBackup,       Command::kGetLog,
};

/// Stable, log-friendly name -- `GetStatus`. Never null; "Unknown" for an id
/// outside the table.
const char* ToString(Command command);

/// True when `id` names a command this build implements.
bool IsCommand(std::uint32_t id, Command* out = nullptr);

// --- the error model ----------------------------------------------------------

/// Why a command did not do what was asked.
///
/// Portable on purpose: the sysmodule maps this onto a Horizon `Result` at the
/// boundary (`sysmodule/source/ipc/service.cpp`) and `core/` never names one.
///
/// `kUnknownCommand` through `kInternal` are the *transport's* errors rather
/// than any one command's. They are here rather than folded into `kInvalid`
/// because they say something different to whoever is reading a log: a command
/// id this build does not know means the two halves are different releases, and
/// that is the sentence the user needs -- not "invalid request".
enum class Error {
  kOk = 0,

  /// The request was understood and refused. **Nothing was written.** This is
  /// `SetConfig`'s rejection: the diagnostics say which line, and the file on
  /// the card is exactly as it was.
  kInvalid,

  /// The change was accepted and the SD write did not happen. The in-memory
  /// state is unchanged too, so a caller that retries is not fighting a
  /// half-applied edit.
  kWriteFailed,

  /// There is no usable `server.url`, so there is nothing to talk to.
  kNotConfigured,

  kUnknownRom,   ///< no rom with that id in the library
  kQueueFull,    ///< the download queue is at its cap
  kDuplicate,    ///< that rom is already queued
  kMultiFile,    ///< a disc set; not supported in v1 (docs/ARCHITECTURE.md)
  kNotQueued,    ///< `Dequeue` for a rom that is not in the queue
  kBadCursor,    ///< a list cursor that was reclaimed, never issued, or ended
  kOffline,      ///< a list page needed the server and could not reach it

  /// An id this build does not implement -- the two halves are different
  /// releases. `GetInterfaceVersion` is what diagnoses it.
  kUnknownCommand,

  /// The request payload did not decode: truncated, over-long, or a field of
  /// the wrong type. Nothing was attempted.
  kMalformedRequest,

  /// The response would not fit `kMaxPayloadBytes`. A bug on this side rather
  /// than a caller's mistake, and named so it reads as one.
  kTooLarge,

  /// The engine failed in a way it could not name. The last resort, never the
  /// first choice.
  kInternal,

  /// This build has no engine behind that command yet.
  ///
  /// The sysmodule hosts the whole command set from M4-1 (#23) onward, because
  /// the overlay needs a service to talk to, while the machinery behind half of
  /// it is still being built -- the download queue was M3-2 (#19), live config
  /// writes M5-3 (#30), pairing M1-6 (#123). Those commands answer this rather
  /// than a plausible-looking refusal: an `Enqueue` reported as `kQueueFull`
  /// sends a user looking for a full queue, and a `SetEnabled` reported as
  /// `kWriteFailed` sends them looking at their SD card. **Nothing was
  /// attempted and nothing changed**, exactly as for `kInvalid`.
  ///
  /// What still answers it, and what removes each one. M5-4 (#31) took the three
  /// list commands off this list and M7-2 (#37) took `SyncNow` -- the sysmodule
  /// has a scheduler to hand a tick to now, so `RequestSync` starts one and
  /// answers `false` for one reason only, a tick already running. This is what
  /// is left:
  ///
  /// - `StartPair` on a build with **no HTTP transport**. Not the console any
  ///   more: M1-6 (#123) built the engine half and M1-7 (#126) the Horizon
  ///   backend, and `main.cpp` installs it at boot, so a console answers this
  ///   only if that wiring is removed. It survives for a host binary that never
  ///   installed one. It is a different sentence from `kNotConfigured`, which is a
  ///   console with no `server.url` -- one is something the user can fix on the
  ///   settings screen and the other is not. **Note it is not `kOffline`
  ///   either**, which is what a *list* answers on the same console: a list has
  ///   a server it cannot reach, and a pairing attempt has no way to reach one.
  ///
  /// It is appended rather than inserted: `sysmodule::ToResult` maps the
  /// ordinal, so renumbering one would change what an already-built overlay
  /// reads a `Result` as. Each of the issues above removes its own use of it.
  kUnavailable,
};

/// Stable, log-friendly name -- `queue_full`. Never null.
const char* ToString(Error error);

/// Every error, in ordinal order. The same reason `kAllCommands` exists: an
/// enum whose extent is written down once cannot drift from a second copy of
/// the list.
inline constexpr std::array kAllErrors = {
    Error::kOk,          Error::kInvalid,       Error::kWriteFailed,
    Error::kNotConfigured, Error::kUnknownRom,  Error::kQueueFull,
    Error::kDuplicate,   Error::kMultiFile,     Error::kNotQueued,
    Error::kBadCursor,   Error::kOffline,       Error::kUnknownCommand,
    Error::kMalformedRequest, Error::kTooLarge, Error::kInternal,
    Error::kUnavailable,
};

/// True when `ordinal` names an error this build knows, in which case `out`
/// holds it.
///
/// The inverse of the sysmodule's `Error` -> `Result` mapping, which is by
/// ordinal (`sysmodule/source/ipc/service.hpp`), and the reason the ordinals
/// are append-only: this is how the *overlay* reads a refusal back off a
/// failing `Result` (`overlay::DecodeError`). An ordinal from a newer sysmodule
/// is false rather than guessed.
bool IsError(std::uint32_t ordinal, Error* out = nullptr);

/// A value and the reason there isn't one: `value` is default-constructed on
/// failure and must not be used -- check `ok()`.
///
/// Spelled `auth::Parsed`, not a second struct with the same three members. The
/// name is local because these are IPC payloads rather than server responses;
/// the type is shared so a caller that already knows one knows the other.
template <typename T>
using Decoded = auth::Parsed<T>;

// --- payloads -----------------------------------------------------------------

/// How the console stands with the server.
///
/// Three states rather than a bool because they need three different screens:
/// a console that has never paired gets the pairing flow, one whose token was
/// revoked gets "pair again", and a paired one gets the status screen. Collapsing
/// the first two sends a working user through a flow they do not need.
enum class AuthState {
  kNeverPaired,      ///< no `token.dat`; this console has not been paired
  kUnauthenticated,  ///< a token exists and the server has stopped accepting it
  kPaired,
};
const char* ToString(AuthState state);

/// How the last sync ended. `kNever` is what a console reports before its first.
///
/// `kPartial` is its own answer because M2-7 makes it one: a tick that uploaded
/// four saves and failed the fifth is not a failed sync, and telling the user it
/// was sends them looking for a problem with the four that worked.
enum class SyncResult { kNever, kOk, kPartial, kFailed };
const char* ToString(SyncResult result);

/// What the download worker is doing with the entry it is on.
///
/// The per-entry model belongs to M3-5 (#22); this is the projection `Status`
/// carries so the overlay's status screen is one poll per frame rather than two.
/// `kVerifying` is separate from `kDownloading` because a SHA-1 over 120 MiB is
/// seconds with no bytes moving, and a bar stuck at 100% with no label reads as
/// a hang.
enum class DownloadState { kIdle, kQueued, kDownloading, kVerifying, kFailed };
const char* ToString(DownloadState state);

/// The entry the worker is on, as the status screen draws it.
///
/// `bytes_total == 0` is a real answer from a server that declared no length
/// (#22): the overlay needs an indeterminate bar and must never synthesise a
/// percentage from it.
struct DownloadSnapshot {
  DownloadState state = DownloadState::kIdle;

  /// Zero when `state` is `kIdle`. Never a path -- a file name (`sync.hpp`).
  std::int64_t rom_id = 0;
  std::string fs_name;

  /// Bytes already on the card, `.part` included. A resumed download that
  /// reported only this call's bytes would show the bar restarting at zero,
  /// which is the most visible way to get this wrong (#22).
  std::int64_t bytes_done = 0;
  std::int64_t bytes_total = 0;
};

/// Everything the status screen needs, in one call.
struct Status {
  /// `kVersion` as this sysmodule was built with, so the overlay can compare
  /// without a second round trip.
  std::uint32_t interface = kVersion;

  /// `rommsync::version()` -- the build, not the contract. A support thread
  /// starts with "which one are you running", and the overlay is where a user
  /// can read it.
  std::string build;

  /// `[sync] enabled` as it stands on the card, not as anyone asked for it.
  bool enabled = false;

  AuthState auth = AuthState::kNeverPaired;

  /// There is a usable `server.url`. The URL itself never crosses (SECURITY.md).
  bool configured = false;

  /// The last thing the engine tried reached the server. Not a ping: nothing
  /// here touches the network.
  bool online = false;

  /// Whole Unix seconds, `0` for never. Seconds rather than RFC 3339 for the
  /// reason `state.db` uses them: a number cannot be spelled two ways, where the
  /// string can -- this client writes `Z` and RomM writes `+00:00`
  /// (`sync::FormatTimestamp` and `sync::ParseTimestamp` handle both) -- and the
  /// overlay renders a relative time anyway.
  std::int64_t last_sync_at = 0;
  SyncResult last_sync_result = SyncResult::kNever;

  /// A sync tick is running right now.
  ///
  /// The same fact `SyncNow` reports as `kAlreadyRunning`, carried here because
  /// the status screen has to draw it without pressing anything: a console
  /// mid-tick otherwise renders as an idle one whose counts are about to change
  /// on their own. A download is not a substitute -- a tick that is uploading
  /// saves or negotiating has no `DownloadSnapshot` at all (M4-1, #23).
  bool sync_in_progress = false;

  /// The last sync's counts. Not cumulative -- a running total nobody can reset
  /// is a number that stops meaning anything after a month.
  std::int64_t uploaded = 0;
  std::int64_t downloaded = 0;
  std::int64_t conflicts = 0;
  std::int64_t failed = 0;

  std::int64_t queue_depth = 0;

  /// How many `kError` diagnostics `config.ini` produced.
  ///
  /// A count rather than the diagnostics themselves: the whole list is
  /// `GetConfig`'s, and the settings screen (#26) is where a user reads it. What
  /// the status screen needs is the one bit that sends them there, and it needs
  /// it without a second round trip -- a console whose `server.url` will not
  /// parse otherwise renders as merely unconfigured, with nothing saying that
  /// the file it is being told to edit is the file that is already wrong.
  ///
  /// `kError` only. A warning is compatible with a working client
  /// (`config::Severity`), and a status screen that counted those would be red
  /// on a console with nothing wrong with it.
  std::int64_t config_error_count = 0;

  DownloadSnapshot download;
};

/// `GetConfig`'s answer: the file as the sysmodule read it, plus everything
/// wrong with it.
///
/// The diagnostics are the point of the settings screen (#26): `config.ini` is
/// edited on a console with no keyboard, and "line 9 [sync] states: expected
/// true or false" is the difference between a fix and an evening.
struct ConfigView {
  config::Config config;
  std::vector<config::Diagnostic> diagnostics;

  /// The folder map did not fit `kMaxPayloadBytes` and was left out.
  ///
  /// `GetConfig` never fails (it is how the overlay draws anything at all), and
  /// a `[platform.*]` map is the one section a user can grow without limit -- so
  /// the scalar sections are always served and the map is dropped with this flag
  /// and a `kNotice`. The overlay says "too large to show here"; it does not
  /// render an empty map as though the user had configured none.
  bool platforms_truncated = false;
};

/// The longest `server.url` this contract carries.
///
/// The same bound `NormalizeServerUrl` already applies, named again rather than
/// re-chosen: a value below it would withhold a URL `config.ini` accepts, and a
/// value above it would be a bound that never fires. It is here at all because
/// `GetConfig` may not fail, and because a `Config` reaching this boundary need
/// not have come from `ParseConfig` -- M5-3 (#30) builds one from an edit.
inline constexpr std::size_t kMaxServerUrlBytes = config::kMaxPathLength;

/// The longest name `Status` carries -- a rom's `fs_name`, and the build string.
///
/// `Status` is documented never to fail, and a `fs_name` comes off a RomM
/// library, so its length is not this client's to assume. Truncating one costs
/// a few characters off a label; not bounding it costs the whole status screen.
inline constexpr std::size_t kMaxNameBytes = 256;

/// The longest verification URL the pairing payload carries.
///
/// `auth.cpp` reads `verification_path` straight off the server's JSON with no
/// length limit, so a RomM -- or a proxy in front of one -- answering something
/// enormous would make `GetPairState` unable to answer, and it is documented
/// never to fail. A real one is `/pair/device?user_code=ABCD2345`.
inline constexpr std::size_t kMaxVerificationUrlBytes = 512;

/// How many `config::Diagnostic`s one payload carries, and how long each of
/// their three text fields may be.
///
/// `config::kMaxDiagnostics` is 64 and a message may quote a path of
/// `config::kMaxPathLength`, so an unbounded list is several times the payload
/// cap on its own. These two numbers cut it to a size that ordinarily fits.
///
/// **They are not the guarantee.** A diagnostic quotes what the user wrote, and
/// `json::Quote` escapes: a backslash doubles, a control character becomes six
/// bytes -- and `config.cpp` quotes the rejected path back, which may hold
/// exactly those, since holding them is often why it was rejected. So the
/// encoded length is not a function of these constants, and `GetConfig` drops
/// complaints until the payload fits rather than trusting them.
///
/// Nothing is dropped silently: `TrimDiagnostics` appends a `kNotice` naming
/// how many did not fit, and a text field that was cut ends in `...`.
inline constexpr std::size_t kMaxDiagnosticsInPayload = 8;
inline constexpr std::size_t kMaxDiagnosticTextBytes = 240;

/// `diagnostics`, cut down to what one payload carries.
///
/// Shared by `GetConfig` and `SetConfig` so the two cannot disagree about what a
/// settings screen is shown, and public because M5-3 (#30) produces the list
/// `SetConfig` answers with.
std::vector<config::Diagnostic> TrimDiagnostics(const std::vector<config::Diagnostic>& diagnostics);

/// One `key = value` under one section, as `config.ini` spells it, and the edit
/// they arrive in.
///
/// `SetConfig` is a list of these rather than a whole `Config` for two reasons:
/// a whole map does not fit the payload cap, and an overlay that sends back
/// everything it was shown would silently overwrite a section the running
/// sysmodule has since re-read.
///
/// They are `config::`'s own types rather than copies of them. M5-3 (#30) owns
/// what applying one means and what it refuses (`config::ApplyEdit`), and a
/// second struct here would be a second place for the wire's bounds and the
/// applier's rules to drift apart -- the encoded bytes are the same either way,
/// and the field names are the ones `ipc.cpp` already writes.
using ConfigAssignment = config::Assignment;
using ConfigEdit = config::Edit;

/// How many assignments one `SetConfig` may carry.
///
/// Bounded for `kMaxPayloadBytes`'s reason and one more: an edit is applied as a
/// unit, so an unbounded list is an unbounded amount of work between reading
/// `config.ini` and writing it back. It is `config::kMaxEditAssignments`, since
/// a decoder that accepted one more than the applier does would answer
/// `kInvalid` to a payload this contract said was legal.
inline constexpr std::size_t kMaxAssignments = config::kMaxEditAssignments;

/// What `SyncNow` did with the request. Never "failed": the command hands work
/// to the engine thread and returns, so it reports what it *did with the ask*,
/// and how the sync itself went arrives later through `Status`.
///
/// `kDisabled` is a distinct outcome from `kAccepted` because M4-2 (#24) renders
/// it: a user who pressed "Sync now" with auto-sync switched off has to be told
/// which switch to flip, not shown a spinner that never moves.
enum class SyncOutcome {
  kAccepted,
  kAlreadyRunning,
  kNotConfigured,
  kUnauthenticated,
  kDisabled,
};
const char* ToString(SyncOutcome outcome);

/// What a command that writes to the SD card did.
///
/// The same shape as `SyncOutcome`, and for the same reason: it is an *answer*
/// rather than a failure, so it rides in the payload where the rest of the
/// answer is. That is not a style choice -- it is what the wire can carry. A
/// `cmif` reply's data words are not delivered to the client when the `Result`
/// says the call failed (libnx's `cmifParseResponse` returns before it exposes
/// them), so a refusal reported *only* as a `Result` arrives with nothing
/// attached. `SetConfig`'s diagnostics and `SetEnabled`'s effective state are
/// the whole value of those two refusals, so they may not be attached to one.
///
/// Commands whose failure carries nothing -- `Unpair`, `Enqueue`, `ListNext` --
/// keep reporting it as an `Error`, which the sysmodule maps to a `Result`.
enum class WriteOutcome {
  kApplied,      ///< it took, and the card holds it
  kInvalid,      ///< refused; **nothing was written**. The diagnostics say why.
  kWriteFailed,  ///< accepted, and the write did not happen. Nothing changed.
};
const char* ToString(WriteOutcome outcome);

/// `SetConfig`'s answer.
///
/// The diagnostics come back whichever way it went: on `kInvalid` they *are*
/// the refusal, and a settings screen on a console with no keyboard has nothing
/// else to show. Already trimmed by `TrimDiagnostics`.
struct ConfigResult {
  WriteOutcome outcome = WriteOutcome::kApplied;
  std::vector<config::Diagnostic> diagnostics;
};

/// `SetEnabled`'s answer.
///
/// `enabled` is the state as it stands **after** the attempt, read back rather
/// than assumed -- including after a failed write, which is the case that
/// matters: an overlay that drew the state it asked for would show a switch
/// that did not move (#24).
struct EnabledResult {
  WriteOutcome outcome = WriteOutcome::kApplied;
  bool enabled = false;
};

/// Which list `ListBegin` opens. The per-item projection for each kind is M5-4's
/// (#31); this header owns the envelope they travel in.
enum class ListKind {
  kPlatforms,  ///< RomM's platforms, paged on this side (5.2.0 sends them whole)
  kRoms,       ///< filtered by `platform_id`, optionally by `search`
  kQueue,      ///< served from `queue.json`, never from RomM
};
const char* ToString(ListKind kind);

/// The field names each `ListKind`'s items carry, pinned once.
///
/// An item is a flat object of scalars and `ListItem::Find` reads one *by name*
/// (below), so a producer and a consumer that spell the same field differently
/// do not fail to build -- they render a page of empty rows on a console. The
/// producer is M5-4 (#31) and the first consumer is M4-3's `LibraryBrowserModel`
/// (`overlay_library_model.hpp`); they were written in different worktrees, so
/// the names live here, where the rest of this wire's field names already do.
///
/// **These are the projections, whole.** A kind carries these fields and no
/// others: `AppendIfItFits` bounds a page by *bytes*, so an extra field on a rom
/// is fewer roms per page for every user, forever.
///
/// #25 and #31 named two of them differently before this was written down --
/// `rom_id`/`id` and `size_bytes`/`fs_size_bytes`. Settled as `rom_id` and
/// `size_bytes`: the queue kind has to say `rom_id` because that is what a
/// `download::QueueEntry` is keyed by and what `EncodeRomId` puts on the wire
/// for `Enqueue`, and `core/` already renames RomM's `fs_size_bytes` to
/// `size_bytes` in `roms::Rom` and `download::RomDetail`. One name for one
/// thing, across all three kinds.
namespace list_keys {

/// `kPlatforms`. `mapped` is `config::Config::Platform(fs_slug) != nullptr` as
/// the *sysmodule* reads it -- the overlay never opens `config.ini` to decide
/// whether a platform has a folder, and a platform with none is drawn as
/// skipped with the reason rather than hidden (#25).
/// RomM's row id, which is what `ListRequest::platform_id` takes. #25's scope
/// named only the slug; the slug is what `config::Config::platforms` is keyed
/// by and what a user reads, and the id is what opens the rom list, so the
/// projection carries both.
inline constexpr std::string_view kPlatformId = "id";
inline constexpr std::string_view kPlatformFsSlug = "fs_slug";
inline constexpr std::string_view kPlatformName = "name";
inline constexpr std::string_view kPlatformRomCount = "rom_count";
inline constexpr std::string_view kPlatformMapped = "mapped";

/// `kRoms`.
///
/// `on_disk` and `queued` are what let the browser grey a row *before* a press
/// rather than after one: neither is an `Error` -- `Enqueue` accepts both and
/// the worker settles them `kDone`/`kSkipped` (`download.hpp`) -- so without
/// them a user would queue a rom that is already on the card and see nothing
/// happen.
///
/// `fs_name` is the *directory's* name for a nested single-file rom and
/// therefore carries no extension (#21). It is not a file name to derive
/// anything from.
inline constexpr std::string_view kRomId = "rom_id";
inline constexpr std::string_view kRomName = "name";
inline constexpr std::string_view kRomFsName = "fs_name";
inline constexpr std::string_view kRomPlatformFsSlug = "platform_fs_slug";
inline constexpr std::string_view kRomSizeBytes = "size_bytes";
inline constexpr std::string_view kRomHasMultipleFiles = "has_multiple_files";
inline constexpr std::string_view kRomOnDisk = "on_disk";
inline constexpr std::string_view kRomQueued = "queued";

/// `kQueue`, which is served from `queue.json` and never from RomM (#31), so
/// every one of these is a `download::QueueEntry` field of the same name.
/// `state` is `download::ToString(QueueState)`.
inline constexpr std::string_view kQueueRomId = "rom_id";
inline constexpr std::string_view kQueueFsName = "fs_name";
inline constexpr std::string_view kQueuePlatformFsSlug = "platform_fs_slug";
inline constexpr std::string_view kQueueState = "state";
inline constexpr std::string_view kQueueBytesDone = "bytes_done";
inline constexpr std::string_view kQueueSizeBytes = "size_bytes";
inline constexpr std::string_view kQueueBytesPerSecond = "bytes_per_second";
inline constexpr std::string_view kQueueAttempts = "attempts";
inline constexpr std::string_view kQueueMessage = "message";

}  // namespace list_keys

/// How many items one page may hold, whatever the client asks for.
///
/// A count cap, and it is not the binding one: a page of 64 roms with long
/// names is several times `kMaxPayloadBytes`, so the *byte* bound is what
/// actually decides a page's length. `AppendIfItFits` is how a producer honours
/// both without having to know either.
inline constexpr std::int32_t kMaxPageSize = 64;

/// How many fields one item may carry.
inline constexpr std::size_t kMaxItemFields = 16;

struct ListRequest {
  ListKind kind = ListKind::kPlatforms;

  /// `kRoms` only. `0` means every platform.
  std::int64_t platform_id = 0;

  /// `kRoms` only. Empty means no filter.
  std::string search;

  /// What the client would like. **Clamped** to `[1, kMaxPageSize]` by the
  /// sysmodule; the page says how many it actually holds.
  std::int32_t page_size = kMaxPageSize;
};

/// One field of one list item.
///
/// A tagged scalar rather than a named struct per kind, because the three kinds
/// have three different projections and only M5-4 knows what they are. What this
/// header fixes is that an item is a flat object of scalars -- no nesting, no
/// arrays -- which is what keeps a page's size predictable and its decoder
/// unable to recurse.
struct ListValue {
  enum class Type { kString, kInteger, kBool };

  Type type = Type::kString;
  std::string text;      ///< `kString` only
  std::int64_t number = 0;  ///< `kInteger` only
  bool flag = false;     ///< `kBool` only

  static ListValue Text(std::string value);
  static ListValue Integer(std::int64_t value);
  static ListValue Flag(bool value);

  bool operator==(const ListValue& other) const;
};

struct ListField {
  std::string key;
  ListValue value;
};

struct ListItem {
  std::vector<ListField> fields;

  /// The field named `key`, or nullptr. Valid until this item changes.
  const ListValue* Find(std::string_view key) const;
};

struct ListPage {
  std::vector<ListItem> items;

  /// Another `ListNext` would return more. False on the last page, and on a
  /// `pending` one -- there is nothing yet to be at the end of.
  bool has_more = false;

  /// The engine is still fetching this page; ask again.
  ///
  /// Fetching may not block the IPC thread on the network (#31), so a page that
  /// is not there yet is an answer rather than a wait. `items` is empty.
  bool pending = false;
};

/// Add `item` to `page` unless doing so would break a bound.
///
/// **This is how a page gets filled.** A rom's `name` and `fs_name` are the
/// user's data and have no length this contract can assume, so "64 items" is not
/// a size -- a page of long names is several times `kMaxPayloadBytes`. A producer
/// (M5-4, #31) appends until this returns false, then sets `has_more` and stops.
///
/// Returns false without touching `page` when the item would push it past the
/// item cap or the byte cap. A false on an *empty* page means one single item
/// does not fit a payload, which is a projection that needs shortening rather
/// than a page that needs splitting.
///
/// It re-encodes the page per append, which is O(n^2) in a page of at most 64
/// items fetched at the speed a human scrolls. That is the right trade for a
/// bound that is checked rather than estimated: an estimate that drifted would be
/// a `kTooLarge` on a real user's library and on nobody's test.
bool AppendIfItFits(ListPage* page, ListItem item);

/// A cursor names one open list. `0` is never a valid one, so a
/// default-constructed request cannot accidentally address a live cursor.
using Cursor = std::uint32_t;

// --- the conflict history (M7-1, #36) -----------------------------------------
//
// Two commands rather than a fourth `ListKind`, and the reason is the payload
// rather than taste. A `ListItem` is a flat object of at most `kMaxItemFields`
// scalars; a conflict entry carries eighteen -- both sides of a comparison, two
// paths and the sentence RomM sent -- so it does not fit the envelope the three
// browsable lists share. It also needs no cursor: the history is a bounded
// local vector that never touches the network, so an offset is the whole of the
// state a page needs, and a screen that opened one would spend a cursor out of
// `lists::kMaxCursors` for no fetch.
//
// The entry itself is `conflicts::Entry` rather than a second copy of the
// fields. It is the type the sysmodule stores and the type the overlay draws,
// and one struct is what keeps the two halves from disagreeing about which
// timestamp is whose -- the same reason `ipc::list_keys` exists.

/// How many entries one page may hold, whatever the client asks for.
///
/// A count cap, and -- like `kMaxPageSize` -- not the binding one: an entry
/// carries two paths and a rom's name, so the *byte* bound is what usually
/// decides a page's length. `AppendIfItFits` honours both.
inline constexpr std::int32_t kMaxConflictPage = 8;

/// Which slice of the history to send.
///
/// **An offset, not a page number.** A page can come back shorter than `limit`
/// because the next entry would not fit the payload, and a client that then
/// asked for "page 2" would skip whatever the short page left out. The answer
/// carries the offset it started at and how many it holds, so the next request
/// is `offset + entries.size()` and nothing can fall through the gap.
struct ConflictQuery {
  /// How many entries to skip, newest first. Past the end is an empty page, not
  /// an error: a history that shrank under an open screen is normal.
  std::int32_t offset = 0;

  /// What the client would like. **Clamped** to `[1, kMaxConflictPage]` by the
  /// sysmodule; the page says how many it actually holds.
  std::int32_t limit = kMaxConflictPage;
};

/// One entry, plus the one thing about it that is not in the file.
struct ConflictRow {
  conflicts::Entry entry;

  /// The backup this entry names is still on the card.
  ///
  /// **Not a stored fact.** Whether a file is there is a fact about the card
  /// *now* -- a user can delete a backup, or move the card -- so it is answered
  /// by the sysmodule as the page is built rather than written into
  /// `conflicts.db`, where it would be a claim that went stale the moment it was
  /// made. It is what lets a screen draw an entry as unrestorable **before** a
  /// press instead of finding out at write time (#36).
  ///
  /// False means the sysmodule looked and it was not there. A build that cannot
  /// look at all reports `true` for any entry that names a backup and refuses
  /// the restore itself with `RestoreOutcome::kBackupFailed`, which is the
  /// honest split: "it is gone" and "I cannot see" are different sentences.
  bool backup_present = false;
};

struct ConflictPage {
  /// Newest first, the order the history keeps.
  std::vector<ConflictRow> entries;

  /// Where this page started. Echoed rather than assumed, so a screen that sent
  /// two requests can tell which answer it is holding.
  std::int32_t offset = 0;

  /// There are more entries after this page.
  bool has_more = false;

  /// How many entries the history holds altogether. What lets a screen say
  /// "3 of 12" without walking to the end.
  std::int32_t total = 0;
};

/// Add `entry` to `page` unless doing so would break a bound.
///
/// `AppendIfItFits(ListPage*, ListItem)`'s job and its reasoning: a rom's name
/// and a save's file name are the user's data, so "eight entries" is not a size.
/// Returns false without touching `page`. A false on an *empty* page means one
/// entry does not fit a payload on its own, which is why `conflicts::Shorten`
/// bounds every string before an entry is ever stored.
bool AppendIfItFits(ConflictPage* page, ConflictRow row);

/// How long a `conflicts::RestoreReport::message` may be on this wire.
///
/// The message is written by `core/` and is a sentence for a human, so it is
/// bounded rather than trusted: it names two paths, and a path is as long as a
/// card lets it be.
inline constexpr std::size_t kMaxRestoreMessageBytes = 512;

// --- the log tail -------------------------------------------------------------

/// How many log lines one `GetLog` may answer with.
///
/// `log::kTailLines` and not a number of this header's own: the sysmodule keeps
/// exactly that many in memory (log.hpp), so a larger cap here would promise a
/// page nothing can fill and a smaller one would hide lines the console is
/// holding. A request over it is **clamped**, the way `ConflictQuery::limit` is,
/// rather than refused.
inline constexpr std::int32_t kMaxLogLines = static_cast<std::int32_t>(log::kTailLines);

/// The end of the log, as the overlay draws it.
///
/// Rendered lines rather than a decomposed record. The line is already one flat
/// string with its ordinal, level and event tag in it (log.hpp), it is already
/// bounded, and the thing a user is asked to do with it is *read it and paste
/// it* -- so a struct-per-line would be three fields the overlay would have to
/// join back together before showing anything, and a second spelling of a format
/// docs/TROUBLESHOOTING.md pins.
struct LogTail {
  /// Oldest first, which is the order a log is read in. **What is dropped when
  /// the answer will not fit is the oldest**, so the end of the log -- the part
  /// that says why the last tick failed -- is the part that always arrives.
  std::vector<std::string> lines;

  /// How many lines this process has written in all, kept or not. What tells a
  /// screen that it is looking at a tail rather than at the whole log, and what
  /// makes "it logged nothing at all" distinguishable from "I dropped them".
  std::int64_t total = 0;
};

/// Add `line` to `tail` unless doing so would break a bound.
///
/// `AppendIfItFits(ConflictPage*, ConflictRow)`'s job and its reasoning: a log
/// line carries a file path and a `Diagnostic`, which are the user's data, so
/// `kMaxLogLines` is not a size. Returns false without touching `tail`.
bool AppendIfItFits(LogTail* tail, std::string line);

// --- codecs -------------------------------------------------------------------
//
// Every one of these round-trips losslessly and every decoder refuses a payload
// it cannot read whole -- truncated, over-long, or a field of the wrong type --
// rather than defaulting a field. That is `json::Reader`'s rule applied one
// level up: a `Status` whose `enabled` silently defaulted is an overlay drawing
// a switch in the wrong position, and a `ConfigEdit` whose `remove` defaulted is
// a setting the user did not ask to lose.
//
// Encoders return the text. They can fail only by exceeding `kMaxPayloadBytes`,
// which the caller checks with `Fits` -- `Dispatch` does it for every response
// so no individual encoder has to remember.

/// True when `payload` is inside `kMaxPayloadBytes`.
bool Fits(std::string_view payload);

/// The frozen answer to command 0. `{"interface":N}` and nothing else, forever.
std::string EncodeInterfaceVersion(std::uint32_t interface_version);
Decoded<std::uint32_t> DecodeInterfaceVersion(std::string_view text);

std::string EncodeStatus(const Status& status);
Decoded<Status> DecodeStatus(std::string_view text);

std::string EncodeConfigView(const ConfigView& view);
Decoded<ConfigView> DecodeConfigView(std::string_view text);

std::string EncodeConfigResult(const ConfigResult& result);
Decoded<ConfigResult> DecodeConfigResult(std::string_view text);

std::string EncodeEnabledResult(const EnabledResult& result);
Decoded<EnabledResult> DecodeEnabledResult(std::string_view text);

std::string EncodeConfigEdit(const ConfigEdit& edit);
Decoded<ConfigEdit> DecodeConfigEdit(std::string_view text);

/// `{"enabled":<bool>}` -- `SetEnabled`'s *request*. The answer is an
/// `EnabledResult`, which carries the state that took alongside what happened.
std::string EncodeEnabled(bool enabled);
Decoded<bool> DecodeEnabled(std::string_view text);

std::string EncodeSyncOutcome(SyncOutcome outcome);
Decoded<SyncOutcome> DecodeSyncOutcome(std::string_view text);

/// `{"rom_id":<int>}` -- `Enqueue` and `Dequeue`.
std::string EncodeRomId(std::int64_t rom_id);
Decoded<std::int64_t> DecodeRomId(std::string_view text);

/// `{"position":<int>}` -- where in the queue `Enqueue` put it, 1-based.
std::string EncodeQueuePosition(std::int32_t position);
Decoded<std::int32_t> DecodeQueuePosition(std::string_view text);

std::string EncodeListRequest(const ListRequest& request);
Decoded<ListRequest> DecodeListRequest(std::string_view text);

std::string EncodeCursor(Cursor cursor);
Decoded<Cursor> DecodeCursor(std::string_view text);

std::string EncodeListPage(const ListPage& page);
Decoded<ListPage> DecodeListPage(std::string_view text);

std::string EncodeConflictQuery(const ConflictQuery& query);
Decoded<ConflictQuery> DecodeConflictQuery(std::string_view text);

std::string EncodeConflictPage(const ConflictPage& page);
Decoded<ConflictPage> DecodeConflictPage(std::string_view text);

/// `{"entry_id":<int>}` -- `RestoreBackup`'s request.
///
/// Spelled apart from `EncodeRomId` even though both are one integer: they name
/// different things, and a screen that sent a `rom_id` where an entry id was
/// wanted would restore whatever entry happened to carry that number.
std::string EncodeEntryId(std::int64_t entry_id);
Decoded<std::int64_t> DecodeEntryId(std::string_view text);

std::string EncodeRestoreReport(const conflicts::RestoreReport& report);
Decoded<conflicts::RestoreReport> DecodeRestoreReport(std::string_view text);

/// `{"lines":<int>}` -- `GetLog`'s request: how many of the last lines to send.
///
/// Spelled apart from every other bare integer on this wire for `EncodeEntryId`'s
/// reason, and clamped to `[1, kMaxLogLines]` by `ServiceCore::GetLog` rather
/// than refused here.
std::string EncodeLogRequest(std::int32_t lines);
Decoded<std::int32_t> DecodeLogRequest(std::string_view text);

std::string EncodeLogTail(const LogTail& tail);
Decoded<LogTail> DecodeLogTail(std::string_view text);

/// The payload of a command that carries none: `{}`. A shape rather than an
/// empty buffer, so every decoder on both sides can assume a JSON object.
std::string EncodeEmpty();
json::Error DecodeEmpty(std::string_view text);

// --- the engine behind the service --------------------------------------------

/// One consistent read of everything `GetStatus` needs from the running engine.
///
/// Taken as a value under the engine's own lock, never a reference into live
/// worker state -- `PairingSession::status()`'s contract, for the reason it has
/// it: the overlay asks while a sync tick is running.
struct EngineSnapshot {
  AuthState auth = AuthState::kNeverPaired;
  bool online = false;
  std::int64_t last_sync_at = 0;
  SyncResult last_sync_result = SyncResult::kNever;
  /// A tick is running. See `Status::sync_in_progress`.
  bool sync_in_progress = false;
  std::int64_t uploaded = 0;
  std::int64_t downloaded = 0;
  std::int64_t conflicts = 0;
  std::int64_t failed = 0;
  std::int64_t queue_depth = 0;
  DownloadSnapshot download;
};

/// What `ServiceCore` needs from the rest of the sysmodule.
///
/// This exists because the pieces the commands drive are still being built --
/// the download queue is M3-2 (#19), the scheduler M7-2, live config writes
/// M5-3 (#30), list paging M5-4 (#31) -- and a contract that waited for all four
/// would unblock nothing. It is the seam they implement into, and it is what
/// lets the whole command set be driven end to end by the host harness against a
/// real RomM today (`ipc.engine`).
///
/// **Nothing here may block on the network.** Every method is expected to answer
/// from state the engine already holds, or to hand work to a worker and return.
/// `RequestSync` starts a tick; it does not run one.
///
/// Every method is callable from the IPC thread while the engine's own threads
/// are running, so an implementation owns whatever locking that needs.
class Engine {
 public:
  virtual ~Engine() = default;

  /// The configuration in force, and everything that was wrong with the file it
  /// came from. Both are held by the engine because it is the half that read
  /// them, and the overlay must never re-read the file to answer a question the
  /// sysmodule already knows the answer to.
  virtual const config::Config& config() const = 0;
  virtual const std::vector<config::Diagnostic>& config_diagnostics() const = 0;

  virtual EngineSnapshot Snapshot() const = 0;

  /// The live pairing attempt, or an idle status when there is none.
  virtual auth::PairingStatus pairing_status() const = 0;

  /// Persist `[sync] enabled`. `kWriteFailed` leaves both the file and the
  /// in-memory config exactly as they were.
  virtual Error SetSyncEnabled(bool enabled) = 0;

  /// Apply an edit to `config.ini` (M5-3). Diagnostics come back whether it was
  /// applied or refused; on `kInvalid` nothing was written.
  virtual Error ApplyConfigEdit(const ConfigEdit& edit,
                                std::vector<config::Diagnostic>* diagnostics) = 0;

  /// Ask for a sync tick. False when one is already running. The caller has
  /// already established that there is a server, a token and a switch that is
  /// on -- see `ServiceCore::SyncNow`.
  virtual bool RequestSync() = 0;

  /// Start a pairing attempt, discarding whatever the last one left behind.
  ///
  /// **`pairing_status()` must report an attempt in progress from the moment
  /// this returns.** `PairingSession` only leaves `kIdle` once `Begin()` runs,
  /// and `Begin()` runs on the engine's own thread because it is a request --
  /// so there is a window between handing the work over and the session knowing
  /// about it. An engine that let `kIdle` show through that window would have
  /// the overlay tell the user that pressing Pair did nothing, which is the
  /// exact failure `PairingState::kStarting` exists to prevent (pairing.hpp).
  ///
  /// **A refusal writes nothing and touches no token.** That is what lets the
  /// settings screen's "Re-pair" ask before it discards (M4-4, #26): it sends
  /// this first, and `Unpair` only once an attempt is genuinely under way, so a
  /// console never passes through "unpaired with nothing to restart".
  virtual Error StartPairing() = 0;

  /// Discard the credentials. `kWriteFailed` if they are still on the card.
  virtual Error Unpair() = 0;

  /// Add `rom_id` to the download queue. `position` is 1-based and is only set
  /// on `kOk`.
  virtual Error Enqueue(std::int64_t rom_id, std::int32_t* position) = 0;
  virtual Error Dequeue(std::int64_t rom_id) = 0;

  /// Open a list. `cursor` is set only on `kOk` and is never `0`.
  virtual Error ListBegin(const ListRequest& request, Cursor* cursor) = 0;
  virtual Error ListNext(Cursor cursor, ListPage* page) = 0;
  virtual Error ListEnd(Cursor cursor) = 0;

  /// One page of the conflict history (M7-1, #36). Never fails: a console that
  /// has never overwritten anything has an empty history, and that is the page
  /// the screen most needs to draw.
  ///
  /// **`[sync] conflict_show` is not consulted here.** It hides the screen, and
  /// the screen is the overlay's; an engine that filtered the answer would have
  /// the sysmodule lie about what it recorded. See conflict_log.hpp.
  virtual Error ListConflicts(const ConflictQuery& query, ConflictPage* page) = 0;

  /// Put the bytes an entry names back on the card.
  ///
  /// Never fails at the transport either, for `SetConfig`'s reason: what
  /// happened is a `conflicts::RestoreOutcome` inside a successful reply,
  /// because a `Result` that says the call failed takes the answer with it --
  /// and "the backup is gone" and "the sysmodule is not running" must not arrive
  /// at a screen as the same thing.
  virtual Error RestoreBackup(std::int64_t entry_id, conflicts::RestoreReport* report) = 0;
};

/// The service, as a plain C++ class with one method per command.
///
/// All of the logic the boundary has is here rather than in the sysmodule, so
/// it is testable natively: which of the five `SyncNow` outcomes applies, what
/// counts as paired, whether the folder map fits, and what the *effective*
/// enabled state is after a write. The Horizon side is buffers and a `Result`.
///
/// Const methods are the ones the overlay may call at any rate; they read a
/// snapshot and touch nothing.
class ServiceCore {
 public:
  explicit ServiceCore(Engine& engine);

  ServiceCore(const ServiceCore&) = delete;
  ServiceCore& operator=(const ServiceCore&) = delete;

  /// Command 0. Never fails, and its encoding is frozen (see `Command`).
  std::uint32_t GetInterfaceVersion() const;

  /// Command 1. Never fails: an unconfigured, unpaired, offline console has a
  /// status, and it is the one the overlay most needs to draw.
  Status GetStatus() const;

  /// Command 2. Never fails -- `config::LoadConfig` cannot refuse to produce a
  /// config, by design (config.hpp), and a map too large to send is a flag
  /// rather than an error.
  ConfigView GetConfig() const;

  /// Command 3. Never fails at the transport: what happened is `outcome` and
  /// why is `diagnostics`, both in the answer -- see `WriteOutcome` for why that
  /// is the only shape that works.
  ConfigResult SetConfig(const ConfigEdit& edit);

  /// Command 4. Same shape, same reason. `enabled` is the state read back off
  /// the config afterwards, not the one that was asked for.
  EnabledResult SetEnabled(bool enabled);

  /// Command 5. Hands work to the engine; never blocks.
  SyncOutcome SyncNow();

  /// Command 6. `kNotConfigured` when there is no server to pair with, and
  /// `kUnavailable` when the build has no transport to reach one with. The
  /// status it answers with is the attempt as it stands one instant later --
  /// `kStarting`, since the init request has not come back.
  Error StartPair(auth::PairingStatus* status);

  /// Command 7. Never fails; safe at any rate and from any thread.
  auth::PairingStatus GetPairState() const;

  /// Command 8.
  Error Unpair();

  /// Command 9. `position` is 1-based, set only on `kOk`.
  Error Enqueue(std::int64_t rom_id, std::int32_t* position);

  /// Command 10.
  Error Dequeue(std::int64_t rom_id);

  /// Commands 11-13. The page size is clamped here, not in the engine, so every
  /// implementation of `Engine` is held to the same cap.
  Error ListBegin(const ListRequest& request, Cursor* cursor);
  Error ListNext(Cursor cursor, ListPage* page);
  Error ListEnd(Cursor cursor);

  /// Command 14. The limit is clamped here rather than in the engine, so every
  /// implementation is held to the same cap.
  ConflictPage ListConflicts(const ConflictQuery& query);

  /// Command 15. Never fails; the outcome is in the answer.
  conflicts::RestoreReport RestoreBackup(std::int64_t entry_id);

  /// Command 16. The last `lines` lines of this process's log, clamped to
  /// `[0, kMaxLogLines]`. Never fails: a console that has logged nothing has an
  /// empty tail, and that is itself an answer -- it says the sysmodule is
  /// running and has not got as far as a tick.
  ///
  /// `0` is an empty tail with the total still filled in, which is the cheap
  /// question "has this console logged anything" -- not one line. The wire
  /// refuses `0` outright (`DecodeLogRequest`), so that answer is only ever an
  /// in-process caller's.
  ///
  /// **Answered from `log::Tail` rather than through `Engine`.** The log is
  /// process state, like `io::FileSync`, and not something the engine holds --
  /// so routing it through the seam would be a method every implementation of
  /// `Engine` had to write the same way, in front of a global they all share.
  /// It is also what keeps this command off the SD card: the tail is in memory,
  /// so a console whose card refused the log file still answers this
  /// (`log::FileSink` is silent about its own failures for the same reason).
  LogTail GetLog(std::int32_t lines) const;

 private:
  /// A pairing status cut to what a payload carries. See the definition: the
  /// verification URLs come off a server response, so their length is not this
  /// client's to assume, and both commands that answer one never fail.
  static auth::PairingStatus Bounded(auth::PairingStatus status);

  Engine& engine_;
};

/// The whole dispatch table: a command id and a request payload in, a response
/// payload out.
///
/// This is what the sysmodule's IPC thread calls, and it is why that binding
/// holds no logic -- the decode, the call, the encode and the size check are all
/// here, where they run on the host under `ctest`.
///
/// `response` is set on `kOk` and cleared otherwise -- no exceptions, which is
/// what the wire can actually carry: a `cmif` reply's data words never reach a
/// client whose `Result` says the call failed, so a payload attached to a
/// failure is a payload nobody can read. The two commands whose refusal has
/// something to say answer it as a `WriteOutcome` inside a successful reply
/// instead.
///
/// Clearing first means a caller that reads the buffer off a failed call gets an
/// empty one rather than the previous command's answer.
Error Dispatch(ServiceCore& core, std::uint32_t command_id, std::string_view request,
               std::string* response);

}  // namespace rommsync::ipc

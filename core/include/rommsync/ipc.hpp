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
// inner heap is `0x80000` with ~390 KiB left after the trimmed bsd transfer
// memory (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision), and that
// budget already owes a download buffer and the `state.db` baseline. Nothing
// here may grow with the size of the library: lists page (`ListBegin` /
// `ListNext` / `ListEnd`, M5-4), and the one payload that can legitimately grow
// with a *user's file* -- the folder map in `GetConfig` -- is truncated with a
// flag rather than refused, because `GetConfig` is documented never to fail.
//
// ## What never crosses this boundary
//
// No `device_code`, no bearer token, and no `server.url` inside an error string
// (docs/SECURITY.md). `config::Diagnostic` already keeps that last rule for the
// same reason -- a URL is the one configured field that can carry a credential
// -- and `Status` carries `configured` rather than the URL itself. `ipc.secrets`
// asserts it over every command rather than leaving it reviewed.
//
// ## What never blocks
//
// No command waits on the network. `SyncNow` and `StartPair` hand work to the
// engine thread and return; the overlay polls. That is the same contract
// `PairingSession::status()` already documents, and the reason it exists: an
// overlay redrawing at 60 Hz cannot be parked on a socket, and a sysmodule may
// not park a thread at all when the rule is that nothing blocks boot.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/json.hpp"
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
inline constexpr std::uint32_t kVersion = 1;

/// The ceiling on one encoded request and on one encoded response.
///
/// A few KiB, sized against the sysmodule heap rather than against a round
/// number -- see the header note. Both halves check it: the sysmodule so a
/// buffer it was handed cannot be overrun, the overlay so a sysmodule that
/// answered something enormous is a named refusal rather than an allocation.
inline constexpr std::size_t kMaxPayloadBytes = 8 * 1024;

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
};

/// Every command, in id order. The dispatch table and the documentation are both
/// checked against this rather than against a second copy of the list.
extern const Command kAllCommands[14];

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
/// The last four are the *transport's* errors rather than any one command's.
/// They are here rather than folded into `kInvalid` because they say something
/// different to whoever is reading a log: a command id this build does not know
/// means the two halves are different releases, and that is the sentence the
/// user needs -- not "invalid request".
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
};

/// Stable, log-friendly name -- `queue_full`. Never null.
const char* ToString(Error error);

/// A value and the reason there isn't one, matching `auth::Parsed`'s contract:
/// `value` is default-constructed on failure and must not be used.
template <typename T>
struct Decoded {
  T value{};
  json::Error error;
  bool ok() const { return error.ok(); }
};

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
  /// reason `state.db` uses them: the engine can format that and has no parser
  /// for it, and the overlay renders a relative time anyway.
  std::int64_t last_sync_at = 0;
  SyncResult last_sync_result = SyncResult::kNever;

  /// The last sync's counts. Not cumulative -- a running total nobody can reset
  /// is a number that stops meaning anything after a month.
  std::int64_t uploaded = 0;
  std::int64_t downloaded = 0;
  std::int64_t conflicts = 0;
  std::int64_t failed = 0;

  std::int64_t queue_depth = 0;
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
/// `config.ini` may hold a line of up to `config::kMaxConfigBytes`, and
/// `NormalizeServerUrl` bounds the *shape* of a URL but not its length -- so
/// without a bound here one absurd line could make `GetConfig` unable to answer,
/// and `GetConfig` is documented never to fail. Generous next to a real one:
/// RomM behind a reverse proxy at `/romm` is well inside it.
inline constexpr std::size_t kMaxServerUrlBytes = 512;

/// How many `config::Diagnostic`s one payload carries, and how long each of
/// their three text fields may be.
///
/// `config::kMaxDiagnostics` is 64 and a message may quote a path of
/// `config::kMaxPathLength`, so an unbounded list is several times the payload
/// cap on its own. These two numbers are what make `GetConfig` and `SetConfig`
/// fit *without* a trimming loop -- a payload whose size depends on the user's
/// file is a payload that is one bad `config.ini` away from not being sendable.
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

/// One `key = value` under one section, as `config.ini` spells it.
///
/// `SetConfig` is a list of these rather than a whole `Config` for two reasons:
/// a whole map does not fit the payload cap, and an overlay that sends back
/// everything it was shown would silently overwrite a section the running
/// sysmodule has since re-read. M5-3 (#30) owns what merging one means and what
/// it refuses; this header owns only the shape it arrives in.
struct ConfigAssignment {
  /// As written minus the brackets: `sync`, `platform.snes`. Never empty.
  std::string section;

  /// Never empty.
  std::string key;

  /// Exactly the text that would follow the `=`. An empty string is a legal
  /// value (a folder key set to nothing), which is why removal is `remove`
  /// rather than an empty `value`.
  std::string value;

  /// Drop the key instead of setting it -- what "reset to default" does.
  bool remove = false;
};

/// How many assignments one `SetConfig` may carry.
///
/// Bounded for `kMaxPayloadBytes`'s reason and one more: an edit is applied as a
/// unit, so an unbounded list is an unbounded amount of work between reading
/// `config.ini` and writing it back.
inline constexpr std::size_t kMaxAssignments = 64;

struct ConfigEdit {
  std::vector<ConfigAssignment> assignments;
};

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

/// Which list `ListBegin` opens. The per-item projection for each kind is M5-4's
/// (#31); this header owns the envelope they travel in.
enum class ListKind {
  kPlatforms,  ///< RomM's platforms, paged on this side (5.2.0 sends them whole)
  kRoms,       ///< filtered by `platform_id`, optionally by `search`
  kQueue,      ///< served from `queue.json`, never from RomM
};
const char* ToString(ListKind kind);

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

std::string EncodeDiagnostics(const std::vector<config::Diagnostic>& diagnostics);
Decoded<std::vector<config::Diagnostic>> DecodeDiagnostics(std::string_view text);

std::string EncodeConfigEdit(const ConfigEdit& edit);
Decoded<ConfigEdit> DecodeConfigEdit(std::string_view text);

/// `{"enabled":<bool>}` -- the request and the answer share a shape, because the
/// answer is the same question asked of the card afterwards.
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

  /// Command 3. `kInvalid` when the edit was refused, in which case nothing was
  /// written; `diagnostics` says which assignment and why either way.
  Error SetConfig(const ConfigEdit& edit, std::vector<config::Diagnostic>* diagnostics);

  /// Command 4. `effective` is the state as it stands **after** the attempt,
  /// read back rather than assumed: an overlay that drew the state it asked for
  /// would show a switch that did not move (#24).
  Error SetEnabled(bool enabled, bool* effective);

  /// Command 5. Hands work to the engine; never blocks.
  SyncOutcome SyncNow();

  /// Command 6. `kNotConfigured` when there is no server to pair with. The
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

 private:
  Engine& engine_;
};

/// The whole dispatch table: a command id and a request payload in, a response
/// payload out.
///
/// This is what the sysmodule's IPC thread calls, and it is why that binding
/// holds no logic -- the decode, the call, the encode and the size check are all
/// here, where they run on the host under `ctest`.
///
/// `response` carries whatever the command produced, and is cleared first, so a
/// caller that reads it off a failed call gets an empty buffer rather than the
/// previous command's answer.
///
/// A failing command usually produces nothing -- but `SetConfig` is the
/// exception and is the reason this is not "on `kOk` only": its `kInvalid` is a
/// refusal *with* the diagnostics that explain it, and those diagnostics are the
/// entire value of the refusal to a user editing settings on a console with no
/// keyboard. `SetEnabled` answers the same way, with the state that did not
/// move.
Error Dispatch(ServiceCore& core, std::uint32_t command_id, std::string_view request,
               std::string* response);

}  // namespace rommsync::ipc

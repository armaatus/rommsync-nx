// Step 1 of the sync loop, both halves: what the client *tells* RomM it has,
// and the plan RomM sends back.
//
// The request side is `SyncNegotiatePayload` and the `ClientSaveState` entries
// under it (M2-1). The answer side is `Negotiate`, which makes the one call the
// whole loop hangs off, and `SyncPlan`, which is what came back once it has been
// read strictly enough to act on -- an `action`, a classified `reason` and the
// server's own hash and timestamp, per save. Nothing here executes a plan; M2-5
// owns that.
//
// Every field name below is pinned to
// server/contract/romm-openapi-5.2.0.json (`ClientSaveState`) and was sent to a
// live 5.2.0 by server/probe_contract.py, whose replies are committed under
// server/contract/captures/. `sync.payload` re-reads that snapshot on every run,
// so a RomM that renames or re-types a field goes red here rather than on a
// console.
//
// Three of the seven fields are not what an obvious guess produces:
//
//   - `content_hash`, not `hash`, and an **MD5**: roms carry sha1/md5/crc, saves
//     are compared on MD5 alone. A SHA1 here matches nothing, so every
//     unchanged save negotiates as changed, forever.
//   - `file_size_bytes`, not `size`.
//   - `slot` is the pairing key with `rom_id`. A `null` slot means "archival,
//     manual upload" and is never paired with a slotted server save, so it
//     negotiates as `upload` on every tick even when the identical bytes are
//     already there.
//
// Only one of those mistakes announces itself. `file_size_bytes` is required, so
// guessing `size` is a 422 that names the field. `content_hash` is optional, so
// guessing `hash` is a **200**: RomM ignores the unknown key, reads the hash as
// absent, falls back to timestamps, and plans an `upload` for a save it already
// has -- on this tick and every tick after it. `sync.understood` demonstrates
// exactly that against a live server.
//
// So nothing here is built by concatenation and nothing is sent unchecked:
// `EncodeNegotiateRequest` refuses a save it cannot express faithfully and
// names the field, on the same reasoning as `json::Reader` -- a body the server
// accepts but reads as a different save is a bug that surfaces hours later, as
// a save that came back wrong.
//
// The answer is read to the same standard, and for a reason with the same shape:
// this is the one call where the *server* decides what happens to each save, so
// a field that silently defaulted is a decision the client did not make and
// cannot see. Hence `ParseNegotiateResponse` refuses a plan whole rather than
// returning a partial one, every `T | null` in the response is an `optional`
// rather than a sentinel, and an `action` or a `reason` string this client does
// not recognise is downgraded to `no_op` and reported -- because on a save, the
// default branch is the one that can overwrite it.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::sync {

/// An instant as RomM compares it: UTC, seconds.
///
/// `std::chrono::system_clock::from_time_t` is how an mtime off `stat` becomes
/// one, on Horizon and on the host alike.
using Timestamp = std::chrono::system_clock::time_point;

/// `2026-09-04T11:36:27Z` -- RFC 3339, UTC, whole seconds.
///
/// Sub-second precision is dropped *downwards*, never rounded: RomM stores these
/// at second granularity and compares strictly greater-than, so rounding a
/// 11:36:27.9 mtime up to :28 would claim a file is newer than it is and hand it
/// an arbitration it should have lost.
///
/// The offset is spelled `Z` rather than `+00:00`. RomM sends the latter and
/// accepts both; `Z` is what the probe sent and what the docs quote.
///
/// Empty when `when` is outside `[kMinTimestampSeconds, kMaxTimestampSeconds]`,
/// which no valid save reaches -- `Validate` refuses those first, so an empty
/// string cannot become a field in a request body.
std::string FormatTimestamp(Timestamp when);

/// `when` as whole seconds since the Unix epoch, rounded towards the past.
///
/// The bounds below are seconds rather than `Timestamp`s because
/// `system_clock::time_point` cannot be relied on to *hold* the upper one: its
/// tick is implementation-defined, and a nanosecond tick runs out in 2262.
std::int64_t UnixSeconds(Timestamp when);

/// The earliest instant a save may claim, one second past the epoch.
///
/// The epoch itself is excluded on purpose. It is what a console with an unset
/// clock reports and what a default-constructed `ClientSaveState` holds, and
/// both mean "nobody knows when this file changed" -- which the server would
/// read as "very old" and answer with a `download` over a save that may be the
/// only copy. Refusing it costs one save one tick; accepting it costs the save.
inline constexpr std::int64_t kMinTimestampSeconds = 1;

/// `9999-12-31T23:59:59Z`, the last instant `FormatTimestamp` can spell.
inline constexpr std::int64_t kMaxTimestampSeconds = 253402300799;

/// Hex digits in a `content_hash`. MD5, so 32 -- a SHA1's 40 is the mistake this
/// number exists to catch.
inline constexpr std::size_t kContentHashDigits = 32;

/// One local save, as `POST /api/sync/negotiate` reads it.
///
/// Required by the snapshot: `rom_id`, `file_name`, `updated_at`,
/// `file_size_bytes`. The other three are `T | null`, and their `null` means
/// something in each case -- see the field comments. They are `optional` rather
/// than empty-string-means-absent because "" and `null` are different values to
/// the server, and only one of them is a value.
struct ClientSaveState {
  /// The rom this save belongs to, matched locally
  /// (SYNC_PROTOCOL.md#step-0--matching-local-files-to-roms). RomM's ids are
  /// positive; a `0` here is an unmatched save, which has nothing to negotiate.
  std::int64_t rom_id = 0;

  /// The save file's own name, `Game (USA).srm` -- a name, never a path. The
  /// server joins it into a storage path, so a separator in it is a client
  /// asking the server to write somewhere else.
  std::string file_name;

  /// The pairing key, with `rom_id`. Pick a stable one (`autosave`) and keep it:
  /// changing it between ticks makes the same file a new save every time.
  ///
  /// `null` is legitimate and means archival/manual-upload, which never pairs
  /// with a slotted server save -- so a null-slot save uploads on every tick.
  /// That is a deliberate choice, not a default to fall into, which is why an
  /// empty string is refused rather than quietly treated as one or the other.
  std::optional<std::string> slot;

  /// Which emulator wrote it (`retroarch`, `tico`), when that is known.
  std::optional<std::string> emulator;

  /// **MD5** of the file's bytes, 32 lowercase hex digits. `null` when it has
  /// not been hashed, which the server reads as "cannot compare content" and
  /// falls back to timestamps for.
  ///
  /// Lowercase because the server compares the string it stored, and RomM
  /// stores `hexdigest()`. An uppercase digest of the same bytes matches
  /// nothing and makes every unchanged save look changed -- the same failure a
  /// SHA1 produces, from a subtler cause, so `Validate` refuses both.
  std::optional<std::string> content_hash;

  /// The local file's mtime, in UTC. What the server arbitrates on when the
  /// hashes cannot settle it.
  Timestamp updated_at{};

  /// The file's size in bytes. Reported, not enforced: the server does not
  /// reject a mismatch, so this is a hint for its UI rather than a checksum.
  std::int64_t file_size_bytes = 0;
};

/// The whole `POST /api/sync/negotiate` body.
struct SyncNegotiatePayload {
  /// Which device is syncing. The snapshot marks it optional -- a device-bound
  /// client token identifies the device on its own -- but send it anyway: it is
  /// the id the token response already carried, and being explicit is what
  /// keeps a token that is *not* device-bound from negotiating as nobody.
  std::optional<std::string> device_id;

  /// One entry per local save. Legitimately empty: a negotiation that reports
  /// nothing is how a client asks "what am I missing?", and it is the read-only
  /// shape the probe uses (docs/API_CONTRACT.md).
  std::vector<ClientSaveState> saves;
};

/// A request body, or the reason there isn't one. `body` is empty on failure and
/// must not be sent -- check `ok()`.
struct Encoded {
  std::string body;
  json::Error error;
  bool ok() const { return error.ok(); }
};

/// Everything wrong with one save that would make its negotiation mean
/// something other than what the client meant.
///
/// `error.field` is the JSON field name (`content_hash`), so a caller can say
/// which one; `EncodeNegotiateRequest` prefixes it with the entry's index.
/// Values are never quoted back, matching `json::Error`.
json::Error Validate(const ClientSaveState& save);

/// True for a string that can compare equal to a RomM save digest: 32
/// lowercase hex characters.
///
/// Both directions need it, and for the same reason in each: RomM stores
/// `hexdigest()` and compares the *string*, so a SHA1 or an uppercase MD5
/// matches nothing and makes an unchanged save look changed on every tick,
/// forever, with no other symptom. `Validate` refuses one on the way out;
/// `ParseNegotiateResponse` reports one on the way in -- the server's digest is
/// whatever some other client uploaded -- and M2-5 will not verify a download
/// against one it cannot compare.
bool IsContentHash(std::string_view value);

/// `Game (USA).srm` -> `.srm`, dot included. Empty when there is none.
///
/// **A leading dot is not an extension**: `.DS_Store` is a whole name, and
/// stripping there would leave nothing -- which matches every rom with an empty
/// name and none with a real one. Three places in the engine split a file name
/// on that rule (the scanner's `BaseName` and `SlotFor`, and the backup name
/// M2-5 writes) and they must not disagree about where the split is, so there
/// is one of it.
std::string_view ExtensionOf(std::string_view file_name);

/// True for a string that names one file and cannot redirect a join.
///
/// Both directions need it and they must not drift: the client sends a
/// `file_name` and an `emulator` that RomM pastes into a storage path
/// (`users/<user>/saves/<platform>/<rom>/<emulator>/<file_name>`), and the
/// server sends a `file_name` back that this client must never join into an SD
/// path. `.` and `..` redirect a join with no separator in them, which a
/// directory scan hands out for free.
///
/// Backslash is deliberately allowed: RomM joins POSIX paths, and a save that
/// came off a FAT volume is entitled to a backslash in its name.
bool IsSingleFileName(std::string_view value);

/// The request body for `POST /api/sync/negotiate`, or a named error.
///
/// The `device_id` and every save are validated first, and the first failure
/// stops the encode: a body that is missing the one save the tick was about is
/// worse than no body, since the plan that comes back looks complete.
Encoded EncodeNegotiateRequest(const SyncNegotiatePayload& payload);

// --- the answer ---------------------------------------------------------------

/// What the server decided to do about one save.
///
/// The wire spelling of `kNoOp` is `no_op`, with the underscore. It is also
/// where an `action` this build does not recognise lands: on a save, the default
/// branch is the one that can overwrite it, so an unknown action is pinned to
/// the only action that touches nothing (`ClassifyAction`).
enum class Action { kUpload, kDownload, kConflict, kNoOp };

/// The wire spelling, which doubles as the log-friendly name. Never null.
const char* ToString(Action action);

/// `upload` / `download` / `conflict` / `no_op`, and `kNoOp` for anything else.
///
/// `recognized`, when not null, is set false for that last case. It is the only
/// way a caller can tell a planned no-op from a server this build has stopped
/// understanding, because both arrive as `kNoOp` -- deliberately, since the
/// alternative is guessing at what to do with a save.
Action ClassifyAction(std::string_view action, bool* recognized = nullptr);

/// Which arbitration produced an operation.
///
/// This is the **complete** set 5.2.0 can emit, tabulated in
/// docs/API_CONTRACT.md#save-sync--negotiate--execute--complete. Only five of
/// the thirteen appear in server/contract/captures/, so `contract.captures`
/// cannot guard the rest and `sync.plan` holds this enum to the document
/// instead.
///
/// The client obeys `action` and not `reason` -- the server is the source of
/// truth (docs/SYNC_PROTOCOL.md). What the reason is for is everything around
/// obeying it: the two `conflict` reasons are different situations with
/// different things to tell a user, `kUntracked` is a `no_op` the user asked
/// for rather than one the bytes earned, and the three no-history reasons are
/// the ones that say an upload will need `overwrite=true`.
enum class Reason {
  /// A string this build does not know. First, so it is what a
  /// default-constructed operation carries.
  kUnrecognized,

  kClientOnly,                     ///< upload: no server save for that (rom_id, slot)
  kClientNewerNoHistory,           ///< upload: no sync record, the client's is later
  kClientNewer,                    ///< upload: only the client moved past the record
  kServerOnly,                     ///< download: the client never reported that pair
  kServerNewerNoHistory,           ///< download: no sync record, the server's is later
  kServerNewer,                    ///< download: only the server moved past the record
  kServerChangedClientMissing,     ///< download: the client dropped a pair that then changed
  kBothChanged,                    ///< conflict: both moved past the record
  kSameTimestampDifferentContent,  ///< conflict: no record, equal timestamps, different hashes
  kContentIdentical,               ///< no_op: the hashes match -- checked before anything else
  kNoChanges,                      ///< no_op: neither side moved past the record
  kAppearIdentical,                ///< no_op: no record, equal timestamps, hashes not comparable
  kUntracked,                      ///< no_op: the device's sync row is marked untracked
};

/// A stable slug for logs -- `both_changed`, not the server's sentence. Never
/// null.
const char* ToString(Reason reason);

/// The exact sentence RomM sends for `reason`, and what `ClassifyReason` matches
/// on. Empty for `kUnrecognized`, which is the absence of one.
const char* ReasonText(Reason reason);

/// Classify one `reason`. Anything unlisted is `kUnrecognized`: a server that
/// moved, not a case to guess at.
Reason ClassifyReason(std::string_view reason);

/// One entry of the plan -- `SyncOperationSchema`.
///
/// Five of the nine fields are `T | null` and are `optional` rather than
/// sentinels, because on this response a defaulted field is a decision the
/// client did not make: a `save_id` of `0` is a save id, and one that names no
/// save.
struct SyncOperation {
  /// What to do. `kNoOp` for an `action` this build does not recognise, with
  /// `known_action` false and the string kept in `action_text`.
  Action action = Action::kNoOp;

  /// True when the server's `action` was one of the four this build knows.
  bool known_action = true;

  /// Exactly what the server sent, for the log line that explains a downgrade.
  std::string action_text;

  std::int64_t rom_id = 0;

  /// The server's save row. Empty for an `upload` the server has nothing for --
  /// which is also the operation whose execution needs it least, and the one
  /// place an unconditional dereference would fire.
  std::optional<std::int64_t> save_id;

  /// **The server's name, never a path and never yours.** RomM renames a save
  /// on ingest (`probe.srm` -> `probe [2026-09-04_11-12-27].srm`) and every
  /// later operation echoes that, so writing this to the SD card produces a
  /// file no emulator loads. Match the operation back to a local file on
  /// `(rom_id, slot)` and keep your own path -- nothing in this header will
  /// join this value into one (docs/SYNC_PROTOCOL.md step 2).
  std::string file_name;

  /// The pairing key with `rom_id`. `null` is an archival save, which pairs
  /// with nothing.
  std::optional<std::string> slot;

  std::optional<std::string> emulator;

  /// The arbitration, classified. `kUnrecognized` for a sentence this build
  /// does not know; `reason_text` still carries it.
  Reason reason = Reason::kUnrecognized;

  /// Exactly what the server sent. The client acts on `action`, so this is for
  /// the log and for the sentence an overlay shows on a conflict.
  std::string reason_text;

  /// The server copy's timestamp and MD5, when it has a copy. Both are `null`
  /// on an `upload` of a save the server has never seen. RomM sends the
  /// timestamp with a numeric offset (`2026-09-04T11:36:27+00:00`), not the `Z`
  /// this client writes, so it is carried as text rather than parsed: nothing
  /// here compares it, and a parser that guessed the shape would be a second
  /// place for the two spellings to disagree.
  std::optional<std::string> server_updated_at;
  std::optional<std::string> server_content_hash;
};

/// A whole `SyncNegotiateResponse`.
struct SyncPlan {
  /// The session every upload and the final `complete` call is tied to.
  std::int64_t session_id = 0;

  /// Legitimately empty: a fully-synced device is answered with no operations,
  /// which is a plan, not an error (docs/SYNC_PROTOCOL.md step 1).
  std::vector<SyncOperation> operations;

  /// The server's own counts. Read rather than recomputed -- a disagreement
  /// between these and `operations` is drift worth being able to see, and
  /// recomputing them here would hide it.
  std::int64_t total_upload = 0;
  std::int64_t total_download = 0;
  std::int64_t total_conflict = 0;
  std::int64_t total_no_op = 0;

  /// One line per operation this build did not fully understand: an unknown
  /// `action` (downgraded to `no_op`), an unknown `reason`, or a `file_name`
  /// that is not a single path component.
  ///
  /// `core/` has no logger -- logging is the sysmodule's, behind an interface
  /// (docs/ARCHITECTURE.md) -- so the parse hands the lines up rather than
  /// swallowing them. A silent downgrade is exactly the thing that must not be
  /// silent: it is the client quietly declining to sync a save it no longer
  /// understands, on every tick, with nothing to say why.
  ///
  /// These do quote the server's strings, which `json::Error` refuses to do for
  /// a value. The reason that rule exists does not apply here: a rejected body
  /// may still be carrying an access token, and a plan never carries one.
  std::vector<std::string> warnings;
};

/// Parse a 200 body from `POST /api/sync/negotiate`.
///
/// Whole or not at all. One unreadable operation fails the plan rather than
/// being skipped, because a plan with a save missing from it looks exactly like
/// a plan for a device that is already in sync -- and the save that got dropped
/// is the one nobody will hear about again.
///
/// An unrecognised `action` or `reason` is *not* an unreadable operation: those
/// are a server that moved, and refusing the whole tick over a `no_op` nobody
/// had to act on would be the client breaking itself on a RomM upgrade. They
/// are downgraded, recorded in `SyncPlan::warnings`, and reported.
auth::Parsed<SyncPlan> ParseNegotiateResponse(std::string_view body);

/// Why a negotiation did not produce a plan.
///
/// The three the issue for this call names by hand are here rather than folded
/// into a general "network" -- they are the ones with different remedies, and
/// each is reachable without anything being broken: a device deleted in RomM's
/// web UI, sync switched off for it there, and a token revoked. None of the
/// three gets better by retrying, and only the first and last are fixed by
/// pairing again.
enum class NegotiateError {
  kNone,
  kUnusablePayload,  ///< a save could not be sent faithfully; nothing was sent
  kNotRegistered,    ///< the token names no device, so there is nothing to negotiate for
  kUnauthorized,     ///< 401 -- revoked. `expires_at` is null, so there is nothing to refresh
  /// 403 -- the token is real and was not granted what this call needs.
  ///
  /// Not a revocation, and kept apart from one for that reason: RomM approves
  /// what the *user* ticked, which need not be what was requested, so a 403 here
  /// is a scope missing from an otherwise working pairing
  /// (docs/AUTH.md#scopes-to-request). Telling that user their token was revoked
  /// sends them looking for something that did not happen.
  kForbidden,
  kNoSuchDevice,     ///< 404 -- the device was deleted in RomM's web UI
  kSyncDisabled,     ///< 400 "Sync is disabled for this device" -- the user's own switch
  kRejected,         ///< another 4xx; a 422 names the field the body got wrong
  kUnreachable,      ///< the exchange never completed -- offline, stalled, dropped
  kServerError,      ///< 5xx, or the 429/408 a rate limiter or a proxy answers with
  kMalformed,        ///< a 2xx that is not a plan -- truncated, or a shape that moved
};

/// Stable, log-friendly name. Never null.
const char* ToString(NegotiateError error);

/// Whether the same call could succeed later. Only the two that say nothing
/// about the pairing or the payload -- the same split
/// `auth::ShouldRetry` makes, and for the same reason.
bool ShouldRetry(NegotiateError error);

/// Whether the remedy is to pair this console again. Deliberately disjoint from
/// `ShouldRetry`: sending a user to a pairing screen over a dropped connection
/// throws away a working pairing.
///
/// `kForbidden` is in it, and the sentence it earns is not the same one:
/// re-pairing is where a user approves the scope that is missing, so the remedy
/// really is to pair again -- but "pair again and approve the scopes sync needs"
/// is a different instruction from "your pairing is gone", and only the second
/// is true of a 401. The client is supposed to read `scopes` back off the token
/// and never reach here (docs/AUTH.md); this is the branch for when it does.
///
/// It classifies one answer; it is not an instruction to act on the first one.
/// A single 401 can come from something in *front* of RomM having a bad minute,
/// and the token that got it usually still works -- `harness.expired` shows
/// exactly that against a live server. Counting consecutive ones before
/// discarding a pairing is the caller's, the way `PairingConfig` counts
/// rejected polls.
bool NeedsPairing(NegotiateError error);

/// How hard one negotiation tries.
struct NegotiateOptions {
  /// Ceiling on a single request. A tick that hangs is a tick that never ends,
  /// and nothing may block boot (CLAUDE.md).
  std::chrono::milliseconds timeout = http::kDefaultTimeout;

  /// Requests one negotiation may spend, including the first. Only a
  /// `ShouldRetry` failure spends another.
  int max_attempts = 3;

  /// The first delay before a retry, doubled per consecutive retryable failure
  /// and capped at `max_backoff`.
  std::chrono::milliseconds backoff{1'000};
  std::chrono::milliseconds max_backoff{8'000};

  /// How the caller waits between attempts.
  ///
  /// Injected rather than called directly, for the reason `PairingSession` takes
  /// a clock: a sysmodule may want to park on its own primitive, and a test that
  /// had to spend the backoff to prove there was one would be a test nobody
  /// runs.
  ///
  /// **Null means the default sleep, not "do not wait".** A caller that wants a
  /// single attempt says so with `max_attempts`, because that is the field the
  /// question is about -- a budget of three that quietly spends one is worse
  /// than either honest answer, and the rule is that every network call retries
  /// with backoff (CLAUDE.md). The wait happens on the calling thread, which is
  /// a sync tick's worker and never boot.
  std::function<void(std::chrono::milliseconds)> wait;
};

/// A plan, or the reason there isn't one. `plan` is left default-constructed on
/// failure and must not be used -- check `ok()`.
struct Negotiation {
  SyncPlan plan{};
  NegotiateError error = NegotiateError::kNone;

  /// For logs and for the overlay. Names the status and the reason, never the
  /// token.
  std::string message;

  /// Requests actually sent, and the backoff actually asked for. Both are here
  /// so a caller can see a tick that cost three attempts, and so the retry is
  /// checkable without waiting it out.
  int attempts = 0;
  std::chrono::milliseconds waited{0};

  bool ok() const { return error == NegotiateError::kNone; }
};

/// `POST /api/sync/negotiate`, and the plan it answers with.
///
/// The one call where the server, not the client, decides what happens to each
/// save. `token.device_id` is sent as `device_id` even though the snapshot marks
/// it optional: a device-bound token identifies the device on its own, but a
/// token that is *not* device-bound would otherwise negotiate as nobody, and
/// every save would come back as a first encounter. A record with no device id
/// is `kNotRegistered` rather than a request with a `null` in it.
///
/// `saves` may be empty. That is not a degenerate call -- it is the client
/// asking what it is missing, and the server answers with every save this
/// device has no history for.
///
/// Nothing is executed here; the plan is data. M2-5 owns acting on it.
Negotiation Negotiate(http::HttpClient& client, const auth::StoredToken& token,
                      const std::vector<ClientSaveState>& saves,
                      const NegotiateOptions& options = {});

}  // namespace rommsync::sync

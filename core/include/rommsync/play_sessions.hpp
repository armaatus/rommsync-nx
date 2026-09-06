// How long a rom was played on the console, and how that reaches RomM.
//
// M7-4, and the most droppable thing in the client: it writes no save, touches
// no baseline, and a failure anywhere in here must cost nothing but the play
// time it was carrying. A play-session error that failed a sync tick would be
// an optional feature holding hard rule 2's machinery hostage.
//
// ## The console cannot see which rom is running, and this is the honest answer
//
// Horizon will name the foreground *application* -- `pmdmntGetApplicationProcessId`
// plus `pminfoGetProgramId` -- but on this console that application is RetroArch
// or Tico, never the rom. No Horizon API says which file an emulator loaded, so
// a title id cannot produce a `rom_id`, and neither service is in
// `sysmodule/sys-rommsync.json` today. None of it is testable off a console
// either, which hard rule 1 settles: it is not in this build.
//
// What is here instead is derived from the saves. A save whose mtime moved
// between two ticks means that rom was played, the two ticks bound the window,
// and `duration_ms` is therefore an **upper bound rather than a measurement**.
// `DeriveSessions` says so in one place and the rest of the module is about not
// losing the result:
//
//   - the window's end is the save's own mtime, which is a real observation --
//     the moment the emulator wrote -- rather than the tick that noticed it;
//   - its start is the later of the previous tick and the save's previous
//     mtime, which is the tightest lower bound this client actually has;
//   - a save whose mtime did not move produces nothing at all.
//
// A session that could not be attributed is still worth sending: `rom_id` is
// nullable in the pinned schema. This client always has one, because the
// attribution *is* a save that belongs to a rom -- but the field stays optional
// so a later foreground detector can fill it in without a format change.
//
// ## Wall clock only
//
// Everything here is `std::chrono::system_clock`. `steady_clock` cannot be
// compared against an mtime and does not survive a suspend, and a console whose
// clock was never set produces timestamps RomM would file under 1970 forever --
// so `kEarliestPlausibleSeconds` refuses those outright rather than sending
// them. That bound is this module's, deliberately tighter than
// `sync::kMinTimestampSeconds`: a save's mtime with a wrong year is still the
// only record of that save, where a play session with a wrong year is a row in
// a library's play-time totals that nobody can tell from a real one.
//
// ## Where a session waits
//
// On the card, in `play.db`, bounded and oldest-dropped -- `conflicts.db`'s
// shape and its reader, for its reason: a second format under
// `/config/rommsync` would be a second place to get a truncation wrong. An
// offline console fills it and stops growing; a reboot does not lose it.
//
// The ordinary carrier is the sync tick, which already makes the request:
// `sync::FinishTick` puts them in `SyncCompletePayload.play_sessions[]` and no
// second call is spent on battery. `IngestSessions` is the other route,
// `POST /api/play-sessions`, for flushing without a sync.
//
// **Both are guarded by scopes this client already has.** The ingest endpoint
// declares `roms.user.write` and the completion declares `devices.write`, both
// of which are in `auth::MinimumScopes()`. `me.write` has nothing to do with
// play sessions -- it guards `PUT /api/users/{id}`, the client-token family and
// the device approve/deny pair -- and is still not requested (docs/AUTH.md#scopes).
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::play {

/// Where the buffer lives, SD-root absolute -- `state.db`'s and `conflicts.db`'s
/// neighbour, for `conflicts::kHistorySdPath`'s reason.
inline constexpr const char* kBufferSdPath = "/config/rommsync/play.db";

/// The same file's name on its own, for a caller that has the directory.
inline constexpr const char* kBufferFileName = "play.db";

/// The first line of a well-formed file, `<magic> <version> <next id> <last seen>`.
inline constexpr const char* kFormatMagic = "rommsync-play-sessions";
inline constexpr int kFormatVersion = 1;

/// How many unsent sessions are kept, and the largest file that will be read.
///
/// Sized the way `conflicts::kMaxEntries` is -- against what is left of the
/// sysmodule's heap beside `state.db` and a transfer
/// (`sysmodule/source/main.cpp`) -- and deliberately small, because the thing
/// this bound protects against is an offline console appending one row per tick
/// forever. At the default half-hour interval, forty-eight rows is a day of
/// play nobody could sync, which is more than a play-time total ever needs.
///
/// A row is a rom id, a slot, two timestamps and a duration: ~140 bytes, so
/// `kMaxSessions` of them fit inside `kMaxBufferBytes` with room over. The
/// *file* bound is what the reader enforces (`LoadBuffer`), so the two move
/// together.
inline constexpr std::size_t kMaxSessions = 48;
inline constexpr std::size_t kMaxBufferBytes = 8 * 1024;

/// How many diagnostics a load or a derivation keeps.
/// `state::kMaxDiagnostics`'s reasoning: a card that produced four hundred bad
/// rows must cost a count and a handful of lines, not four hundred strings on a
/// sysmodule heap.
inline constexpr std::size_t kMaxDiagnostics = 16;

/// How long a `save_slot` may be before it is refused.
///
/// Refused rather than shortened, which is where this parts company with
/// `conflicts::Shorten`: a slot is a *key*, and half of one names a save that
/// does not exist. A slot this long is a bug upstream rather than a user's long
/// game title -- `scan::SlotFor` builds them from an emulator name and an
/// extension.
inline constexpr std::size_t kMaxSlotBytes = 64;

/// The earliest instant a play session may claim: `2020-01-01T00:00:00Z`.
///
/// Deliberately far tighter than `sync::kMinTimestampSeconds`, which is one
/// second past the epoch and exists to catch an unset clock reporting *zero*.
/// A Switch whose clock was never set does not report zero -- it reports
/// Horizon's own 2000-01-01 -- so the save bound would pass it straight
/// through. A save may be sent with a wrong year and still be arbitrated
/// correctly on content; a play session with a wrong year is play time in a
/// total a user reads, indistinguishable from the real thing and impossible to
/// find afterwards. Anything before this project existed is refused.
inline constexpr std::int64_t kEarliestPlausibleSeconds = 1'577'836'800;

// --- deriving a session from the saves ----------------------------------------

/// One save as a tick saw it: enough to tell whether it moved, and nothing more.
///
/// Deliberately not `scan::SaveFile` or `state::SaveRecord`. Either would work
/// and both would drag the scanner or the baseline in behind them, and what this
/// module needs is three fields that both of those already have -- so the caller
/// projects, and this stays a module a test can drive with a literal.
struct SaveObservation {
  std::int64_t rom_id = 0;

  /// The pairing key with `rom_id` (`scan::SaveFile::slot`). Empty for an
  /// archival save, which pairs on the rom alone.
  std::string slot;

  /// mtime, whole seconds, UTC.
  sync::Timestamp mtime{};
};

/// What one derivation produced, and what it declined to.
struct Derivation {
  /// In the order the current observations were given, so two runs over an
  /// unchanged card produce the same list.
  std::vector<sync::PlaySession> sessions;

  /// Saves whose mtime moved and which still produced nothing: an mtime before
  /// the window, one in the future, or the bound below. **Not an error** --
  /// each costs one session of play time and nothing else.
  std::size_t skipped = 0;

  /// One line per skip, bounded by `kMaxDiagnostics`, plus one for a clock this
  /// refused to believe.
  std::vector<std::string> warnings;
};

/// Bounds on one derivation.
struct DeriveOptions {
  /// The most sessions one tick will produce.
  ///
  /// A card whose every save changed at once -- a restore, a card swap, a
  /// `touch` over the folder -- would otherwise produce one session per save,
  /// and the buffer that holds them is `kMaxSessions` deep. Producing more than
  /// it can hold would push out sessions that are already there in favour of
  /// ones this tick cannot explain either.
  std::size_t max_sessions = kMaxSessions;
};

/// Turn "what the saves looked like last tick" and "what they look like now"
/// into the sessions that must have happened in between.
///
/// `previous` is the last tick's observations -- `state.db`'s rows, projected --
/// and `last_seen` is when that tick ran, which is what the buffer carries in
/// its header. `now` is this tick's wall clock, and is used only as a ceiling:
/// a save whose mtime is in the future is skipped rather than believed.
///
/// **The first tick after a boot with no buffer produces nothing**, silently,
/// and it must: with no `last_seen` there is no lower bound at all, and every
/// save on the card would come back as one enormous session. That is the empty
/// result with no warning; a clock that is unset or has gone backwards is the
/// empty result *with* one.
Derivation DeriveSessions(const std::vector<SaveObservation>& previous,
                          const std::vector<SaveObservation>& current,
                          sync::Timestamp last_seen, sync::Timestamp now,
                          const DeriveOptions& options = {});

// --- the buffer on the card ---------------------------------------------------

/// One buffered session and the id that addresses it.
///
/// The id never reaches the server: it is how `Reconcile` says which rows an
/// answer released, and it comes off a counter in the file header so an id that
/// fell off the end can never name a later session.
struct BufferedSession {
  std::int64_t id = 0;
  sync::PlaySession session;
};

/// One row of `play.db`. The id is in the row and never in a request body.
std::string SerializeRow(const BufferedSession& buffered);

/// Read one back, or say what was wrong with it. `why` is set on false.
///
/// Strict in `json::Reader`'s way, and held to `sync::Validate` on top: a row
/// that would be refused on the way out is a row worth dropping on the way in,
/// because keeping it would wedge every flush after it behind a body that can
/// never be built.
bool ParseRow(const json::Value& object, BufferedSession* out, std::string* why);

/// The whole file: the header line, then one row per session, oldest first,
/// ending in a newline.
///
/// `last_seen` rides in the header beside `next_id` because it is this module's
/// half of the window `DeriveSessions` needs, and nothing else on the card
/// records when a tick *looked*. `state.db` holds mtimes, not observations.
std::string SerializeBuffer(const std::vector<BufferedSession>& sessions, std::int64_t next_id,
                            sync::Timestamp last_seen);

/// A buffer and everything wrong with the file it came from.
struct LoadedBuffer {
  /// Oldest first, so `sessions().front()` is the next one to send.
  std::vector<BufferedSession> sessions;

  /// The id the next session takes. At least `1`, and greater than every id
  /// held.
  std::int64_t next_id = 1;

  /// When the tick that wrote this file looked at the card. At the epoch when
  /// there is no file, which `DeriveSessions` reads as "no window yet".
  sync::Timestamp last_seen{};

  /// In the order they were found, bounded by `kMaxDiagnostics`.
  std::vector<std::string> diagnostics;
};

/// Parse the contents of a `play.db`. Pure: no filesystem, no clock.
///
/// **A row that will not read is skipped and the rest are kept** --
/// `conflicts::ParseHistory`'s rule rather than `state::ParseBaseline`'s,
/// because these rows are independent of each other and each one is play time
/// that is nowhere else. A missing or wrong header line is still fatal: those
/// bytes are not this file, and a `last_seen` read out of a corrupt header
/// would silently bound every session of the next tick.
LoadedBuffer ParseBuffer(std::string_view text);

/// Read `path` and parse it.
///
/// A *missing* file falls back to `io::PreviousPathFor(path)`, the recovery
/// `state::LoadBaseline` and `conflicts::LoadHistory` both make, and for their
/// reason: the one moment `play.db` legitimately does not exist is the window
/// between `io::WriteAtomically`'s two renames.
LoadedBuffer LoadBuffer(const std::string& path);

/// Why writing the buffer did not work. Mirrors `conflicts::StoreError`.
enum class StoreError {
  kNone,
  kTooLarge,      ///< the serialized file exceeds `kMaxBufferBytes`
  kOpenFailed,    ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,   ///< the bytes did not all reach the card; the destination is untouched
  kCommitFailed,  ///< the rename onto `path` failed; see atomic_file.hpp
};
const char* ToString(StoreError error);

struct StoreResult {
  StoreError error = StoreError::kNone;

  /// For logs. Names the path and what went wrong, never a save's contents.
  std::string message;

  /// Sessions that fell off the front to stay inside `kMaxSessions`. Not an
  /// error: it is the bound doing its job on a console that has been offline.
  std::size_t dropped = 0;

  /// Sessions that were handed over and not stored, because `sync::Validate`
  /// refuses them or their slot does not fit.
  ///
  /// **Not an error either**, which is `state::StoreResult::skipped`'s stance
  /// and its reason: the rest of the batch was written, and a caller reading
  /// `ok()` should see "the file is on the card" rather than "one session in
  /// forty-eight was malformed". `play::DeriveSessions` cannot produce one, so
  /// a non-zero count here means a caller built a session by hand.
  std::size_t unusable = 0;

  bool ok() const { return error == StoreError::kNone; }
};

/// The unsent sessions as the card holds them.
///
/// **Every mutation writes the file.** `conflicts::History`'s rule and its
/// reason one step weaker: what is lost here is play time rather than the name
/// of a backup, but the write is one small atomic file per tick and the console
/// this protects is the one that is switched off between ticks -- which is
/// every console.
///
/// One instance per engine, and nothing here is thread-safe: it is touched from
/// whatever drives a tick and the caller owns that seam, exactly as
/// `conflicts::History` states it.
class Buffer {
 public:
  /// `path` is the resolved host path, not an SD-root one -- the records under
  /// `/config/rommsync` are read and written through `io::` throughout
  /// (`conflicts::History`'s contract, for its reason).
  explicit Buffer(std::string path);

  const std::string& path() const { return path_; }

  /// Read the file. Returns what was wrong with it, for a log.
  std::vector<std::string> Load();

  /// Oldest first.
  const std::vector<BufferedSession>& sessions() const { return sessions_; }
  std::size_t size() const { return sessions_.size(); }
  bool empty() const { return sessions_.empty(); }

  /// When the tick that last wrote this file looked at the card. At the epoch
  /// until one has.
  sync::Timestamp last_seen() const { return last_seen_; }

  /// Append `sessions`, stamp `seen_at` as the new window edge, and write.
  ///
  /// **The stamp happens even when `sessions` is empty**, and that is the point:
  /// a tick that found nothing still looked, and a window that only moved on
  /// ticks that produced something would hand the next session a start time
  /// hours older than the truth.
  ///
  /// A session `sync::Validate` refuses is not stored and does not stop the
  /// others -- it is counted in `StoreResult::unusable`, which is not an error.
  /// The oldest fall off the front once there are more than `kMaxSessions`.
  StoreResult Record(const std::vector<sync::PlaySession>& sessions, sync::Timestamp seen_at);

  /// The first `limit` sessions, oldest first -- what a flush sends.
  std::vector<BufferedSession> Pending(std::size_t limit) const;

  /// Drop the sessions with these ids and write. Ids that are not held are
  /// ignored, which is what makes a release safe to repeat.
  StoreResult Release(const std::vector<std::int64_t>& ids);

 private:
  /// Write the whole file. The caller has already changed the members.
  StoreResult Persist();

  std::string path_;
  std::vector<BufferedSession> sessions_;
  std::int64_t next_id_ = 1;
  sync::Timestamp last_seen_{};
};

// --- what an ingest answer means ----------------------------------------------

/// What one answer released, and what it did not.
struct Reconciliation {
  /// The buffered ids the server has now accounted for. Hand these to
  /// `Buffer::Release`.
  std::vector<std::int64_t> release;

  std::size_t created = 0;
  std::size_t duplicate = 0;

  /// Entries RomM answered `error`. **They are released too**, and this is the
  /// one judgement call in the module: a refusal is deterministic -- the body
  /// was already `sync::Validate`-clean, so what RomM refused is the *value* --
  /// and a session kept at the head of the buffer would be re-sent and refused
  /// on every tick forever, holding a slot that would otherwise take a session
  /// RomM will accept. Losing one session of play time is the cheaper failure,
  /// and it is named in `warnings` rather than silent.
  std::size_t refused = 0;

  /// Sessions that were sent and that no result named. Kept buffered: an
  /// answer that says nothing about an entry is not an answer that it landed.
  std::size_t unanswered = 0;

  /// One line per refusal and per result that named an index nothing was sent
  /// for, bounded by `kMaxDiagnostics`.
  std::vector<std::string> warnings;
};

/// Work out which of `sent` the answer accounted for.
///
/// `sent` is what `Buffer::Pending` returned, in the order it was encoded --
/// `PlaySessionIngestResult::index` is an index into that array, and it is read
/// off the result rather than assumed to be the result's own position.
Reconciliation Reconcile(const std::vector<BufferedSession>& sent,
                         const sync::PlaySessionIngest& ingest);

// --- the standalone ingest ----------------------------------------------------

/// Why an ingest did not happen. The same shape as `sync::CompleteError` and
/// split on the same question: what a caller would *do* about it.
enum class IngestError {
  kNone,
  kNotRegistered,   ///< the token names no server
  kNothingToSend,   ///< there were no sessions; no request was made
  kUnauthorized,    ///< 401 -- revoked
  kForbidden,       ///< 403 -- a scope this pairing was not granted
  kRejected,        ///< another 4xx; a 422 names the field the body got wrong
  kUnusablePayload, ///< a session could not be sent faithfully; nothing was sent
  kCanceled,        ///< `CallPolicy::cancel` fired
  kUnreachable,     ///< the exchange never completed -- offline, stalled, dropped
  kServerError,     ///< 5xx, or the 429/408 a rate limiter or a proxy answers with
  kMalformed,       ///< a 2xx that is not an ingest response
};

/// Stable, log-friendly name. Never null.
const char* ToString(IngestError error);

/// Whether the same call could succeed later. The same two members
/// `sync::ShouldRetry` picks, for its reason.
bool ShouldRetry(IngestError error);

/// What this error says about the credentials, for `auth::Gate`.
///
/// **`kNone` is the only `kAccepted` here**, which is narrower than
/// `sync::AnswerOf(CompleteError)`: this endpoint has no refusal gated on
/// RomM's own `detail` text, so there is nothing else that proves the token was
/// read.
auth::Answer AnswerOf(IngestError error);

/// The spelling the ingest takes. An alias rather than a struct of its own, for
/// `sync::CompleteOptions`'s reason.
using IngestOptions = sync::CallPolicy;

/// An ingest answer, or the reason there isn't one. `value` is left
/// default-constructed on failure and must not be used -- check `ok()`.
struct IngestOutcome {
  sync::PlaySessionIngest value{};
  IngestError error = IngestError::kNone;

  /// For logs and for the overlay. Names the status and the reason, never the
  /// token.
  std::string message;

  int attempts = 0;
  std::chrono::milliseconds waited{0};

  bool ok() const { return error == IngestError::kNone; }
};

/// `POST /api/play-sessions`, and what RomM did with each entry.
///
/// **The second-choice route, and nothing in the sysmodule calls it yet.** The
/// sync tick already makes a request that carries these (`sync::FinishTick`),
/// and a console on battery must not spend a second one -- so the engine uses
/// the completion and only the completion. This is the route for flushing
/// *without* a sync, which is the thing a console needs when its negotiations
/// keep failing while the link is fine, and it is what an overlay "send my play
/// time now" would call. It is here because M7-4's acceptance asks for it and
/// because a buffer with only one drain has only one way to stay stuck; it is
/// exercised end to end against the real RomM by `play.ingest`.
///
/// The endpoint declares `roms.user.write`, which `auth::MinimumScopes()`
/// already requests. **Not `me.write`** -- see the header note.
///
/// `token.device_id` is sent as `device_id`. The field is nullable in the
/// schema and this client always has one: a device-bound token identifies the
/// device anyway, but the sessions are read back by `device_id` and one sent
/// without it belongs to the user rather than to this console.
///
/// A retry re-sends the identical body and is answered `duplicate`, which
/// `sync::Ingested` reads as success. That is what makes it safe.
IngestOutcome IngestSessions(http::HttpClient& client, const auth::StoredToken& token,
                             const std::vector<sync::PlaySession>& sessions,
                             const IngestOptions& options = {});

/// The request body for `POST /api/play-sessions`, or a named error.
sync::Encoded EncodeIngestRequest(std::string_view device_id,
                                  const std::vector<sync::PlaySession>& sessions);

/// What one flush did, end to end.
struct FlushReport {
  IngestOutcome sent;
  Reconciliation reconciled;

  /// What `Buffer::Release` did. A release that could not be written costs a
  /// duplicate on the next flush and nothing else, which is exactly what
  /// `sync::Ingested` exists for.
  StoreResult released;

  /// Sessions the buffer handed over. Zero means there was nothing to do, which
  /// is `IngestError::kNothingToSend` and not a failure.
  std::size_t attempted = 0;

  bool ok() const { return sent.ok(); }
};

/// Send what the buffer holds and drop what the server accounted for.
///
/// Nothing is released without an answer that names it. A flush that never
/// reached the server leaves the buffer exactly as it was, and the next one
/// sends the same sessions -- which RomM answers `duplicate` if the first
/// attempt secretly landed.
FlushReport Flush(http::HttpClient& client, const auth::StoredToken& token, Buffer* buffer,
                  const IngestOptions& options = {});

}  // namespace rommsync::play

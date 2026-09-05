// `sdmc:/config/rommsync/queue.json` -- what is still to be downloaded, and the
// worker that drains it.
//
// The overlay's part of a download is one call: it hands the sysmodule a rom id
// and walks away (docs/ARCHITECTURE.md#data-flow-a-download). Everything after
// that is here -- resolving the id against RomM, turning it into a path with
// `config::Config::DestinationFor`, moving the bytes, and writing down what
// happened so a console that loses power mid-transfer picks the same entry back
// up rather than starting over or forgetting it.
//
// Three properties are what this module is for, and each one is a test:
//
//   - **An entry is never lost.** A bad network minute increments `attempts`,
//     leaves the entry `kQueued` and tries again with backoff. Nothing here
//     drops an entry because a request failed; only `Remove` and `Clear` do.
//   - **The file is written after every state transition**, so the state on the
//     card and the state in memory never disagree by more than one `rename`.
//     An entry left `kActive` by a power cut is *resumed* from its `.part`.
//   - **A corrupt `queue.json` costs the queue, never the boot.** Like
//     `config.ini` and `state.db`: an empty queue plus a named diagnostic, and
//     `io::PreviousPathFor` consulted when the file is merely missing.
//
// ## What is not here
//
// Verifying a download against the rom's `sha1_hash` is M3-3 (#20) -- so is the
// finer half of resume, and `DownloadsConfig::verify_hash`. `kVerifying` exists
// and round-trips because the state belongs to the format rather than to the
// step that will use it; this issue's worker goes `kActive` -> `kDone`.
// Byte-level progress over IPC is M3-5 (#22), which reads `bytes_done` from
// here. What to do with a disc set beyond refusing it is M3-4 (#21).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/rom_index.hpp"

namespace rommsync::download {

/// The file the queue lives in, relative to `sdmc:/config/rommsync/`.
inline constexpr const char* kQueueFileName = "queue.json";

/// The `format` string and `version` number a well-formed file opens with.
inline constexpr const char* kFormatMagic = "rommsync-queue";
inline constexpr int kFormatVersion = 1;

/// How many entries the queue holds, and the largest `queue.json` that will be
/// read.
///
/// Bounds for `config::kMaxConfigBytes`' reason -- this runs on a sysmodule heap
/// with ~390 KiB free (docs/DEVELOPMENT.md) over a FAT32 card that gets yanked
/// mid-write, so a corrupt directory entry claiming four gigabytes has to be a
/// named refusal rather than a `bad_alloc`.
///
/// The two are sized against each other rather than picked as round numbers: an
/// entry whose every string is at its own maximum -- a `fs_name` and a
/// `destination` at `config::kMaxPathLength`, a `message` at
/// `kMaxMessageChars` -- serialises to about 2 KiB, so `kMaxQueueEntries` of
/// them fit inside `kMaxQueueBytes` with room to spare. `download.bounds`
/// asserts exactly that, because a writer that can produce a file the reader
/// discards would cost a console its whole queue at the next boot.
///
/// Sixty-four is not a small queue: these are roms, so a full one is several
/// gigabytes of transfer. A user who wants more queues more when these drain.
inline constexpr std::size_t kMaxQueueEntries = 64;
inline constexpr std::size_t kMaxQueueBytes = 160 * 1024;

/// The longest `QueueEntry::message`. Long enough for the sentences
/// `config::RomDestination::reason` and `http::Result::message` produce, short
/// enough that `kMaxQueueEntries` of them are still a bounded file.
inline constexpr std::size_t kMaxMessageChars = 200;

/// How many diagnostics a load reports before the rest are dropped.
inline constexpr std::size_t kMaxDiagnostics = 16;

/// The largest `QueueEntry::attempts` a file may claim.
///
/// A bound because `attempts` is read out of the file into an `int`, and a
/// corrupt card region reading `"attempts":9223372036854775807` would otherwise
/// be a narrowing conversion no check afterwards can undo. A million failed
/// requests for one rom is already past anything a console reaches.
inline constexpr std::int64_t kMaxAttemptsRecorded = 1'000'000;

/// Where one entry has got to.
///
/// `kDone`, `kFailed` and `kSkipped` are terminal and stay in the file: the
/// overlay's queue screen (#31) is served from `queue.json` with the server
/// down, and an entry that vanished the moment it failed would leave a user
/// with a rom that never arrived and nothing that says why.
enum class QueueState {
  kQueued,     ///< waiting for the worker
  kActive,     ///< bytes are moving, or were when the power went out
  kVerifying,  ///< the body is down and the hash is being checked (M3-3)
  kDone,
  kFailed,   ///< the transfer will not work as asked: no such rom, nowhere to write it
  kSkipped,  ///< deliberately not downloaded -- a disc set, an unmapped platform
};

/// Stable, log-friendly name -- `queued`. Never null. This is what goes in the
/// file, so it is a format decision and not just a log convenience.
const char* ToString(QueueState state);

/// The inverse of `ToString`. False for anything else, which is how a file
/// written by a different release is refused rather than read as `kQueued`.
bool ParseQueueState(std::string_view text, QueueState* out);

/// True for a state the worker will never touch again.
bool Terminal(QueueState state);

/// One rom on its way to the card.
///
/// Everything but `rom_id` and `queued_at` is filled in by the worker: `Enqueue`
/// records the id and returns, because no IPC command may block on the network
/// (ipc.hpp) and the rom's name, size and platform come from
/// `GET /api/roms/{id}`.
///
/// The strings are owned. `config::RomFile` holds two `string_view`s and says it
/// is an argument and never a record, so an entry keeps its own copies and
/// builds a `RomFile` at the call.
struct QueueEntry {
  /// RomM's rom id. Positive; the key the queue is deduplicated on.
  std::int64_t rom_id = 0;

  /// RomM's on-disk folder name for the platform. **Not `platform_slug`** --
  /// see `config::RomFile`. Empty until the worker has resolved the rom.
  std::string platform_fs_slug;

  /// The rom's name on the server's filesystem. Empty until resolved.
  std::string fs_name;

  /// `fs_size_bytes`, and what `http::DownloadTarget::expected_size` is set to.
  /// Zero until resolved, and zero from a server that does not know.
  std::int64_t size_bytes = 0;

  /// The rom's `sha1_hash`, lowercase hex, or empty when the library has none
  /// -- an unscanned RomM leaves it null. M3-3 (#20) is what checks it.
  std::string sha1_hash;

  /// The absolute SD path this rom is written to, from
  /// `config::Config::DestinationFor`. Empty until resolved, and empty forever
  /// on an entry that was refused a destination.
  std::string destination;

  QueueState state = QueueState::kQueued;

  /// Bytes already on the card for this entry, the `.part` included. It is what
  /// the overlay draws a bar from (#22), so it counts the whole file rather
  /// than the bytes one request fetched -- a resumed download that reported
  /// only the latter would show the bar restarting at zero.
  std::int64_t bytes_done = 0;

  /// Failed requests spent on this entry, across drains as well as within one.
  /// **Never a reason to drop it** -- an entry leaves the queue only when the
  /// user removes it or it reaches a terminal state. It is what tells "this has
  /// been trying for a while" from "this has not started", and what M7-2's
  /// scheduler will pace a retry from.
  int attempts = 0;

  /// Why it is in the state it is in, in a sentence for a log and an overlay
  /// row. **Never quotes `fs_name`** -- `config::RomDestination::reason` gives
  /// the reason, and a rom refused for a control character in its name must not
  /// put that byte in the queue file.
  std::string message;

  /// Whole Unix seconds, the moment the id was enqueued. Seconds rather than
  /// RFC 3339 for `state.db`'s reason: the engine can format that and has no
  /// parser for it.
  std::int64_t queued_at = 0;
};

/// The queue, in memory and on the card.
///
/// Every method takes the lock: the IPC thread enqueues while the worker
/// drains, which is the contract `ipc::Engine` states -- "every method is
/// callable from the IPC thread while the engine's own threads are running".
///
/// Loading and saving are free functions below rather than methods, so the
/// format is testable against a string literal with no filesystem, exactly as
/// `state::ParseBaseline` is.
class Queue {
 public:
  Queue() = default;

  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  /// Add `rom_id`, or say why not.
  ///
  /// `position` is 1-based among the entries the worker still has to do, and is
  /// set only on `kOk`. This answers `kQueueFull`, `kDuplicate`, and
  /// `kUnknownRom` for an id no rom can have -- zero or negative. The library's
  /// two refusals, a rom that is not in it and a disc set, are `EnqueueRom`'s:
  /// they need the index, and the queue does not hold one.
  ///
  /// **A terminal entry for the same rom is re-queued rather than refused.** A
  /// user pressing download on a rom that failed means "try again", and an entry
  /// that came back `kDuplicate` because of a `kFailed` row from last week would
  /// leave them with no way to ask. It is still one entry: the row is reset --
  /// state, attempts, bytes and message -- and moved to the tail.
  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position);

  /// Drop the entry for `rom_id`, whatever state it is in. `kNotQueued` when
  /// there is none.
  ///
  /// Removing an entry the worker is *on* does not interrupt the transfer in
  /// flight -- `http::CancelToken` is how a caller does that -- but the entry is
  /// gone, so nothing is written down when that transfer ends.
  ipc::Error Remove(std::int64_t rom_id);

  /// Drop every entry, terminal ones included.
  void Clear();

  /// A copy of every entry, in queue order. A copy rather than a reference for
  /// `ipc::EngineSnapshot`'s reason: the overlay asks while the worker is
  /// running.
  std::vector<QueueEntry> Snapshot() const;

  /// The entry for `rom_id`, or a default-constructed one with `rom_id == 0`.
  QueueEntry Find(std::int64_t rom_id) const;

  /// Replace the whole queue -- what a load does, and nothing else should.
  /// Entries past `kMaxQueueEntries` are dropped from the tail.
  void Reset(std::vector<QueueEntry> entries);

  /// Entries the worker still has to do: everything not `Terminal`. This is the
  /// `queue_depth` `ipc::Status` carries, and it deliberately does not count the
  /// rows kept for the queue screen.
  std::size_t pending() const;

  /// Every entry, terminal ones included.
  std::size_t size() const;

  /// The entry the worker is on, as the status screen draws it (ipc.hpp).
  /// `kIdle` when nothing is `kActive` or `kVerifying`.
  ipc::DownloadSnapshot CurrentDownload() const;

  /// The first entry the worker still has to do, or a default-constructed one
  /// with `rom_id == 0` when there is none. A copy, so the worker holds no
  /// reference into a vector the IPC thread may reallocate under it.
  QueueEntry NextPending() const;

  /// Write `entry` back over the row with the same `rom_id`.
  ///
  /// **False when there is no such row**, which is not an error: `Remove` is
  /// callable while the worker is mid-transfer, and an entry the user took out
  /// of the queue must not be put back by the transfer finishing. The worker
  /// treats it as "this entry is no longer mine" and moves on.
  bool Update(const QueueEntry& entry);

 private:
  /// The caller holds `mutex_`.
  std::vector<QueueEntry>::iterator FindLocked(std::int64_t rom_id);

  mutable std::mutex mutex_;
  std::vector<QueueEntry> entries_;
};

/// `ipc::Engine::Enqueue`, whole: the two refusals only the library can answer,
/// then the two only the queue can.
///
/// It is a free function rather than a `Queue` method because the library is not
/// the queue's to hold, and because this is the one piece a `sysmodule/`
/// engine has to get right -- **neither check may touch the network** (ipc.hpp),
/// so `library` is the index the engine already fetched, not a call.
///
/// `kUnknownRom` when the index has no such rom, `kMultiFile` when it has one
/// with `has_multiple_files` -- refused at the door rather than queued and
/// skipped, because the overlay can say so while the user is still looking at
/// the rom (docs/ARCHITECTURE.md puts disc sets out of scope for v1).
///
/// An index that is `truncated()` is *not* a licence to guess: a rom that was
/// not read is still `kUnknownRom`, because the alternative is queueing an id
/// nothing has checked.
ipc::Error EnqueueRom(Queue& queue, const roms::RomIndex& library, std::int64_t rom_id,
                      std::int32_t* position);

// --- the file -----------------------------------------------------------------

/// The entries a file held, and everything wrong with it.
///
/// `entries` is always usable: an empty queue is a perfectly good queue, and it
/// is what every failure here produces. Nothing in this module refuses to
/// produce one -- the sysmodule reads this at boot and nothing may block boot
/// (CLAUDE.md).
///
/// Entries rather than a `Queue`, unlike `state::LoadedBaseline`: a `Queue`
/// owns a mutex, so it is neither copyable nor movable and cannot be returned.
/// The caller adopts them with `Queue::Reset`, which is the one method that
/// replaces the whole queue.
struct LoadedQueue {
  std::vector<QueueEntry> entries;

  /// In the order they were found, bounded by `kMaxDiagnostics`. A first boot
  /// produces exactly one, saying there is no file yet.
  std::vector<std::string> diagnostics;

  /// One line per diagnostic, for a log. Empty when there are none.
  std::string DescribeDiagnostics() const;
};

/// The whole file: `{"format":"rommsync-queue","version":1,"entries":[...]}`,
/// ending in a newline.
///
/// Built with `json::Quote`, never by concatenation -- a `fs_name` holding a
/// quote or a backslash is a name RomM will happily serve, and it must be
/// carried rather than interpreted (json.hpp).
std::string SerializeQueue(const std::vector<QueueEntry>& entries);

/// Parse the contents of a `queue.json`.
///
/// Pure: no filesystem, no clock. **A malformed file yields an empty queue, not
/// the entries that happened to parse.** A truncation leaves a prefix that is
/// individually well-formed and collectively a lie, and "some entries survived"
/// is a state no caller can reason about -- the same call `state::ParseBaseline`
/// makes, and cheaper here, since the cost is a user re-queueing rather than a
/// rom deleted off the card.
LoadedQueue ParseQueue(std::string_view text);

/// Read `path` and parse it.
///
/// A *missing* file falls back to `io::PreviousPathFor(path)` first, the same
/// recovery `token_store`, `device_identity`, `config` and `state_db` make: the
/// one moment `queue.json` legitimately does not exist is the window
/// `io::WriteAtomically` opens between its two renames. A file that exists and
/// will not open does not take it -- that is a bad moment, not a commit window.
LoadedQueue LoadQueue(const std::string& path);

/// Why writing the queue did not work. Mirrors `state::StoreError`, minus the
/// members a queue cannot reach.
enum class StoreError {
  kNone,
  kTooManyEntries,  ///< more than `kMaxQueueEntries`; the reader would discard the file
  kTooLarge,        ///< the serialised file exceeds `kMaxQueueBytes`, same reason
  kOpenFailed,      ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,     ///< the bytes did not all reach the disk; the destination is untouched
  kCommitFailed,    ///< the rename onto `path` failed; see atomic_file.hpp
};

/// Stable, log-friendly name. Never null.
const char* ToString(StoreError error);

struct StoreResult {
  StoreError error = StoreError::kNone;

  /// For logs. Names the path and what went wrong, never a rom's `fs_name`.
  std::string message;

  bool ok() const { return error == StoreError::kNone; }
};

/// Write `queue` to `path`, atomically.
///
/// Unlike `state::SaveBaseline` there is no per-entry skip. A baseline is an
/// optimisation and a dropped row costs a re-hash; a queue entry is a thing the
/// user asked for, and silently leaving one out is how a download never happens
/// and nothing says so. An entry that cannot be written is a bug in whatever put
/// it there, so the whole write refuses and says which bound it hit.
StoreResult SaveQueue(const std::string& path, const Queue& queue);

// --- the worker ---------------------------------------------------------------

/// `GET /api/roms/{id}/content/{fs_name}` against `base_url`, with `fs_name`
/// percent-encoded as one path segment.
///
/// The encoding is not optional: `Synthetic Two Disc Game` has spaces in it, and
/// a raw one produces a request line no server parses. Here rather than in the
/// worker so M3-3 (#20), which owns the ranged half of this same request, uses
/// the same spelling of the endpoint instead of a second one.
std::string ContentUrl(std::string_view base_url, std::int64_t rom_id, std::string_view fs_name);

/// The rom fields the worker reads out of `GET /api/roms/{id}`.
///
/// Five of about eighty, for `roms::Rom`'s reason: the rest are not the client's
/// to hold. `sha1_hash` is `string | null` -- an unscanned library leaves it
/// null -- and is the only one that is allowed to be absent.
struct RomDetail {
  std::int64_t id = 0;
  std::string fs_name;
  std::string platform_fs_slug;
  std::int64_t size_bytes = 0;
  std::string sha1_hash;
  bool has_multiple_files = false;

  /// RomM knows the rom and its file is gone from the server's own filesystem.
  /// A download would 404 a third of the way in; the entry fails with a sentence
  /// instead.
  bool missing_from_fs = false;
};

/// Read one `DetailedRomSchema` body, or say which field was wrong.
json::Error ParseRomDetail(std::string_view body, RomDetail* out);

/// How hard the worker tries, and how it reaches the world.
struct WorkerOptions {
  /// The origin, normalised -- `config::ServerConfig::url`.
  std::string base_url;

  /// The client token. Sent as `Authorization: Bearer`.
  std::string bearer_token;

  /// The real path of `queue.json` -- what `fs::FileSystem::Resolve` makes of
  /// `/config/rommsync/queue.json`. A real path rather than an SD one because
  /// `io::WriteAtomically` opens it with `<cstdio>`, which knows no `sdmc:`.
  std::string queue_path;

  /// Ceiling on one `GET /api/roms/{id}`. The content request deliberately has
  /// none -- a 120 MiB rom on hotel Wi-Fi is slow rather than dead, and
  /// `stall_timeout` is what tells those apart (http.hpp).
  std::chrono::milliseconds detail_timeout = http::kDefaultTimeout;
  std::chrono::milliseconds stall_timeout = http::kDefaultStallTimeout;

  /// Requests one entry may spend inside one drain, including the first. Only a
  /// retryable failure spends another; the entry stays queued either way.
  int max_attempts = 3;

  /// The first delay before a retry, doubled per consecutive retryable failure
  /// and capped at `max_backoff`. `sync::NegotiateOptions`' scheme, and its
  /// reasoning: every network call retries with backoff (CLAUDE.md).
  std::chrono::milliseconds backoff{1'000};
  std::chrono::milliseconds max_backoff{8'000};

  /// How the worker waits between attempts. **Null means the default sleep, not
  /// "do not wait"** -- `sync::NegotiateOptions::wait`'s contract, so a caller
  /// that wants one attempt says so with `max_attempts`.
  std::function<void(std::chrono::milliseconds)> wait;

  /// Optional, not owned; must outlive the drain. Cancelling stops the transfer
  /// in flight and ends the drain, leaving the entry `kQueued` with its `.part`.
  const http::CancelToken* cancel = nullptr;
};

/// Why a drain stopped.
enum class DrainOutcome {
  kIdle,       ///< nothing left to do -- the queue holds no pending entry
  kDisabled,   ///< `[downloads] enabled = false`. Nothing was touched.
  kCompleted,  ///< every pending entry reached a terminal state
  kRetryable,  ///< a network failure; the entry is still queued and will be tried again
  kCanceled,   ///< the caller's `CancelToken` fired
  kUnauthorized,  ///< 401 -- the token is gone. Retrying will not fix it; pairing will.
  kStoreFailed,   ///< the queue could not be written; see `store` for which bound
};

/// Stable, log-friendly name. Never null.
const char* ToString(DrainOutcome outcome);

/// What one drain did.
struct DrainResult {
  DrainOutcome outcome = DrainOutcome::kIdle;

  /// Entries that reached `kDone`, `kSkipped` and `kFailed` this drain.
  int downloaded = 0;
  int skipped = 0;
  int failed = 0;

  /// Requests actually sent and the backoff actually asked for, so a caller can
  /// see a drain that cost three attempts and a test can check the retry without
  /// waiting it out -- `sync::Negotiation`'s reasoning.
  int attempts = 0;
  std::chrono::milliseconds waited{0};

  /// The last write of `queue.json`, when it failed. `ok()` otherwise.
  StoreResult store;

  /// One sentence for the log. Never carries the token.
  std::string message;
};

/// Drain `queue` until it is empty, the network says no, or the caller cancels.
///
/// One transfer in flight, deliberately. The sysmodule heap is measured in
/// megabytes (docs/DEVELOPMENT.md) and two 120 MiB transfers over one Wi-Fi
/// radio buy nothing.
///
/// Per entry: `GET /api/roms/{id}` -> refuse a disc set or a rom missing from
/// the server -> `config::Config::DestinationFor` -> stream the content to
/// `<destination>.part` and rename it on -> `kDone`. The queue is written after
/// every one of those transitions, which is what makes a power cut resumable
/// rather than a restart.
///
/// **`[downloads] enabled = false` idles; it does not drop the queue.** The
/// answer is `kDisabled` and the file is not even opened, so switching downloads
/// back on resumes exactly what was there (docs/CONFIG.md).
///
/// `filesystem` supplies `Resolve` alone: the engine names SD paths and only a
/// backend knows the prefix (file_system.hpp). A destination the backend refuses
/// -- and a mapped folder that does not exist, which the platform layer creates
/// and this does not -- is `kFailed` with a sentence, never a guessed path.
DrainResult Drain(http::HttpClient& client, fs::FileSystem& filesystem,
                  const config::Config& config, Queue& queue, const WorkerOptions& options);

}  // namespace rommsync::download

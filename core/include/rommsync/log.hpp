// The one line this client is allowed to write down, and where it goes.
//
// Until M7-3 nothing here wrote anything anywhere. `core/` is full of strings
// built to be logged and handed *up* instead -- `ExecutionReport::warnings`,
// `ScanResult::DescribeSkipped()`, `LoadResult::DescribeDiagnostics()`,
// `auth::DescribeStoredToken`, half a dozen `ToString`s -- each with a comment
// saying "core/ has no logger". This is that logger, and the reason it arrives
// with docs/TROUBLESHOOTING.md rather than before it: a log nobody can read a
// guide against is a file that costs SD space and answers nothing.
//
// ## What is pinned, and what is not
//
// **`Event` is the contract.** Every line carries exactly one of a closed set of
// tags, `kAllEvents` is its extent, and docs/TROUBLESHOOTING.md documents each
// one -- `docs.troubleshooting_events` checks both directions and
// `docs.troubleshooting_emitted` checks that every tag has a call site. That is
// the same arrangement `auth.scopes` uses to pin the scope block in
// docs/API_CONTRACT.md to `MinimumScopes()`: a document and a header cannot
// drift when a test reads both.
//
// The *detail* after the tag is not pinned and is not meant to be. It is the
// message the failing subsystem already built -- a `Diagnostic::Describe()`, an
// `http::ToString` -- and freezing those would be freezing a dozen other
// headers' output. A guide quotes the tag and paraphrases the rest.
//
// ## What never reaches a sink
//
// A bearer token, a `device_code`, and the credentials in a `user:password@`
// URL. `Redact` runs inside `Write`, **not at the call sites**, because the
// guarantee has to hold for a call site nobody reviewed: the card is readable by
// anything on the console and by anyone who pulls it (docs/SECURITY.md), and a
// log file is forever. `log.redacts` asserts all three against the file the sink
// actually wrote.
//
// This is belt and braces rather than a licence: a caller still passes
// `DescribeStoredToken`'s line and not the token, and `config::Diagnostic`
// still never quotes a `server.url`.
//
// ## What is not in a line
//
// **A timestamp.** Horizon answers `std::chrono::system_clock::now()` with
// nothing usable until `timeInitialize` has run, and a console whose clock will
// not initialise is a supported state -- docs/SYNC_PROTOCOL.md already refuses
// an epoch mtime rather than trusting one. A log stamped from that clock would
// date every line to 1970 and read as a client from the wrong decade. What
// orders the file instead is the ordinal every line carries, which is monotone
// for the life of the process and survives a rotation.
//
// ## Where the halves live
//
// Everything here is portable -- `<cstdio>`, `<mutex>` and nothing else -- so
// `FileSink` runs on the host under `ctest` and on Horizon over the same
// `sdmc:` devoptab `io::WriteAtomically` uses. Only the *path* is the platform's
// (`kLogSdPath` is the SD-root one; `sysmodule/source/main.cpp` prefixes it).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::log {

/// How loud one line is.
///
/// Three, and the same three `config::Severity` draws, because a user reading a
/// log and a user reading the settings screen are asking the same question:
/// `kError` is the client not doing its job, `kWarn` is something that did not
/// happen as written, `kInfo` is the running commentary a bug report needs.
enum class Level { kError, kWarn, kInfo };

/// Stable, log-friendly name -- `warn`. Never null.
const char* ToString(Level level);

/// What a line is *about*, from a closed set.
///
/// One tag per failure mode docs/TROUBLESHOOTING.md has a section for, plus the
/// two a bug report starts with. It is an enum rather than a string at the call
/// site so that adding a mode is a compile-time edit in one place, and so the
/// guide can be checked against something with an extent.
enum class Event {
  /// Which build, and what transport it came up with. The first question of
  /// every support thread.
  kBoot,

  /// `auth::DescribeStoredToken` -- which server, which device, which scopes,
  /// and that there is a token without saying what it is.
  kStoredToken,

  /// The server has stopped accepting this console's credentials: a 401, or a
  /// 403 for a scope the user did not tick. Re-pairing is the remedy for one and
  /// not for the other (docs/AUTH.md#scopes).
  kAuthRejected,

  /// One complaint about `config.ini`, as `config::Diagnostic::Describe()`
  /// renders it -- "line 9 [sync] states: expected true or false".
  kConfigDiagnostic,

  /// There is no usable `[server] url`, so there is nothing to sync with.
  /// **Never quotes the URL**, which is the rule `config::Diagnostic` already
  /// keeps.
  kNoServer,

  /// The folder map yields no save directories at all. The commonest
  /// misconfiguration by a distance: `[platform.snes]` *replaces* that
  /// platform's defaults, so a section listing only `roms` switches saves off
  /// (docs/CONFIG.md).
  kNoSaveDirs,

  /// The exchange never completed -- offline, refused, stalled or dropped. The
  /// tick is skipped and nothing is written.
  kNetOffline,

  /// The handshake or the certificate was refused. Kept apart from
  /// `kNetOffline` because it does not come right on its own: a fault in the
  /// certificate, the hostname or a TLS version, which is why the scheduler
  /// caps its retries rather than reconnecting forever
  /// (`sync::SchedulerConfig::max_tls_attempts`).
  kNetTls,

  /// A file in a save folder that matched no rom, matched two, or could not be
  /// used -- `scan::ScanResult::DescribeSkipped()`. Never guessed at, always
  /// said out loud (docs/SYNC_PROTOCOL.md step 0).
  kScanSkipped,

  /// The server answered, and the answer will not change on a retry: a device
  /// deleted in the web UI, sync switched off for it, or a 2xx this client
  /// cannot read as a plan.
  kSyncRefused,

  /// How one tick ended, and what it moved. The line a user is asked for first.
  kSyncTick,

  /// An operation that would have written a save did not -- a backup that could
  /// not be taken, a card that would not be written, a digest that did not
  /// match. **A failure here means the save was left exactly as it was**, which
  /// is the whole of hard rule 2 (docs/SYNC_PROTOCOL.md#backups).
  kSaveFailed,

  /// The play-session buffer could not be written (M7-4). **Never a reason to
  /// fail a tick** -- play time is the most droppable thing in the client -- but
  /// not silent either: a `play.db` that will not write is a buffer that stops
  /// draining and a window that stops moving, and both are invisible from
  /// outside. No save is at risk; that is `kSaveFailed`.
  kPlayFailed,
};

/// Every event, in declaration order. The guide and the emitted set are both
/// checked against this rather than against a second copy of the list -- the
/// reason `ipc::kAllCommands` and `ipc::kAllErrors` exist.
inline constexpr std::array kAllEvents = {
    Event::kBoot,        Event::kStoredToken, Event::kAuthRejected,
    Event::kConfigDiagnostic, Event::kNoServer, Event::kNoSaveDirs,
    Event::kNetOffline,  Event::kNetTls,      Event::kScanSkipped,
    Event::kSyncRefused, Event::kSyncTick,    Event::kSaveFailed,
    Event::kPlayFailed,
};

/// Stable tag -- `net.offline`. Never null.
///
/// The `<area>.<what>` shape is deliberate: it is what makes a whole area
/// greppable out of a log a user pasted into an issue (`grep net\.` is "the
/// network half"), and it is what the guide's section headings are keyed on.
const char* ToString(Event event);

/// True when `tag` names an event this build writes, in which case `out` holds
/// it. What lets a doc test turn a string it read out of a fence back into a
/// value with an extent.
bool IsEvent(std::string_view tag, Event* out = nullptr);

/// The longest one line may be, tag and ordinal included.
///
/// A `Diagnostic` names a file path and a save's name is the user's data, so
/// "one line" is not a size until something says so. Longer is truncated with a
/// marker rather than dropped: the tag and the start of the detail are the part
/// that identifies the failure, and losing the line entirely would lose those
/// too.
///
/// **The cut is taken at a UTF-8 boundary**, up to three bytes short of this,
/// because a save's name is very often not ASCII and half a character renders as
/// a replacement box in a text editor and in the overlay's font alike.
inline constexpr std::size_t kMaxLineBytes = 256;

/// What the marker says. Public because the doc test greps for it, and because
/// a reader of a log has to be able to tell a truncated line from a short one.
inline constexpr const char* kTruncationMarker = " ...[cut]";

/// How many lines the in-memory tail keeps. See `Tail`.
inline constexpr std::size_t kTailLines = 24;

/// Where a line ends up, once it has been rendered and redacted.
///
/// One method rather than a `printf`-shaped family: `Write` has already decided
/// the level, the tag, the ordinal and the bound, so what is left for a sink to
/// do is put bytes somewhere. That is what keeps `FileSink` twenty lines long
/// and testable, and what lets a test install a sink of its own in two.
class Sink {
 public:
  virtual ~Sink() = default;

  Sink(const Sink&) = delete;
  Sink& operator=(const Sink&) = delete;

  /// `line` is the whole rendered line without a trailing newline, already
  /// bounded by `kMaxLineBytes` and already redacted. `level` is repeated
  /// because it is in the line as text and a sink that filters should not have
  /// to parse it back out.
  ///
  /// **Called synchronously, on whichever thread wrote the line**, and with the
  /// log's own lock *not* held -- so a sink that blocks holds up the tick that
  /// logged, and two sinks may be called at once. `FileSink` owns a lock of its
  /// own for that reason.
  virtual void Write(Level level, std::string_view line) = 0;

 protected:
  Sink() = default;
};

/// Install the sink, process-wide. Null -- the default -- discards.
///
/// **Why a process-wide sink and not an injected interface**, which is how every
/// other platform facility here is reached. `io::SetFileSync` answers this at
/// length and the answer is the same one: the callers are free functions a dozen
/// call sites reach for without holding anything, so injecting it would mean a
/// logger parameter on every signature between `main` and the failure. The cost
/// is that the guarantee depends on `main` having installed one, and the tests
/// install one too.
///
/// Meant to be called once from `main`, before the engine's threads exist.
/// Stored under the same lock the ring uses, so a late call cannot race a write
/// rather than because swapping it mid-run is a supported thing to do.
///
/// **The tail does not depend on it.** `Tail` answers from memory whether a sink
/// was ever installed or not, which is what lets `GetLog` work on a console
/// whose SD card refused the log file (`ipc.hpp`).
void SetSink(Sink* sink);

/// The installed sink, or null. For a test that wants to put its own back.
Sink* GetSink();

/// Write one line: `<ordinal> <level> <event> <detail>`.
///
/// `detail` may be empty, in which case the line is the first three fields. It
/// is redacted and bounded here; see the header note for both.
void Write(Level level, Event event, std::string_view detail);

/// The three levels, spelled so a call site reads as a sentence.
inline void Error(Event event, std::string_view detail) { Write(Level::kError, event, detail); }
inline void Warn(Event event, std::string_view detail) { Write(Level::kWarn, event, detail); }
inline void Info(Event event, std::string_view detail) { Write(Level::kInfo, event, detail); }

/// Every line of `text`, split on newlines, as one log line each.
///
/// The three describers this exists for -- `LoadResult::DescribeDiagnostics`,
/// `ScanResult::DescribeSkipped`, `LoadedQueue::DescribeDiagnostics` -- each
/// render *one line per complaint* into a single string, and a sink handed that
/// whole would write a multi-line record no `tail` and no `grep -c` can count.
/// Empty lines are dropped; an empty `text` writes nothing at all, which is what
/// makes "log the diagnostics" safe to call on a file with none.
void WriteEach(Level level, Event event, std::string_view text);

/// One line each, for a caller that already has them apart.
///
/// The three reports a tick hands up -- `ExecutionReport::warnings`,
/// `TickCompletion::warnings`, `RecoveryReport::warnings` -- are already a
/// `vector<string>`, where the describers above render one block. Both spellings
/// exist so neither caller has to convert into the other's shape and back.
/// Empty entries are dropped, as they are above.
void WriteEach(Level level, Event event, const std::vector<std::string>& lines);

/// One line as the tail remembers it.
struct Line {
  Level level = Level::kInfo;

  /// The ordinal in the line, 1-based and monotone for the life of the process.
  /// Carried separately so a reader does not parse it back out of `text`.
  std::uint64_t ordinal = 0;

  /// The whole rendered line, exactly as the sink saw it.
  std::string text;
};

/// The last `count` lines, oldest first, and how many have been written in all.
///
/// **Always available, sink or no sink.** The ring is the log's own memory: it
/// costs `kTailLines * kMaxLineBytes` at worst -- 6 KiB, which is a term in the
/// sysmodule's heap table (`sysmodule/source/main.cpp`) rather than a cost
/// nobody added up -- and it is what `ipc::ServiceCore::GetLog`
/// answers from, so the overlay can show why a sync did not happen without an SD
/// reader and without the IPC thread opening a file (`ipc.hpp`: no command goes
/// near the card to answer a question the sysmodule already knows).
///
/// `count` is clamped to `kTailLines`. The return value is the total ever
/// written, which is what tells a reader the tail is a tail.
std::uint64_t Tail(std::size_t count, std::vector<Line>* out);

/// Forget every line and reset the ordinal. **For tests**, which run several
/// scenarios in one process and would otherwise read each other's lines.
void Reset();

/// `text` with the three things that may never be written down taken out.
///
/// Exposed because it is what the redaction test asserts on directly, and
/// because a caller that renders a line some other way -- the overlay, one day
/// -- has to be able to reach the same rule rather than write a second one.
///
/// What goes, and how each is recognised:
///
///   * **`user:password@` in a URL.** `auth://` credentials, which
///     `config::NormalizeServerUrl` refuses outright -- so one reaching here
///     came from somewhere that did not go through it.
///   * **A `Bearer <token>` header value.**
///   * **The value of any key whose name says secret**: `token`,
///     `access_token`, `refresh_token`, `device_code`, `user_code`, `password`,
///     `secret`, `authorization`. Matched on `=` and on `:`, with or without
///     quotes, so both a query string and a JSON body are covered.
///
/// The value is replaced by `kRedacted` rather than removed, because a line that
/// silently lost a field reads as a line that never had one.
std::string Redact(std::string_view text);

/// What a redacted value is replaced by.
inline constexpr const char* kRedacted = "<redacted>";

// --- the file on the card -----------------------------------------------------

/// The log file, relative to `sdmc:/config/rommsync/`.
///
/// Named the way `config::kConfigFileName` and `auth::kTokenFileName` are: the
/// directory is the platform's to know (hard rule 4), the file name is this
/// header's.
inline constexpr const char* kLogFileName = "rommsync.log";

/// The same file as an SD-root absolute path, for a caller that has one of those
/// rather than a directory -- `config::kConfigSdPath`'s reason.
inline constexpr const char* kLogSdPath = "/config/rommsync/rommsync.log";

/// How large the live file may get before it is rotated.
///
/// 32 KiB, so the pair on the card is 64 KiB at worst. Small on purpose: this
/// is a log a user pastes the tail of into an issue, not a record anyone
/// analyses, and it shares a card with the roms it exists to help download. At
/// `kMaxLineBytes` a full file is at least 64 lines, and real lines are a
/// quarter of that -- several hundred, which is many ticks.
inline constexpr std::size_t kMaxFileBytes = 32 * 1024;

/// Where the rotated file goes: `<path>.old`.
///
/// Public because a user is told to attach both, and because the rotation test
/// has to name the second one. **Deliberately the same suffix
/// `io::PreviousPathFor` uses** and deliberately not the same function: that one
/// names where an *atomic write* parks the record it is replacing, and a reader
/// that confused the two would look for a half-committed log.
std::string PreviousLogPathFor(std::string_view path);

/// A bounded, rotating file. Everything the guide tells a user to send.
///
/// The whole of the rotation: when a line would take the file past
/// `max_bytes`, whatever is at `<path>.old` is removed, `<path>` is renamed onto
/// it, and the line starts a new file. So the card holds the current file and
/// exactly one previous one, and the total never exceeds twice the cap.
///
/// **No handle is held between lines.** Each write opens, appends and closes,
/// the way `sysmodule::MakeSdCard` opens what it needs and closes it -- which is
/// what makes it safe to share between the worker and the IPC thread, and what
/// means a card pulled mid-run costs the line rather than a file descriptor the
/// process can never let go of. A tick writes a handful of lines, so the cost is
/// a handful of `fopen`s per half hour.
///
/// **A failure is silent.** A missing directory, a full card, a read-only card:
/// the line is dropped and nothing is reported, because there is nowhere left to
/// report it *to* and because a client that stopped syncing over a log it could
/// not write would have the tail wagging the dog. The in-memory tail still has
/// the line, so the overlay still shows it.
///
/// **Durability is not promised.** `io::FileSync` is not called: it commits the
/// whole `sdmc:` device on Horizon (`atomic_file.hpp`), which is far too much to
/// spend per log line, and a log is the one file here whose last few lines are
/// worth less than the cost of guaranteeing them. A console that loses power
/// mid-tick may be missing the tail of the file. The guide says so.
class FileSink : public Sink {
 public:
  /// `path` is a platform path, already prefixed -- `sdmc:/config/rommsync/…`
  /// on a console, somewhere under a sandbox in a test. The directory must
  /// exist; nothing here creates one, for the reason `io::WriteAtomically`
  /// gives.
  explicit FileSink(std::string path, std::size_t max_bytes = kMaxFileBytes);

  void Write(Level level, std::string_view line) override;

  const std::string& path() const { return path_; }

 private:
  /// Rename the live file onto `<path>.old` and drop whatever was there.
  /// Called with `mutex_` held.
  void RotateLocked();

  std::string path_;
  std::size_t max_bytes_;

  /// What the live file holds, as this sink last left it.
  ///
  /// Tracked rather than measured per line, and seeded from the file's real size
  /// on the first write -- a sysmodule restarted twice an hour must not start a
  /// fresh file each time, and must not `fseek` the whole file on every line
  /// either.
  std::size_t bytes_ = 0;
  bool measured_ = false;

  /// `Sink::Write` may be called from two threads at once (see the interface).
  /// One file, one lock.
  std::mutex mutex_;
};

}  // namespace rommsync::log

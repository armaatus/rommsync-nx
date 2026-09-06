// The three lists the overlay browses, served a page at a time (M5-4, #31).
//
// `ipc.hpp` owns the envelope -- `ListBegin`/`ListNext`/`ListEnd`, `ListItem`,
// `ListPage`, and the per-kind field names in `ipc::list_keys`. This is the
// producer behind it: the cursors, the projections, and the one fetch each kind
// needs. `LibraryBrowserModel` (`overlay_library_model.hpp`) is the consumer,
// and the two were written in different worktrees, so anything they both have
// to agree about lives in `ipc::list_keys` rather than here.
//
// ## Why a page at a time at all
//
// The sysmodule's inner heap is `0xC0000` and the trimmed bsd transfer memory
// takes 116 KiB of it (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision).
// A library of ten thousand roms is never materialised -- not from RomM, not in
// an IPC buffer, not in a cache here. What is held is one page and, for
// `platforms` alone, a trimmed projection of a list 5.2.0 refuses to page.
//
// ## What a cursor is, and what it is not
//
// **A cursor holds an offset and a filter, never a snapshot of the library.**
// The library can change under an open cursor -- a scan finishes, a rom is
// deleted -- and when it does a page is allowed to skip an item or to repeat
// one. The overlay must not assume otherwise, and #25's browser does not: it
// draws what it was handed.
//
// `platforms` is the exception, and it is forced. `GET /api/platforms` in 5.2.0
// is an unpaged bare array (docs/API_CONTRACT.md), so it is paged on *this*
// side: the first `ListNext` fetches it whole, keeps the five scalars a row
// needs, and every page after that is served out of that vector with no request
// at all. `kMaxPlatforms` is what stops "however many the server says" from
// being a number this client did not choose.
//
// ## Cursors are capped, reclaimed, and never reused
//
// A cursor is abandoned far more often than it is closed: the user shuts the
// overlay with a button combo and `ListEnd` never arrives. So `kMaxCursors` are
// open at once, `kCursorTtl` since the last touch reclaims one, and opening past
// the cap evicts the least recently touched rather than refusing -- #25 treats
// `kBadCursor` on a `ListNext` as "re-open and reload", so a reclaimed cursor
// costs a round trip, while a `ListBegin` that refused would leave a screen with
// nowhere to go.
//
// Ids come from a counter that only ever increases, so an id that was reclaimed
// can never address a later cursor. That is what makes `kBadCursor` mean what it
// says: reclaimed, never issued, or already ended -- and not "someone else's
// list". Reclamation happens when a command runs or `Pump()` does; there is no
// timer here, so an abandoned cursor costs its own memory until the next call
// and nothing more.
//
// ## Nothing here blocks the IPC thread on the network
//
// `ipc.hpp` forbids it. A kind that needs no request -- `queue`, and `platforms`
// once its snapshot is in hand -- is answered on the spot. A page that needs one
// marks the cursor, answers `kOk` with `ListPage::pending`, and the request
// itself happens in `Pump()`, which the engine's own worker drives. That is
// `auth::PairingSession::Poll`'s shape, for its reason: the object holds the
// state machine and the caller owns the thread.
//
// A page that fails is a failed page. The cursor keeps its offset and stays
// usable, because the alternative is a list that wedges on one timeout.
//
// `Pump()` is the only thing here that runs on another thread, so everything it
// uses is copied out under the lock before the request is made: the two
// backends, and -- since M7-2 (#37) started the worker that drives it -- the
// **configuration**. That one used to be held as a reference, which was a data
// race the moment anything but the IPC thread read it: `SetConfig` replaces the
// whole `Config`. `Service::ConfigSource` is what closed it, and it is the
// snapshot `sysmodule/source/engine.hpp` said would have to replace the
// reference.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth_gate.hpp"
#include "rommsync/backoff.hpp"
#include "rommsync/config.hpp"
#include "rommsync/download.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"

namespace rommsync::lists {

/// How many lists may be open at once.
///
/// Four, which is one more than the browser can have on its stack (platforms,
/// a platform's roms, the queue). The number is small on purpose: each open
/// cursor is heap on a sysmodule, and the cost of being wrong about it is a
/// re-opened list rather than a lost one.
inline constexpr std::size_t kMaxCursors = 4;

/// How long a cursor survives without being touched.
///
/// Ninety seconds is far longer than the gap between two presses of a d-pad and
/// far shorter than the time an overlay closed with a button combo stays closed.
inline constexpr std::chrono::seconds kCursorTtl{90};

/// How many platforms one snapshot may hold. See the header note.
///
/// RomM 5.2.0's whole platform catalogue is a little over a hundred and a
/// library holds at most one row per platform in it, so this is a backstop
/// against a server that is not one rather than a limit a real console meets --
/// the same kind of bound `roms::kMaxIndexRoms` is.
///
/// A list cut here **ends** at that row, with `has_more` false. Keeping it true
/// past the last row would have the browser ask for a page that comes back empty
/// and says there is more, forever, which is a worse failure than a cut nobody
/// can reach.
///
/// The snapshot is **per cursor**, so the worst case this service holds is
/// `kMaxCursors * kMaxPlatforms` trimmed rows rather than one snapshot's worth.
/// Four times a number nothing reaches is still nothing, and sharing one
/// snapshot between cursors would mean deciding when it goes stale -- a cache,
/// which is the thing the header note says this never holds.
inline constexpr std::size_t kMaxPlatforms = 256;

/// How long one string on a list row may be.
///
/// A rom's `name` and `fs_name` are the user's data and have no length this
/// client may assume, and `ipc::AppendIfItFits` bounds a *page* -- it has no
/// answer for a single item that does not fit a payload on its own, which is
/// exactly what one 4 KiB name would be. So every string on a row is cut here,
/// on a UTF-8 boundary, with `...` in place of what was cut. A row is something
/// a human reads on a 1280x720 overlay; nothing downstream derives anything from
/// these strings -- a download is asked for by `rom_id` and the sysmodule
/// resolves the file name itself (`download.hpp`).
inline constexpr std::size_t kMaxRowTextBytes = 160;

/// What one list page fetch is given before it is abandoned.
///
/// Shorter than `http::kDefaultTimeout`, because a page is fetched while a user
/// is looking at a spinner rather than while a sync runs in the background.
inline constexpr std::chrono::milliseconds kRequestTimeout{15'000};

/// The wait after a page fails before another request is sent for that cursor,
/// doubling per consecutive failure up to `kMaxRetryBackoff`.
///
/// The standing rule is "timeout, offline-safe, retry with backoff"
/// (CLAUDE.md), and this is the backoff half of it. Nothing here retries by
/// itself -- a failed page is handed back and the caller asks again, which for
/// the overlay is a human pressing A. What this stops is a caller asking in a
/// loop: inside the window the same failure is answered again with no request
/// made.
inline constexpr std::chrono::milliseconds kRetryBackoff{1'000};
inline constexpr std::chrono::milliseconds kMaxRetryBackoff{8'000};

/// How much a cursor's retry window may be stretched by, as a fraction.
///
/// Small, because unlike the scheduler's this window is one a human is waiting
/// out at a screen. It is here at all because the curve is `retry::Backoff` --
/// the one this client has (M7-2, #37) -- and a caller that switched the jitter
/// off would be re-introducing the lockstep that object exists to prevent.
inline constexpr double kRetryJitter = 0.1;

/// One platform, cut to what a row draws. See `ipc::list_keys`.
struct PlatformRow {
  std::int64_t id = 0;
  std::string fs_slug;
  std::string name;
  std::int64_t rom_count = 0;
};

/// One rom, cut to what a row draws. See `ipc::list_keys`.
///
/// `on_disk` and `queued` are not on it: they are this side's answers rather
/// than the server's, and they are filled in as the page is built.
struct RomRow {
  std::int64_t id = 0;
  std::string name;
  std::string fs_name;
  std::string platform_fs_slug;
  std::int64_t size_bytes = 0;
  bool has_multiple_files = false;
};

/// One page of `GET /api/roms`, as this list reads it.
///
/// Deliberately not `roms::Page`: that one carries the two match keys the save
/// scanner needs and none of the three fields a row draws, and a struct serving
/// both would carry six fields per rom for two callers that each want three.
struct RomPage {
  std::vector<RomRow> roms;

  /// The whole filtered library, as the server counted it when it answered.
  std::int64_t total = 0;
};

/// Read one `GET /api/roms` body, or say which field was wrong.
///
/// Strict, like every other shape in the engine: a body that is not the
/// envelope, or a rom missing `platform_fs_slug`, is a named error rather than
/// a short page. A short page that looked like the end of the library is
/// precisely the failure a truncated response must not be able to produce.
json::Error ParseRomPage(std::string_view body, RomPage* out);

/// Read a `GET /api/platforms` body -- a **bare array**, not an envelope.
///
/// At most `kMaxPlatforms` are kept; `*truncated` says whether there were more,
/// and may be null for a caller that has nowhere to put the answer -- which is
/// every caller today, because the wire has no field for "this list was cut"
/// and the bound is not reachable against a RomM (see `kMaxPlatforms`).
json::Error ParsePlatforms(std::string_view body, std::vector<PlatformRow>* out,
                           bool* truncated = nullptr);

/// Cut `text` to `kMaxRowTextBytes` on a UTF-8 boundary, `...` in place of the
/// rest. Returns it unchanged when it already fits.
std::string Shorten(std::string_view text);

/// The producer behind `ipc::Engine`'s three list commands.
///
/// One instance per engine. Every method is callable from the IPC thread while
/// `Pump()` runs on another, which is `ipc::Engine`'s contract -- so this owns a
/// mutex, and no network call is ever made while it is held.
class Service {
 public:
  /// Where the configuration is read from, and why this is a function rather
  /// than the reference it used to be.
  ///
  /// `Pump()` runs on the engine's worker thread while `ipc::Engine::SetConfig`
  /// replaces the configuration on the IPC thread (M5-3, #30), and a page built
  /// from a `Config` that is being assigned into underneath it is a data race
  /// on every string and vector in it. So the worker takes a **snapshot**: the
  /// engine swaps a whole `Config` behind a pointer and hands out the pointer,
  /// and a page keeps the one it started with. `sysmodule/source/engine.hpp`
  /// named this seam and what would have to change to close it; this is it.
  ///
  /// It is asked once per operation rather than held, so a `SetConfig` between
  /// two pages of the same list is answered by the second page -- which is what
  /// "the answer to *is this platform mapped* has to be the one in force now"
  /// already promised.
  using ConfigSource = std::function<std::shared_ptr<const config::Config>()>;

  /// A source over a `Config` the caller owns and never replaces.
  ///
  /// For every caller that has one thread, which is the suite and anything
  /// driving this directly. `config` must outlive the service; nothing is
  /// copied and nothing is owned.
  static ConfigSource FixedConfig(const config::Config& config);

  /// What one of this service's requests said about the credentials, for
  /// `auth::Gate::Observe`.
  ///
  /// **The reason this exists**: `Pump()` maps every non-2xx to
  /// `ipc::Error::kOffline`, because the fixed error set has no
  /// `kUnauthenticated` and widening it would be invisible to an overlay built
  /// before the new ordinal -- the overlay reads an error by ordinal off a
  /// `Result` (#31 left this open and named M7-2 as the owner). So a console
  /// whose token was revoked would be told its server is unreachable on every
  /// page while `GetStatus` still reported it paired, and nothing anywhere would
  /// count the rejection.
  ///
  /// Rather than a second 401 path, the answer goes where every other call's
  /// already goes: `auth::AnswerOf(result)` into the one `auth::Gate`. The
  /// *page* still says `kOffline`, which is the honest sentence for a screen
  /// with no library to draw; what changes is that the console stops calling
  /// after the third one and the overlay's re-pair prompt comes up.
  ///
  /// Called from the worker thread, outside this object's lock, immediately
  /// after the request. Null means nothing is observing, which is every caller
  /// that has no gate.
  using AuthObserver = std::function<void(auth::Answer)>;
  void UseAuthObserver(AuthObserver observer);
  /// Injectable so the TTL, the eviction and the backoff are testable without
  /// waiting them out -- `auth::PairingSession::Clock`'s reason, and monotonic
  /// for it: a user setting the console clock must not reclaim a live cursor.
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  /// `queue` must outlive the service, and so must whatever `config` reads.
  ///
  /// `config` is asked on every operation rather than captured once, because the
  /// engine re-reads the configuration on every `SetConfig` and the answer to
  /// "is this platform mapped" has to be the one in force now, not the one in
  /// force when the list was opened. A null `clock` means `steady_clock`.
  Service(ConfigSource config, download::Queue& queue, Clock clock = nullptr);

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  /// The network the library is read over, and the credentials to read it with.
  ///
  /// **A null client is a build with no network backend**, which is the
  /// sysmodule today: nothing implements `http::HttpClient` for Horizon yet.
  /// `platforms` and `roms` then answer `ipc::Error::kOffline` -- the same
  /// sentence a console with its Wi-Fi off gets, which is what it amounts to --
  /// while `queue`, which never touches the network, is served in full.
  void UseServer(http::HttpClient* client, std::string bearer_token);

  /// Where a rom already on the card is looked for, for `on_disk`.
  ///
  /// **`on_disk` is a hint, not a verdict**: it is "a file of this rom's
  /// `fs_name` is already in one of the platform's mapped `roms` folders, at the
  /// size the server declares" -- one `stat` a row and no digest. The
  /// authoritative check is the download worker's, which still runs and still
  /// hashes (`download.cpp`); a row greyed wrongly costs a press, not a file.
  /// With no filesystem -- the console today -- it is `false`, which is the
  /// honest answer for a build that cannot look.
  void UseCard(fs::FileSystem* filesystem);

  /// `ipc::Engine::ListBegin`. `*cursor` is set only on `kOk`, and is never `0`.
  ///
  /// The request arrives already clamped by `ipc::ServiceCore::ListBegin`; this
  /// clamps again rather than trusting it, because it is also called directly by
  /// the suite.
  ipc::Error ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor);

  /// `ipc::Engine::ListNext`. See the header note: never blocks on the network,
  /// and a `pending` page is `kOk` rather than an error.
  ipc::Error ListNext(ipc::Cursor cursor, ipc::ListPage* page);

  /// `ipc::Engine::ListEnd`. `kBadCursor` for one that was already reclaimed --
  /// closing a list twice is a client bug worth seeing, not a no-op.
  ipc::Error ListEnd(ipc::Cursor cursor);

  /// Do at most one page's worth of network work, and return whether it did any.
  ///
  /// Called from the engine's worker thread, never from the IPC thread. It
  /// blocks for as long as one request takes; that is the whole point of it
  /// being here rather than in `ListNext`.
  bool Pump();

  /// How many cursors are open. For a log line, and for the suite.
  std::size_t open_cursors() const;

 private:
  using TimePoint = std::chrono::steady_clock::time_point;

  /// Where a cursor's page comes from when it comes from the server.
  enum class Fetch {
    kIdle,     ///< nothing in flight and nothing waiting to be handed over
    kRunning,  ///< `Pump` has it
    kReady,    ///< a page is built and the next `ListNext` takes it
    kFailed,   ///< the last attempt failed; the next `ListNext` reports it
  };

  struct Entry {
    ipc::Cursor id = 0;

    /// What the list was opened for: kind, filter and page size, already
    /// clamped. The whole of it rather than four fields beside each other,
    /// because it is the same clump `ipc::ListRequest` already is and `Pump`
    /// copies it out whole.
    ipc::ListRequest request;

    /// Rows handed over so far -- the offset the next page starts at.
    std::int64_t offset = 0;

    /// The server has nothing after `offset`. Kept so a caller that asks past
    /// the end gets an empty page rather than another request.
    bool exhausted = false;

    TimePoint touched{};

    Fetch fetch = Fetch::kIdle;
    ipc::ListPage ready;

    /// The last failure, kept after it has been reported so a retry inside the
    /// backoff window can answer it again without a request.
    ipc::Error failure = ipc::Error::kOk;

    /// The retry curve for this cursor: the one `retry::Backoff` the client has,
    /// per cursor because the count is per cursor. `failures()` is what
    /// `consecutive_failures` used to be.
    retry::Backoff backoff{{kRetryBackoff, kMaxRetryBackoff, kRetryJitter}};
    TimePoint not_before{};

    /// `kPlatforms` only: the whole list, trimmed, fetched once.
    std::vector<PlatformRow> platforms;
    bool platforms_loaded = false;
  };

  TimePoint Now() const;

  /// The cursor named, or `cursors_.end()`. The caller holds `mutex_`.
  std::vector<Entry>::iterator Find(ipc::Cursor cursor);
  void ReapLocked(TimePoint now);
  void EvictOldestLocked();

  /// Build a page out of what is already in memory. The caller holds `mutex_`.
  ipc::Error FillFromQueueLocked(Entry& entry, ipc::ListPage* page);
  ipc::Error FillFromPlatformsLocked(Entry& entry, ipc::ListPage* page);

  /// Advance `entry`'s offset by what fitted and set `has_more`. See the
  /// definition for the one case it refuses.
  static ipc::Error PageOf(Entry& entry, ipc::ListPage* page, std::int64_t served,
                           std::int64_t count);

  /// Turn fetched rows into a page. Runs **outside** `mutex_`, because it reads
  /// the card for `on_disk` and the queue for `queued`. `card` is the
  /// filesystem copied out under the lock, so a `UseCard` during a fetch cannot
  /// swap it mid-page.
  ipc::ListPage BuildRomPage(const RomPage& fetched, std::int64_t offset,
                             std::int32_t page_size, fs::FileSystem* card,
                             const config::Config& config) const;

  /// True when this rom's bytes look like they are already on the card.
  static bool OnDisk(const RomRow& rom, fs::FileSystem* card, const config::Config& config);

  const ConfigSource config_;
  download::Queue& queue_;
  const Clock clock_;

  mutable std::mutex mutex_;
  std::vector<Entry> cursors_;

  /// Only ever increases, so a reclaimed id is never handed out again.
  ipc::Cursor next_id_ = 1;

  http::HttpClient* client_ = nullptr;
  std::string bearer_token_;
  fs::FileSystem* filesystem_ = nullptr;

  /// Swapped under `mutex_` and copied out with the two backends, for `Pump()`'s
  /// reason: it is called from the worker thread, outside the lock.
  AuthObserver auth_observer_;
};

}  // namespace rommsync::lists

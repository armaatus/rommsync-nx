// sys-rommsync entry point.
//
// It initialises the services the engine will need, registers the `rommsync`
// IPC service, and spends the rest of the process's life answering it (M4-1,
// #23). Since M1-7 (#126) that includes a **transport**: `sys-rommsync` holds an
// `http::HttpClient` of its own -- the Horizon `ssl` one under `http/` -- so the
// engine that was proven against a real RomM on a laptop can reach a server from
// here too. M1-6 (#123) installs it in the pairing seam below, so `StartPair` is
// answered here rather than refused. Since M7-2 (#37) it also starts the
// **worker**: one thread that runs sync ticks on a schedule and drives the list
// paging, which is what makes `SyncNow` start something, the library browsable
// on a console, and `auth.json` a file this build writes rather than only reads
// (see `engine.hpp`).
//
// The service is registered inside `__appInit`, while `sm` is up, because a
// registered port outlives the session that registered it (`ipc/server.hpp`).
// The **session** is a separate question, and since M9-1 (#195) it is held for
// the life of the process: libnx re-opens `sfdnsres` off it on every
// `getaddrinfo`, so closing it made every `server.url` naming a host
// unresolvable. `__appInit` is also where every service acquisition is now
// *bounded* -- an unregistered service makes `sm` defer a request forever, which
// left this process inert with no log and no crash report. Both are argued where
// they happen, at the bottom of `__appInit`.
//
// What this also proves, every CI run, is that core/ still builds for aarch64:
// every translation unit under core/src is compiled into this target
// (../switch.mk), so one that quietly became host-only breaks the build here
// rather than months later.
//
// None of it has run. It is exercised in Ryujinx as a manually-launched build
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).

#include <switch.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "boot_wait.hpp"
#include "card.hpp"
#include "engine.hpp"
#include "http/http_wire.hpp"
#include "http/ssl_http_client.hpp"
#include "ipc/server.hpp"
#include "power.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/core.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/log.hpp"
#include "rommsync/play_sessions.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/version.hpp"
// Named directly rather than through `engine.hpp`, because the heap table below
// is built out of `kThreadStackBytes`: dropping the include from `engine.hpp`
// should break the thread code, not the budget (M9-2, #207).
#include "sized_thread.hpp"

namespace {

// The whole heap this process ever has. Derived, since M1-7 (#126), rather than
// inherited from devkitPro's template -- the transport is what made the old
// 0x80000 a number nobody had added up -- and re-derived by M9-2 (#207), which
// found the table wrong by more than the margin it claimed:
//
//   | term                                     | constant                | bytes   |         |
//   |------------------------------------------|-------------------------|---------|---------|
//   | bsd transfer memory, trimmed config      | `kHeapSocketMemory`     | 0x1D000 | 116 KiB |
//   | `state.db` baseline at its bound         | `kHeapStateBaseline`    | 0x40000 | 256 KiB |
//   | one in-flight transfer buffer            | `kHeapTransferBuffer`   | 0x4000  |  16 KiB |
//   | the largest buffered response            | `kHeapListResponse`     | 0x7D000 | 500 KiB |
//   | two thread stacks (M1-6, M7-2)           | `kHeapThreadStacks`     | 0x44000 | 272 KiB |
//   | the log's in-memory tail (M7-3)          | `kHeapLogTail`          | 0x1800  |   6 KiB |
//   | the play-session buffer (M7-4)           | `kHeapPlaySessions`     | 0x8000  |  32 KiB |
//   | the directories open at once (M9-2)      | `kHeapOpenDirectories`  | 0x2000  |   8 KiB |
//   | newlib arena overhead and fragmentation  | `kHeapNewlibOverhead`   | 0x8000  |  32 KiB |
//   | **peak**                                 | `kHeapPeak`             | 0x135800 | 1238 KiB |
//
// The table is written this way so that something other than a reader adds it
// up. `tests/test_heap_budget.py` totals these rows and compares each against
// the `static_assert` that pins its constant, because for two milestones the
// table and the code disagreed and only the code was checked: two `std::thread`s
// were budgeted at 0x8000 each and cost 0x20000 each, an open `DIR` cost ~24 KiB
// that had no row at all, and both margins quoted below were arithmetic nobody
// had done. With `-fno-exceptions` (`switch.mk`) a `bad_alloc` here is
// `std::terminate` on a console with no crash report, so the margin is the
// only thing between a wrong table and that.
//
// The four terms that are easiest to get wrong, and why each is what it is:
//
//   * **A response is buffered whole**, because `Send` returns a `std::string`
//     (`http.hpp`) and nothing in this client caps it. Three can be large, and
//     all three are measured against the fixture RomM rather than guessed:
//     `/api/platforms` is an unpaged bare array in 5.2.0 at ~800 bytes a row, so
//     `lists::kMaxPlatforms` of them is ~200 KiB; the overlay's own paging at
//     `ipc::kMaxPageSize` = 64 rows is ~136 KiB; and **the rom index is the
//     biggest by a distance** -- `roms::FetchRomIndex` asks for
//     `roms::kDefaultPageSize` = 200 rows, measured at 2,119 bytes a row against
//     fixture roms that carry almost no metadata, so 500 KiB is the rounded-up
//     term. The largest of the three is the row. **They are bounds on what the
//     server sends, not ones this client enforces** -- the row counts are
//     bounded (`list_service.hpp`, `rom_index.hpp`), the row widths are RomM's --
//     which is why the numbers are written down here with where they came from.
//
//     **This one term is 40% of the heap, and it buys only fewer requests.**
//     Dropping `roms::kDefaultPageSize` to `ipc::kMaxPageSize`'s 64 would take
//     ~340 KiB off this table and off the resident image, at three requests per
//     index fetch instead of one. That is a `core/` behaviour change and it is
//     M9-19's (#217) to decide, not this table's to assume.
//   * **Two threads, not one, and each costs 0x22000.** M1-6 (#123) starts a
//     pairing thread and M7-2 (#37) starts the worker that drives `PumpLists`
//     and the sync tick. Their stacks are now this process's own number rather
//     than devkitA64's undeclared 128 KiB (`sized_thread.hpp`), and the term is
//     the stack plus what libnx allocates beside it -- thread-local storage, a
//     `struct _reent`, and the page a `memalign` wastes at the front.
//   * **The log keeps its last lines in RAM**, so `GetLog` can answer without
//     going near the card (log.hpp). It is `kTailLines * kMaxLineBytes` at
//     worst, and it is a term here rather than a cost nobody added up. That is
//     the **content**: the per-`std::string` header and the deque's blocks are
//     allocator overhead and belong to the newlib term below, which is what that
//     term is for.
//   * **An open `DIR` is not free.** fsdev allocates
//     `sizeof(FsDirectoryEntry)` -- 784 bytes -- times
//     `__nx_fsdev_direntry_cache_size` for every `::opendir`, and holds it until
//     `closedir` (fs_dev.c). libnx defaults that to 32, which is ~24.5 KiB per
//     open directory; this build sets it to 1, below.
//
// Every term is a bound something else enforces, and each of them moves with
// this constant rather than independently:
//   * the transfer memory is `SocketBudget` in `http/ssl_http_client.hpp`, and
//     `socketInitializeDefault()` -- 2.25 MiB -- is the one call this process
//     must never make (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision);
//   * the baseline is `state::kMaxStateBytes` + `state::kMaxRecords`, the one
//     term that grows with the library rather than being a fixed buffer, and it
//     is counted twice: the file's text and then the parsed rows it becomes,
//     which cost more than the text they came from (`state_db.hpp`);
//   * the transfer buffer is `kTransferBufferSize` in `http/http_wire.hpp` --
//     one per in-flight request, because roms stream to file and never sit in
//     RAM whole;
//   * the buffered response is the larger of `lists::kMaxPlatforms` and
//     `roms::kDefaultPageSize` times their row estimates below;
//   * the thread stacks are `kThreadStackBytes` (`sized_thread.hpp`), which is
//     where the derivation of that number lives;
//   * the tail is `log::kTailLines` times `log::kMaxLineBytes`, and
//     `ipc::kMaxLogLines` is the first of those rather than a second number;
//   * the play-session buffer is twice `play::kMaxBufferBytes` -- the file's
//     text and then the rows it becomes -- and it is the one term here that is
//     *not* held for the life of the process: it is read at `Load` and rewritten
//     once a tick (`play_sessions.hpp`). It is counted anyway, because a peak is
//     a peak.

/// What one platform's JSON weighs on the wire. Measured against the fixture
/// RomM 5.2.0 (`GET /api/platforms`: 3184 bytes for four rows) and rounded up.
/// Not a bound anything enforces -- `kMaxPlatforms` bounds the count and RomM
/// decides the width -- which is exactly why it is written down here with its
/// provenance rather than folded into a total: a reader can disagree with this
/// number, and cannot disagree with a constant that has no origin.
constexpr size_t kPlatformJsonBytes = 800;

/// What one rom's JSON weighs on the *index* page, which is a different schema
/// from the overlay's list and a different bound from `kPlatformJsonBytes`.
///
/// Measured the same way and against the same fixture RomM 5.2.0:
/// `GET /api/roms?limit=200&with_char_index=false&with_filter_values=false&with_rom_id_index=false`
/// is 14,837 bytes for seven rows -- 2,119 average, 2,109 at the widest. Rounded
/// up to 2.5 KiB rather than to the measurement, because the fixture's roms are
/// homebrew with almost no metadata and a library with full IGDB rows is wider.
/// Like `kPlatformJsonBytes` this is an estimate with a provenance, not a bound
/// anything enforces; `roms::kDefaultPageSize` bounds the count and RomM decides
/// the width.
constexpr size_t kRomJsonBytes = 2560;

/// How many entries fsdev caches per open `DIR`, against libnx's default of 32
/// (M9-2, #207).
///
/// **One**, which is what SysDVR sets and for the same reason: the cache is
/// `sizeof(FsDirectoryEntry)` -- 784 bytes -- per entry per *open directory*
/// (fs_dev.c), and at 32 that is ~24.5 KiB off a heap this size for as long as a
/// scan holds the handle. What it buys is one `fsDirRead` per 32 entries instead
/// of one per entry -- and `card.cpp`'s `List` already spends a `::stat` on
/// every entry it reads, so the cache saves a minority of the IPC on a walk
/// nobody is waiting for, and charges the heap for it for as long as the handle
/// is open.
constexpr u32 kDirectoryEntryCache = 1;

/// How many `DIR`s this process holds open at once.
///
/// Two, because two threads can list at the same time: the worker walks the save
/// folders during a tick (`save_scan.hpp`) while the IPC thread answers a
/// `ListDirectory`. Neither nests -- `card.cpp` reads a directory whole and
/// closes it before it recurses -- so two is the bound rather than an estimate.
constexpr size_t kMaxOpenDirectories = 2;

/// What fsdev, newlib and the allocator cost per open `DIR` *besides* the entry
/// cache: fsdev's `fsdev_dir_t`, newlib's `DIR_ITER` and `DIR`, and the chunk
/// headers on the two allocations they arrive in.
constexpr size_t kOpenDirectoryFixedBytes = 0x400;

/// What one open `DIR` costs, **derived from the cache rather than beside it**.
///
/// Writing this as a literal is how the table drifts: the whole of §2 of #207 is
/// that `__nx_fsdev_direntry_cache_size` moved the cost of a `DIR` and nothing
/// downstream noticed. Multiplying it here means putting the cache back to
/// libnx's 32 fails `static_assert(kHeapOpenDirectories == 0x2000)` rather than
/// passing quietly at eight times the real price.
constexpr size_t kOpenDirectoryBytes =
    (kDirectoryEntryCache * sizeof(FsDirectoryEntry) + kOpenDirectoryFixedBytes + 0xFFF) &
    ~size_t{0xFFF};

// The terms of the table above, as constants the compiler adds up. Each is
// pinned to its row by a `static_assert` below, so a bound that moves is a red
// build here rather than a table nobody re-totalled.
constexpr size_t kHeapSocketMemory = rommsync::sysmodule::ExpectedBsdTransferMemory({});
constexpr size_t kHeapStateBaseline = 2 * rommsync::state::kMaxStateBytes;
constexpr size_t kHeapTransferBuffer = rommsync::sysmodule::kTransferBufferSize;
// The largest of the three buffered responses, not the first one anybody wrote
// down: until M9-2 (#207) this term named only the platforms list, and the rom
// index -- twice its size, and fetched every tick -- was not in the table at all.
constexpr size_t kHeapPlatformsResponse = rommsync::lists::kMaxPlatforms * kPlatformJsonBytes;
constexpr size_t kHeapRomIndexResponse = rommsync::roms::kDefaultPageSize * kRomJsonBytes;
constexpr size_t kHeapListResponse = kHeapPlatformsResponse > kHeapRomIndexResponse
                                         ? kHeapPlatformsResponse
                                         : kHeapRomIndexResponse;
constexpr size_t kHeapThreadStacks =
    2 * (rommsync::sysmodule::kThreadStackBytes + rommsync::sysmodule::kThreadHeapOverheadBytes);
constexpr size_t kHeapLogTail = rommsync::log::kTailLines * rommsync::log::kMaxLineBytes;
constexpr size_t kHeapPlaySessions = 2 * rommsync::play::kMaxBufferBytes;
constexpr size_t kHeapOpenDirectories = kMaxOpenDirectories * kOpenDirectoryBytes;
constexpr size_t kHeapNewlibOverhead = 0x8000;

constexpr size_t kHeapPeak = kHeapSocketMemory + kHeapStateBaseline + kHeapTransferBuffer +
                             kHeapListResponse + kHeapThreadStacks + kHeapLogTail +
                             kHeapPlaySessions + kHeapOpenDirectories + kHeapNewlibOverhead;

// The arithmetic above, checked by the compiler rather than by a reader. It is
// the transfer memory that this is really about: the trimmed socket config is
// the difference between a working engine and one that dies at
// `socketInitialize`, and the number is easy to change by accident and
// impossible to notice off a console. The rest are here because M9-2 (#207)
// found every one of them able to move without anything noticing.
static_assert(kHeapSocketMemory == 0x1D000,
              "the bsd transfer memory is not the trimmed 116 KiB M0-1 measured");
static_assert(kHeapStateBaseline == 0x40000, "state::kMaxStateBytes moved; retotal the table");
static_assert(kHeapTransferBuffer == 0x4000, "kTransferBufferSize moved; retotal the table");
static_assert(kHeapListResponse == 0x7D000,
              "roms::kDefaultPageSize or lists::kMaxPlatforms moved; retotal the table");
static_assert(kHeapThreadStacks == 0x44000, "kThreadStackBytes moved; retotal the table");
static_assert(kHeapLogTail == 0x1800, "log::kTailLines moved; retotal the table");
static_assert(kHeapPlaySessions == 0x8000, "play::kMaxBufferBytes moved; retotal the table");
static_assert(kHeapOpenDirectories == 0x2000,
              "the direntry cache or the open-directory bound moved; retotal the table");
// The one term with no bound behind it, so this pin is what binds the constant
// to its row rather than a second reader of a bound. Changing the term is a red
// build here, which is the point: it is the term a reader is likeliest to nudge.
static_assert(kHeapNewlibOverhead == 0x8000, "the newlib arena term moved; retotal the table");
static_assert(kHeapPeak == 0x135800, "the table above no longer sums to its peak row");

// 0x150000 leaves 0x1A800 -- 106 KiB -- over that peak, which is the margin a
// process nobody can attach a debugger to needs. It is the only margin stated
// here on purpose: #207's prose quoted two, 94 KiB and 78 KiB, and the
// arithmetic gave neither.
//
// **It grew by 0x90000 in M9-2**, from 0xC0000: 576 KiB more `.bss` in a
// resident image that was ~2.00 MiB, against an Atmosphere third-party sysmodule
// budget that is 7 MB on HOS 21.0.0+ and shared with everything else the user
// installed (M9-9, #200). None of it is new cost -- the thread stacks were
// always allocated and the rom index page was always fetched -- but all of it is
// newly *declared*, and that is the point: an undeclared allocation on a
// `-fno-exceptions` build is `std::terminate` with no crash report.
//
// **Two thirds of the growth is one term**, `kHeapListResponse`, and M9-19
// (#217) is where the case for taking it back sits. M9-9's two unwind flags take
// ~640 KiB off the same image, which is more than the whole of this.
//
// **What is still NOT in this table**, and is M9-19's rather than this file's:
// `roms::kMaxIndexRoms` is 20,000 and a `roms::Rom` is 112 bytes on aarch64, so
// the index `RunOneTick` holds for the length of a tick is bounded at ~2.3 MiB
// -- larger than this whole heap, which is why it has no row here rather than a
// row that would not fit. `fs::kMaxDirectoryEntries` is 4,096 at 56 bytes an
// entry, and `card.cpp` grows past it before it pops back, so a listing peaks
// near 448 KiB. Both are bounds set without reference to this heap, and both
// want a number derived from it rather than a term budgeted for the number they
// have.
constexpr size_t kInnerHeapSize = 0x150000;
constexpr size_t kHeapMargin = kInnerHeapSize - kHeapPeak;

static_assert(kHeapPeak < kInnerHeapSize, "the heap no longer covers the peak in the table above");
static_assert(kHeapMargin == 0x1A800,
              "the margin in the sentence above is no longer the one left");

alignas(16) u8 g_inner_heap[kInnerHeapSize];

/// The registered port, claimed in `__appInit` and served in `main`. A file
/// scope variable because `__appInit` takes no arguments and returns nothing --
/// it is libnx's hook, not ours.
Handle g_service_port = INVALID_HANDLE;

/// The console's serial, read in `__appInit` for the same reason: `set:sys` is
/// only open there. Empty when it could not be read, which is a state
/// `IdentitySeed` has an answer for and a placeholder would not be.
///
/// It never leaves this process. `auth::DeriveDeviceIdentity` hashes it with a
/// published salt and the hash is what RomM sees; the serial identifies the
/// hardware and, through a warranty record, a person (docs/SECURITY.md).
char g_serial[0x18] = {};

/// The one `http::HttpClient` this process has: the Horizon `ssl` backend
/// (M1-7, #126). File scope because it outlives every caller and because there
/// is only ever one -- the download worker and the sync engine share it, which
/// is what `http.hpp` requires a backend to be safe for.
///
/// Built at start rather than on the first request, for the reason a sysmodule
/// does everything at start: a failure here is a line in a boot log, and the
/// same failure under a user's thumb is a pairing screen that never moves.
/// Building it costs a heap allocation and nothing else -- the `ssl` context and
/// the socket are made when a request is (`ssl_http_client.cpp`).
std::unique_ptr<rommsync::http::HttpClient> g_http;

/// The card, behind `fs::FileSystem` (M7-2, #37). File scope for `g_http`'s
/// reason: the worker holds it for the life of the process, and there is only
/// ever one.
std::unique_ptr<rommsync::fs::FileSystem> g_card;

/// `sdmc:/config/rommsync/rommsync.log`, and the reason there is one (M7-3,
/// #38). File scope for `g_http`'s reason: it outlives every caller, every
/// thread writes through it, and there is only ever one.
///
/// **It is installed in `main`, not in `__appInit`.** Nothing may block boot,
/// and this opens a file on the card -- but more to the point, the directory has
/// to exist first, and making it needs the `fs::FileSystem` backend that is
/// built below. What is written before it is installed is not lost: the log
/// keeps its last lines in memory whether a sink exists or not (log.hpp), so
/// `GetLog` answers them and only the *file* starts where the sink does.
std::unique_ptr<rommsync::log::FileSink> g_log;

/// The Horizon half of `io::FileSync` (#16): make what was just written
/// durable, before the rename that publishes it.
///
/// `fsdevCommitDevice` rather than a per-file sync, and **not** because there is
/// no per-file one: `fsync(fileno(fp))` compiles and links against libnx --
/// devkitA64's newlib routes it through `fsdev_fsync` to `fsFileFlush` (#195
/// corrected the claim that it does not). It is the wrong call anyway.
/// `fsFileFlush` pushes one file's buffered writes at the `fs` service; what
/// hard rule 2 needs is the *commit* that makes a journalled write survive a
/// power cut, and on Horizon that is `fsFsCommit`, which `fsdevCommitDevice`
/// reaches. It covers the staged file the way the contract in `atomic_file.hpp`
/// allows: the path is ignored because everything on this card is committed
/// together.
///
/// **What it buys is hard rule 2 on a console that loses power**: without it a
/// save's backup can be renamed into place while the copied bytes are still only
/// in a cache, which is a backup that reads as present and holds nothing
/// (docs/SYNC_PROTOCOL.md#backups). It is also the reason to keep the writes
/// this runs on rare -- one per record and one per backup, not one per chunk.
///
/// The name carries its colon: newlib's `FindDevice` reads a name without one as
/// "the default device", which is right only by accident.
bool HorizonFileSync(const std::string&) {
  return R_SUCCEEDED(fsdevCommitDevice("sdmc:"));
}

/// What `client_device_identifier` is derived from on this console.
///
/// `stable` is the serial `__appInit` read, and empty when it could not be read
/// -- never a placeholder, because every console handed the same one would
/// derive the same identifier and RomM would treat them as one device
/// (`device_identity.hpp` is explicit about it). `entropy` is the fallback that
/// makes that case unlinkable rather than shared.
///
/// `randomGet` rather than `csrngGetRandomBytes`: libnx seeds its ChaCha from
/// the kernel's own `InfoType_RandomEntropy` for this process, which is a
/// syscall this NPDM already allows -- where the `csrng` service would be one
/// more capability in `service_access` for the same bytes.
rommsync::auth::IdentitySeed ConsoleIdentitySeed() {
  rommsync::auth::IdentitySeed seed;
  seed.stable = g_serial;

  // Twice `kMinimumEntropyBytes`, because this is the value a console with no
  // readable serial is identified by for the life of its SD card.
  unsigned char bytes[2 * rommsync::auth::kMinimumEntropyBytes] = {};
  randomGet(bytes, sizeof(bytes));
  seed.entropy.assign(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  return seed;
}

/// What `__appInit` could not say at the time (M9-1, #195).
///
/// **Constant-initialised, and it has to be.** `__libnx_init` calls `__appInit`
/// *before* `__libc_init_array`, so a file-scope object `__appInit` touches is
/// used before its constructor would have run. Everything at this scope today is
/// clean -- `g_inner_heap` and `g_serial` are arrays, `g_service_port` is a
/// `Handle`, and the three `std::unique_ptr`s have `constexpr` default
/// constructors and are touched only from `main` -- and the rule that keeps it
/// clean is this: **nothing `__appInit` reads or writes may have a runtime
/// constructor.** `boot::Journal` is fixed character storage for exactly that
/// reason and static_asserts it (boot_wait.hpp).
///
/// It is flushed in `main`, once there is a log to flush it to.
rommsync::sysmodule::boot::Journal g_boot{};

/// `sm`, asked whether a service is registered rather than for the service.
///
/// `smGetService` is the call that hangs: Atmosphere's sm **defers** a request
/// for a service that is in this process's SAC but not yet registered, and a
/// deferred request is never answered (`sm_service_manager.cpp`). Its
/// `AtmosphereHasService` -- command **65100** -- answers instead, which is what
/// makes a bounded wait possible at all. libnx does not export it, so the
/// dispatch is here; it is ten lines and Atmosphere's own signature
/// (`sm_user_interface.hpp`).
///
/// **tipc, not cmif**, because that interface is `AMS_TIPC_DEFINE_INTERFACE` and
/// the extension commands exist nowhere else. That also makes the probe
/// Atmosphere-only, which is not a limitation worth working around: a
/// `/atmosphere/contents` sysmodule has no other host. An `sm` that will not
/// answer it reports `Probed() == false` and the caller stops rather than
/// waiting out a budget for an answer that is not coming.
///
/// **What it does not cover, stated rather than implied.** `GetServiceHandle`
/// defers on four conditions and `HasService` reflects only the first:
///
///   * `service_info == nullptr` -- not registered. **This one**, and it is the
///     failure #195 is about: a name in the SAC that nothing ever registers.
///   * `ShouldDeferForInit(service)` -- `fsp-srv` alone, until `sm:m` is told
///     the initial defers are over. `pm` does that at its own startup, before
///     boot2 reaches `/atmosphere/contents`, so it is closed by the time this
///     runs.
///   * `HasFutureMitmDeclaration(service)` -- a mitm module has called
///     `AtmosphereDeclareFutureMitm` and not yet installed. `HasService` says
///     yes and the acquisition still parks.
///   * `mitm_info->waiting_ack` -- a mitm session mid-acknowledgement.
///
/// The last two are windows another module opens and closes within its own
/// startup, and nothing sm exposes can be polled for either. So the bound this
/// class buys is a bound on *registration*, not a proof that the next call
/// returns -- which is the difference between the failure mode that has no
/// symptom and one that lasts as long as another sysmodule's init.
class SmWaiter final : public rommsync::sysmodule::boot::Waiter {
 public:
  bool Ready(const char* service) override {
    bool present = false;
    const SmServiceName name = smEncodeName(service);
    const Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, present);
    probed_ = R_SUCCEEDED(rc);
    return probed_ && present;
  }

  bool Probed() const override { return probed_; }

  void Sleep(std::chrono::milliseconds slice) override {
    svcSleepThread(static_cast<u64>(slice.count()) * 1000000ULL);
  }

 private:
  bool probed_ = true;
};

/// Whether `timeInitialize()` ran and succeeded. `__appExit` may not unwind what
/// `__appInit` skipped: libnx's service guard counts, and an exit without an
/// init takes the count below zero.
bool g_time_up = false;

/// The same, for `pscmInitialize()` (M9-4, #208), and for the same reason: an
/// unmatched `pscmExit` takes libnx's refcount below zero.
bool g_psc_up = false;

/// Whether the "this sm will not answer 65100" note has already been taken. One
/// note, not one per service: it is the same answer for every question that
/// follows, and a journal full of it would push out the note that matters.
bool g_sm_unaskable = false;

/// Wait for `service`, say so in the journal when it does not come, and answer
/// whether the acquisition below may go ahead.
///
/// Returns false on a timeout. What the caller does about that is the caller's:
/// the services this process cannot run without abort, and the ones it can run
/// degraded without are skipped. Either way the reason is written down here,
/// which is the whole difference from before -- a `__appInit` that parked in
/// `sm` left no log, no crash report and no symptom beyond an overlay saying
/// "not running".
///
/// **An `sm` that cannot be asked returns true**, and that is the conservative
/// direction rather than the convenient one. `AtmosphereHasService` exists only
/// on Atmosphere's sm, which is the only host a `/atmosphere/contents`
/// sysmodule has -- but if the question ever cannot be put, refusing to start
/// would trade a rare hang for a certain failure. So the note is taken, it goes
/// to the debug channel *before* the call that might park, and the acquisition
/// proceeds exactly as it did before #195. The bound exists wherever the
/// question can be asked, which is everywhere this ships.
bool MayProceedWith(const char* service, rommsync::sysmodule::boot::Waiter& waiter) {
  namespace boot = rommsync::sysmodule::boot;

  // Asked and answered. An `sm` that would not answer 65100 for one service will
  // not answer it for the next, so every remaining acquisition would otherwise
  // pay a round trip to learn that again -- `WaitFor` asks once before it looks
  // at `Probed()`, which is right for the first question and pure cost after it.
  if (g_sm_unaskable) return true;

  const boot::Outcome outcome = boot::WaitFor(service, waiter);
  if (outcome.ready) return true;

  char line[boot::kMaxNoteBytes] = {};
  if (!waiter.Probed()) {
    g_sm_unaskable = true;
    std::snprintf(line, sizeof(line),
                  "rommsync: sm does not answer AtmosphereHasService; waits are unbounded");
  } else {
    std::snprintf(line, sizeof(line), "rommsync: %s never registered after %lldms", service,
                  static_cast<long long>(outcome.waited.count()));
  }
  boot::Note(g_boot, line);
  svcOutputDebugString(line, std::strlen(line));
  return !waiter.Probed();
}

/// ...and abort when this process cannot do its job without it.
///
/// A `diagAbortWithResult` is not silence: Atmosphere writes
/// `/atmosphere/crash_reports/`, which is a file a user can find and attach
/// (docs/TROUBLESHOOTING.md). Parking in `sm` produces neither that nor a log
/// line, which is why a bounded wait that ends in an abort is strictly better
/// than an unbounded one that ends in nothing.
void RequireService(const char* service, rommsync::sysmodule::boot::Waiter& waiter) {
  if (!MayProceedWith(service, waiter)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_Timeout));
  }
}

/// One boot line, to a debugger and to the card.
///
/// `svcOutputDebugString` is what a Ryujinx run and an attached debugger see and
/// is all this process had before M7-3 (#38); the log is what a *user* can read,
/// and what docs/TROUBLESHOOTING.md asks them to attach. Both, because they
/// reach different people and neither is a superset of the other.
///
/// `level` because the journal's lines are `warn` and everything else here is
/// `info`: a journal note exists only when something did not come up (M9-1,
/// #195), and a user scanning the file for the first `warn` should land on the
/// reason their console is degraded rather than on the version line.
void Log(rommsync::log::Level level, const std::string& line) {
  svcOutputDebugString(line.c_str(), line.size());
  rommsync::log::Write(level, rommsync::log::Event::kBoot, line);
}

/// The `info` one, which is most of them.
void Log(const std::string& line) { Log(rommsync::log::Level::kInfo, line); }

}  // namespace

extern "C" {

// A sysmodule has no applet session and wants one FS session, not the several
// libnx opens for homebrew.
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// How many directory entries fsdev caches per open `DIR` (M9-2, #207). libnx
// declares this weak and defaults it to 32, which is 784 bytes an entry -- about
// 24.5 KiB -- taken out of `g_inner_heap` by every `::opendir` in `card.cpp` and
// held until `closedir` (nx/source/runtime/devices/fs_dev.c). It had no term in
// the table above because nothing here had ever named it. See
// `kDirectoryEntryCache` for why the deliberate value is 1.
u32 __nx_fsdev_direntry_cache_size = kDirectoryEntryCache;

void __libnx_initheap(void) {
  extern void* fake_heap_start;
  extern void* fake_heap_end;

  fake_heap_start = g_inner_heap;
  fake_heap_end = g_inner_heap + sizeof(g_inner_heap);
}

// The clock this process reads, and the one grant that makes it readable.
//
// libnx defaults `__nx_time_service_type` to `TimeServiceType_User`, so
// `timeInitialize()` asks sm for **`time:u`** -- and `sys-rommsync.json` grants
// `time:s`. sm validates the SAC before it looks at whether the service is
// registered and returns `sm::ResultNotAllowed` (0x1015) straight away
// (`sm_service_manager.cpp`), so that is a clean failure on every boot rather
// than a race: the clock never comes up, `core/`'s
// `std::chrono::system_clock::now()` answers the epoch, and every save this
// client would stamp is one docs/SYNC_PROTOCOL.md refuses (M7-2, #37). TLS
// wants a sane clock too -- `SslVerifyOption_DateCheck` fails a handshake with
// `0x25E7B` on a skewed one.
//
// **This line, not an SAC edit**, and the difference is a race. `time:u` and
// `time:a` are registered by **glue**, near the end of Atmosphere's
// `AdditionalLaunchPrograms`; `time:s` comes from **psc**, which boot2 launches
// first. Adding `time:u` to the SAC would trade a failure that always happens
// for one that sometimes does. `boot.clock` is what holds the declaration and
// the grant together (M9-1, #195).
TimeServiceType __nx_time_service_type = TimeServiceType_System;

void __appInit(void) {
  Result rc = smInitialize();
  if (R_FAILED(rc)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
  }

  // **Nothing below asks sm for a service it has not waited for first.** A
  // service in this process's SAC but not yet registered is not an error sm
  // returns -- it is a request sm *defers*, and never answers. `svcStartProcess`
  // is asynchronous, so boot2 does not wait on us: the console boots normally,
  // `sys-rommsync` is an inert process, and there is no crash report and no log,
  // because the log sink does not exist until `main`. That was this sysmodule's
  // worst realistic failure mode and it had no symptom at all (M9-1, #195).
  //
  // `boot.bounded` is the check that keeps it that way -- it reads this function
  // and fails on any initialiser without a `WaitForService` above it, so the
  // name in each call below is load-bearing.
  //
  // The bound is on **registration**, which is the condition with no symptom.
  // `SmWaiter` lists the three other things sm defers on and why none of them
  // can be polled for; each is a window another module closes inside its own
  // startup rather than one that lasts for the life of the console.
  SmWaiter sm;
  const auto WaitForService = [&sm](const char* service) { return MayProceedWith(service, sm); };
  const auto WaitForServiceOrAbort = [&sm](const char* service) { RequireService(service, sm); };

  // hosversionSet before anything version-gated is called; libnx assumes it.
  // Aborting rather than carrying on is the point: an unset host version reads
  // as 0, so every hosversionAtLeast() gate after this -- including the ones
  // inside fsInitialize() below -- silently takes the pre-1.0.0 path. A wrong
  // answer everywhere is worse than a refusal to start.
  WaitForServiceOrAbort("set:sys");
  rc = setsysInitialize();
  if (R_FAILED(rc)) {
    diagAbortWithResult(rc);
  }
  SetSysFirmwareVersion fw;
  rc = setsysGetFirmwareVersion(&fw);
  if (R_FAILED(rc)) {
    diagAbortWithResult(rc);
  }
  hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));

  // The console's serial, read here because this is the only moment `set:sys`
  // is open -- and read into `g_serial` rather than used, because what leaves
  // this process is a hash of it and never the value
  // (`rommsync/device_identity.hpp`, docs/SECURITY.md).
  //
  // **A failure leaves it empty and that is deliberate.** The platform layer
  // must fail rather than substitute: every console handed the same placeholder
  // would derive the same `client_device_identifier`, and RomM would treat them
  // as one device -- one console's saves overwriting another's. An empty
  // `stable` is what makes `DeriveDeviceIdentity` mint from entropy instead,
  // which is unlinkable and correct rather than shared and wrong.
  SetSysSerialNumber serial{};
  if (R_SUCCEEDED(setsysGetSerialNumber(&serial))) {
    serial.number[sizeof(serial.number) - 1] = '\0';
    std::snprintf(g_serial, sizeof(g_serial), "%s", serial.number);
  }
  setsysExit();

  // config.ini, token.dat, save staging and the download destinations all live
  // on the SD card, so fs is not optional for this process.
  //
  // `fsp-srv` is also the one name Atmosphere's sm defers even once registered,
  // until `sm:m` is told the initial defers are over -- but `pm` does that at
  // its own startup, long before boot2 launches anything out of
  // `/atmosphere/contents`, so by the time this runs the wait below is a
  // question about registration and nothing else.
  WaitForServiceOrAbort("fsp-srv");
  rc = fsInitialize();
  if (R_FAILED(rc)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
  }
  fsdevMountSdmc();

  // **`core/` calls `std::chrono::system_clock::now()`**, in `sync_execute.cpp`,
  // `state_sync.cpp` and `download.cpp`, and on Horizon that answers nothing at
  // all until time is initialised -- which has to happen here, while `sm` is
  // still open. M1-7 (#126) dropped `time:s` from the NPDM because nothing in
  // that build called it; the scheduler is what makes it matter, because a save
  // stamped from an uninitialised clock is one docs/SYNC_PROTOCOL.md refuses as
  // an epoch `updated_at` (M7-2, #37).
  //
  // Not fatal. A console whose clock will not initialise is one whose saves
  // cannot be stamped, and every path that needs a stamp already refuses an
  // epoch one -- where refusing to *start* would take the overlay, the settings
  // and the queue down with it.
  //
  // The result is read rather than discarded, and it goes in the journal: this
  // is the call #195 found asking for a service the npdm does not grant, and it
  // failed the same way on every boot with nobody able to see it.
  if (WaitForService("time:s")) {
    rc = timeInitialize();
    if (R_FAILED(rc)) {
      rommsync::sysmodule::boot::Note(g_boot, "rommsync: timeInitialize", rc);
    }
    g_time_up = R_SUCCEEDED(rc);
  }

  // **PSC, so this process finds out that the console is going to sleep**
  // (M9-4, #208). Without it the transition happens around us: `fsp-srv` and the
  // sockets are still in use when the services behind them go down, which is the
  // pattern that crashes consoles for other projects -- and for us it is a save
  // write cut in half, which is hard rule 2. `power.hpp` has the whole argument
  // and the crash reports.
  //
  // Registered here, where every service acquisition is, and **subscribed to in
  // `main`**: `pscmGetPmModule` needs a `power::Sink` to hand requests to, and
  // the engine that is one does not exist until then.
  //
  // Not fatal, and this is the one place the choice is arguable. A console with
  // no sleep handling is one that can lose a save, so aborting has a case -- but
  // it would take the overlay, the settings screen and the queue down with it
  // over a service Nintendo's own boot registers before `/atmosphere/contents`
  // is reached at all (`psc` is what publishes `time:s`, waited for just above).
  // So it is a `warn` in the log a user is asked to attach, and the client runs
  // as it did before this issue.
  if (WaitForService("psc:m")) {
    rc = pscmInitialize();
    if (R_FAILED(rc)) {
      rommsync::sysmodule::boot::Note(g_boot, "rommsync: pscmInitialize", rc);
    }
    g_psc_up = R_SUCCEEDED(rc);
  }

  // The transport, here rather than on first use, because a sysmodule does
  // everything at start: a failure here is a line in a boot log, and the same
  // failure under a user's thumb is a pairing screen that never moves. Its
  // failure is deliberately *not* fatal: a console with no network is one the
  // overlay still has to be able to open, read its settings on and see its
  // queue on, so the engine gets a client that answers `kConnectFailed` rather
  // than a process that refuses to start (`http/ssl_http_client.hpp`).
  //
  // It is also not a boot-time wait: nothing here talks to a network. The bsd
  // transfer memory this allocates out of `g_inner_heap` is the dominant term
  // in the budget above.
  //
  // Four names, because `NetworkInitialize` opens four sessions: `nifm:u` for
  // the connection probe, `bsd:u` and `sfdnsres` for `socketInitialize`, and
  // `ssl` for the TLS layer. **All four, including the probe**, and the `&&` is
  // the whole of the reason: `NetworkInitialize` acquires them itself, so one
  // name that is not registered is one request parked inside a call this
  // function cannot bound from outside. Short-circuiting also stops a console
  // that is plainly broken from spending the budget four times over; the note
  // names whichever one it stopped at.
  //
  // A missing `nifm:u` therefore costs the transport as well, which is more than
  // the probe alone is worth -- and it is the honest price of not being able to
  // bound a call from outside it. `NetworkInitialize` would have to take a
  // parameter to skip the probe, and the guards it would then need are plain
  // `bool`s that the worker and the pairing thread both read. A console missing
  // a service Nintendo's own boot registers is not the case to add a lock for.
  //
  // `ConsoleIsOnline` is not the thing that suffers: it answers **true** when
  // nifm was never opened, deliberately, so the worker still ticks and every
  // request logs the transport error a user can act on rather than waiting
  // silently on a probe that will never say yes (`ssl_http_client.hpp`).
  //
  // It is the only `nifmInitialize` in this build --
  // there was a second one here until #195, and since libnx refcounts it, the
  // second call's `NifmServiceType` was silently ignored, which is a trap and
  // not a redundancy.
  //
  // **The transport is not retried later, and that is now a decision rather
  // than an oversight.** `socketInitialize` is not refcounted -- a second call
  // answers `0xF59 AlreadyInitialized` forever -- so a lazy retry would have to
  // be guarded, and the guards in `NetworkInitialize` are plain `bool`s read
  // from the worker and the pairing thread both. What made a retry worth that
  // was the transient failure: `bsd:u` or `ssl` not registered *yet*. The wait
  // above is what removes it. What is left is a transfer memory that would not
  // fit, which is a heap failure a reboot does not fix either.
  const bool network = WaitForService("nifm:u") && WaitForService("bsd:u") &&
                       WaitForService("sfdnsres") && WaitForService("ssl");
  if (network) {
    rc = rommsync::sysmodule::NetworkInitialize();
    if (R_FAILED(rc)) {
      rommsync::sysmodule::boot::Note(g_boot, "rommsync: NetworkInitialize", rc);
    }
  }

  // Last, and aborting rather than carrying on: a
  // sysmodule that runs without its service is a process nothing can reach and
  // nothing can diagnose -- the overlay would report it as not running, which
  // is the one thing it would not be.
  rc = rommsync::sysmodule::RegisterPort(&g_service_port);
  if (R_FAILED(rc)) {
    diagAbortWithResult(rc);
  }

  // **No `smExit()` here, and that is the fix rather than an omission (#195).**
  //
  // The argument that used to be at the top of this file is right about the
  // registered *port*: it outlives the session that registered it, so a resident
  // process need not hold `sm` open to keep its own name. It does not extend to
  // DNS. libnx re-opens `sfdnsres` off the `sm` session on **every**
  // `getaddrinfo` -- `_sfdnsresDispatchImpl` begins with
  // `smGetServiceOriginal(&h, smEncodeName("sfdnsres"))` -- and
  // `posix_connection.cpp` falls back to `getaddrinfo` for anything that is not
  // a bare IPv4 literal. With the session closed, every `server.url` naming a
  // host -- `romm.local`, a NAS name, a DDNS name -- answered
  // `http::Error::kUnresolvedHost` on the console and nowhere else, which is why
  // no test caught it.
  //
  // The cost of holding it is one of sm's 87 user sessions, for the life of the
  // process. sys-clk holds one for the same reason. `boot.dns` is what keeps
  // this decision from quietly reverting to a comment.
}

void __appExit(void) {
  // `nifmExit` is `NetworkExit`'s, not ours: this process opens `nifm:u` once,
  // inside `NetworkInitialize` (#195).
  if (g_psc_up) pscmExit();
  if (g_time_up) timeExit();
  rommsync::sysmodule::NetworkExit();
  fsdevUnmountAll();
  fsExit();
  smExit();
}

}  // extern "C"

int main(int, char**) {
  // Before anything writes, and once: `io::SetFileSync` is process-wide and is
  // not meant to be swapped while a write is in flight (atomic_file.hpp). First
  // in `main` since M7-3 (#38) rather than after the boot lines, because the
  // card is now built above them and an ordering where a hook is installed after
  // the first thing that touches the card is one somebody eventually relies on.
  rommsync::io::SetFileSync(&HorizonFileSync);

  // The card, and the log on it, before anything else says anything (M7-3, #38).
  //
  // `MakeSdCard` is built here rather than where M7-2 (#37) built it -- it holds
  // no handle and no state (`card.hpp`), so making it early costs nothing -- and
  // `CreateDirectory` is here because `log::FileSink` cannot make its own:
  // `core/` has only standard headers, and a first boot on a fresh card would
  // otherwise drop exactly the lines a new user is asked for.
  //
  // **A card that refuses is not fatal and is not reported.** There is nowhere
  // left to report it to, and the log's in-memory tail still answers `GetLog` --
  // a client that stopped syncing over a log file it could not write would have
  // the tail wagging the dog. `fsdevMountSdmc` has already run, in `__appInit`.
  //
  // **What this costs at boot**, stated rather than left to be measured: one
  // `mkdir` and one append of a few dozen bytes. It is SD I/O on the boot path
  // and it is bounded -- `engine.Load()` a few lines below already reads five
  // files off the same card, and the rule that matters is that nothing here
  // touches the *network* (CLAUDE.md, "Never block boot"), which is still true.
  // Nothing in this block waits on anything.
  g_card = rommsync::sysmodule::MakeSdCard();
  static_cast<void>(g_card->CreateDirectory(rommsync::sysmodule::kConfigSdDir));
  g_log = std::make_unique<rommsync::log::FileSink>(
      std::string(rommsync::sysmodule::kConfigDir) + rommsync::log::kLogFileName);
  rommsync::log::SetSink(g_log.get());

  // What `__appInit` could not say at the time, now that there is somewhere to
  // say it (M9-1, #195). It is first, before even the version line, because a
  // boot that went wrong went wrong before this point -- and it is in the log
  // file docs/TROUBLESHOOTING.md asks a user to attach *and* in the in-memory
  // tail the overlay's `GetLog` reads, so a console whose card cannot be written
  // still shows the reason on screen. Ordinarily there is nothing here and this
  // costs one comparison.
  for (std::size_t i = 0; rommsync::sysmodule::boot::NoteAt(g_boot, i) != nullptr; ++i) {
    Log(rommsync::log::Level::kWarn, rommsync::sysmodule::boot::NoteAt(g_boot, i));
  }
  if (g_boot.dropped != 0) {
    Log(rommsync::log::Level::kWarn,
        "rommsync: " + std::to_string(g_boot.dropped) + " more boot notes were dropped");
  }

  // A crash dump or a debug log that cannot say which build produced it costs
  // an afternoon, and this is the cheapest possible answer. It goes to the
  // debugger and to the card, which is `Log`'s whole job -- and it is the first
  // line of the file docs/TROUBLESHOOTING.md asks a user to attach.
  //
  // `kUserAgent` rather than `version()`: they differ by the `rommsync-nx/`
  // prefix, and the prefixed one is what RomM records against every request
  // this console makes (`version.hpp`). A support thread that has the server's
  // logs and the console's should be reading the same string in both.
  Log(rommsync::kUserAgent);

  // Which way this console's `client_device_identifier` will be derived, and
  // whether this build has a transport at all. Two lines at boot because they
  // are the first two things anyone debugging a console wants: a device that
  // shows up twice in RomM is a `source` question (`device_identity.hpp`), and
  // a console that reaches nothing is a `NetworkReady()` one.
  const rommsync::auth::IdentitySeed seed = ConsoleIdentitySeed();
  const rommsync::auth::DerivedIdentity identity = rommsync::auth::DeriveDeviceIdentity(seed);
  Log(identity.ok() ? std::string("rommsync: device identity from ") +
                          rommsync::auth::ToString(identity.value.source)
                    : std::string("rommsync: no device identity: ") + identity.message);
  g_http = rommsync::sysmodule::MakeHorizonHttpClient();
  Log(rommsync::sysmodule::NetworkReady()
          ? "rommsync: ssl transport up"
          : "rommsync: no transport; every request will answer connect-failed");

  // Read once, here rather than per request: `GetStatus` and `GetConfig` are
  // documented never to fail and are polled every frame by the status screen,
  // so neither may go near the SD card (`ipc.hpp`). Since M5-3 (#30) the one
  // command that *does* -- `SetConfig` -- re-reads the file it just wrote and
  // swaps the live `Config`, which is what makes a setting changed from the
  // overlay take effect without a reboot.
  rommsync::sysmodule::SdEngine engine;
  engine.Load();

  // The transport M1-7 (#126) built, installed through the seam this issue
  // (M1-6, #123) added. #126 wrote this call out commented, because the seam was
  // not on `main` when it landed; this is that line, uncommented.
  //
  // After it, `StartPair` is answered on a console rather than refused: the
  // engine drives a real device-code attempt on a thread of its own, and the
  // overlay's pairing screen has a code to draw.
  engine.UsePairingBackend({g_http.get(), seed});

  // The other two seams, and the worker that makes them safe -- M7-2 (#37). The
  // card itself was built above, because the log needed it first.
  //
  // **`UseServer` and `StartWorker` are one commit and have to stay one line
  // apart.** `lists::Service` answers a page that needs a request with `kOk` and
  // `ListPage::pending` and makes the request in `Pump()`, so a client handed
  // over with no worker driving it turns "offline", which the browser draws,
  // into "pending" forever, which it cannot (`list_service.hpp`). The token is
  // left empty deliberately: `token.dat` is the engine's to read, and it re-reads
  // it when a pairing commits (`SdEngine::UseServer`).
  engine.UseServer(g_http.get(), "");
  engine.UseCard(g_card.get());
  engine.UseNetworkProbe(&rommsync::sysmodule::ConsoleIsOnline);
  engine.StartWorker();

  // **The console can now tell this process that it is going to sleep** (M9-4,
  // #208). After `StartWorker`, because the thing a sleep has to stop is the
  // worker -- a subscription taken before it would answer its first request by
  // waiting for a thread that does not exist yet.
  //
  // Held for the life of the process. It is never destroyed, because `main`
  // never returns; the destructor exists for the tests, which is where the
  // watcher's thread is actually joined.
  //
  // **Declared after `engine`, and it has to be.** `Quiesce` runs on the thread
  // this owns and reads the engine's members, so were the engine to be destroyed
  // first it would be torn out from under a quiesce in flight. Reverse
  // declaration order is what makes this go first; `SdEngine::Quiesce` records
  // the requirement.
  const std::unique_ptr<rommsync::sysmodule::power::Subscription> psc =
      rommsync::sysmodule::power::Subscribe(engine);
  Log(psc != nullptr ? "rommsync: subscribed to psc:m; sleep will be handled"
                     : "rommsync: no psc:m subscription; this console will not be told when "
                       "it sleeps");

  rommsync::ipc::ServiceCore core(engine);
  rommsync::sysmodule::ServiceServer server(core, g_service_port);
  // Does not return. A sysmodule that fell out of its service loop would sit in
  // the process list answering nothing.
  server.Run();
  return 0;
}

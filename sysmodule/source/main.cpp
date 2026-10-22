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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "boot_wait.hpp"
#include "card.hpp"
#include "engine.hpp"
#include "handle_budget.hpp"
#include "http/http_wire.hpp"
#include "http/ssl_http_client.hpp"
#include "ipc/server.hpp"
#include "power.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/core.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/log.hpp"
#include "rommsync/play_sessions.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/server_capabilities.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/state_sync.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/version.hpp"
// Named directly rather than through `engine.hpp`, because the heap table below
// is built out of `kThreadStackBytes`: dropping the include from `engine.hpp`
// should break the thread code, not the budget (M9-2, #207).
#include "sized_thread.hpp"
// For its `static_assert`: the deepest JSON document `json::kMaxDepth` lets
// through has to fit this thread's npdm-sized stack, and this is the thread
// (M9-18, #216).
#include "stack_budget.hpp"

namespace {

// The whole heap this process ever has. Derived, since M1-7 (#126), rather than
// inherited from devkitPro's template -- the transport is what made the old
// 0x80000 a number nobody had added up -- and re-derived by M9-2 (#207), which
// found the table wrong by more than the margin it claimed:
//
//   | term                                     | constant                | bytes   |         |
//   |------------------------------------------|-------------------------|---------|---------|
//   | bsd transfer memory, trimmed config      | `kHeapSocketMemory`     | 0x1D000 | 116 KiB |
//   | `state.db` baseline at its bound         | `kHeapStateBaseline`    | 0x10000 |  64 KiB |
//   | a download's buffer and an API call's    | `kHeapTransferBuffer`   | 0x14000 |  80 KiB |
//   | the largest buffered response or listing | `kHeapListResponse`     | 0x35400 | 213 KiB |
//   | the rom index at its bound (M9-19)       | `kHeapRomIndex`         | 0x40C00 | 259 KiB |
//   | a tick's saves at `kMaxSaves` (M9-20)    | `kHeapTickSaves`        | 0x36000 | 216 KiB |
//   | the list service's pages (M9-20)         | `kHeapListPages`        | 0x24800 | 146 KiB |
//   | two thread stacks (M1-6, M7-2)           | `kHeapThreadStacks`     | 0x44000 | 272 KiB |
//   | the PSC watcher's stack (M9-4)           | `kHeapWatcherStack`     | 0xA000  |  40 KiB |
//   | the log's in-memory tail (M7-3)          | `kHeapLogTail`          | 0x1800  |   6 KiB |
//   | the play-session buffer (M7-4)           | `kHeapPlaySessions`     | 0x8000  |  32 KiB |
//   | the directories open at once (M9-2)      | `kHeapOpenDirectories`  | 0x2000  |   8 KiB |
//   | newlib arena overhead and fragmentation  | `kHeapNewlibOverhead`   | 0x8000  |  32 KiB |
//   | **peak**                                 | `kHeapPeak`             | 0x173000 | 1484 KiB |
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
// The terms that are easiest to get wrong, and why each is what it is:
//
//   * **A response is buffered whole**, because `Send` returns a `std::string`
//     (`http.hpp`) and nothing in this client caps it. Three can be large, and
//     all three are measured against the fixture RomM rather than guessed:
//     `/api/platforms` is an unpaged bare array in 5.2.0 at ~800 bytes a row,
//     and both rom pages -- the index's `roms::kDefaultPageSize` and the
//     overlay's `ipc::kMaxPageSize`, 64 rows each since M9-19 (#217) -- are
//     ~2.1 KiB a row. **They are bounds on what the server sends, not ones this
//     client enforces** -- the row counts are bounded (`list_service.hpp`,
//     `rom_index.hpp`), the row widths are RomM's -- which is why the numbers
//     are written down below with where they came from.
//
//     A response costs its text **once** and **one row's document** beside it,
//     and both of those were made true by M9-19 rather than assumed. The body
//     is reserved to its `Content-Length` (`kMaxReservedBodyBytes`), where it
//     used to double its way there a read at a time; and each row is parsed,
//     read and dropped before the next (`json::ParseEach`), where the whole
//     page used to be one document -- at ~11 times its text, a 200-row page was
//     ~4.6 MiB of `json::Value`s that no row of this table had ever counted.
//     M9-20 (#246) closed the three ways round that M9-19 named. A body that
//     declares no length -- chunked, or ended by the close, which a reverse
//     proxy may send where RomM's own nginx never does (measured on 5.2.0) --
//     grows in powers of two to `kMaxUndeclaredBodyBytes` and is refused past
//     it, so it costs at most half again its cap (`kHeapUndeclaredBody`). A body
//     of the wrong shape -- a bare array where the envelope belongs -- is
//     refused before it is parsed (`json::ParseEach`), where it used to be
//     held whole at ~11x on its way to the refusal. And the rows a list page
//     becomes are counted where the worker holds them: a rom page's under
//     `kHeapTickSaves`, which the worker never holds at the same time, and the
//     platform snapshot under `kHeapListPages`. Every other response whose size
//     comes from an array -- the negotiate plan, the state listing, the device
//     list, and `queue.json` at boot -- is read the same way, and the ones this
//     row has to count are named in it below.
//
//     **The row is the largest of the things the worker holds one at a time**:
//     the platforms list, a rom page, the directory listings -- two at most, at
//     `fs::kMaxDirectoryEntries` each (`kHeapDirectoryListing`) -- the
//     heartbeat, the negotiate plan's text, the state listing's, and a body
//     that declared no length.
//     It is a `max()` and not a sum because all of them are taken on the worker
//     thread, one after another: a list kind that needs the network defers it to
//     `Pump()` (`list_service.hpp`), `RunWorker` runs either `RunOneTick` or
//     `PumpLists` in one iteration and never both (`engine.cpp`), and the tick
//     reads its listings after its index fetch has finished. **If list-serving
//     ever moves to a thread of its own, this row is wrong by whichever two
//     overlap** -- which is a second reason, beside `engine.hpp`'s, not to.
//   * **The rom index is held for the whole tick** (`engine.cpp`, deliberately
//     uncached), beside whichever of the above the tick is holding, so it is a
//     row of its own and adds. Its bound, `roms::kMaxIndexRoms`, is the number
//     this heap holds rather than the 20,000 it was -- ~4.7 MiB at ~248 bytes a
//     rom -- and the derivation is below the constants.
//   * **A tick holds every save several times over**, and until M9-20 (#246)
//     none of it had a row. The scan's records (`scan::ScanResult`, and while it
//     walks, its plan of folders), what is reported and what is targeted for
//     each save, the negotiate plan once it arrives, and what each operation
//     did, all held to the end of the tick beside the index and whichever
//     response the tick is reading. Measured on aarch64 that is ~1.7 KiB a save
//     at the tick's worst moment, ~0.8 MiB at the 512 saves `kMaxSaves` used to
//     allow -- more than half this heap for one row. So the row is the largest
//     of the tick's four phases at `scan::kMaxSaves`, and that bound is now the
//     number this heap holds rather than the other way round (below the
//     constants). The engine frees the scan's records before the plan arrives
//     (`engine.cpp`), which is what makes the scan and the plan two phases and
//     not one sum.
//   * **The list service holds pages between pumps.** One platform snapshot and
//     one built rom page, across every cursor (`lists::Service::Pump`, M9-20),
//     plus the page the IPC thread is handing over -- built out of the snapshot
//     on that thread, beside whatever the worker is doing. Before M9-20 every
//     cursor kept its own of each, `kMaxCursors` of them, and the header said
//     four times a number nothing reaches was still nothing; this table budgets
//     at bounds, not at what is reached.
//   * **Two threads, not one, and each costs 0x22000.** M1-6 (#123) starts a
//     pairing thread and M7-2 (#37) starts the worker that drives `PumpLists`
//     and the sync tick. Their stacks are now this process's own number rather
//     than devkitA64's undeclared 128 KiB (`sized_thread.hpp`), and the term is
//     the stack plus what libnx allocates beside it -- thread-local storage, a
//     `struct _reent`, and the page a `memalign` wastes at the front.
//   * **Three threads since M9-4 (#208)**, and the third is a quarter of the
//     size, which is why it is its own row rather than a bigger multiplier on
//     the one above. The PSC watcher waits on an event and calls
//     `SdEngine::Quiesce`; it reaches no JSON, no save write and no recursion,
//     so `kWatcherStackBytes` is 32 KiB where the engine's threads take 128
//     (`sized_thread.hpp` measures both). Its overhead beside the stack is the
//     same `kThreadHeapOverheadBytes` the other two pay.
//   * **The log keeps its last lines in RAM**, so `GetLog` can answer without
//     going near the card (log.hpp). It is `kTailLines * kMaxLineBytes` -- not
//     at worst but always, since M9-15 (#204) made the ring a fixed one whose
//     slots are reserved at that bound (log.cpp) -- and it is a term here rather
//     than a cost nobody added up. That is the **content**: the per-`std::string`
//     header is allocator overhead and belongs to the newlib term below, which
//     is what that term is for.
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
//   * the transfer buffers are `kDownloadBufferSize` and `kTransferBufferSize`
//     in `http/http_wire.hpp` -- one per in-flight request, because roms stream
//     to file and never sit in RAM whole. **Two requests can be in flight**, one
//     on each thread that makes them: the worker runs ticks, list pages and the
//     download drain one after another, and the pairing thread polls beside it.
//     Only the worker downloads, so the peak is one of each. Until M9-10 (#201)
//     this row was a single 16 KiB buffer, which undercounted the pairing
//     thread's by exactly that;
//   * the buffered response is the largest of `lists::kMaxPlatforms`, the
//     larger of `roms::kDefaultPageSize` and `ipc::kMaxPageSize`,
//     `fs::kMaxDirectoryEntries`, `sync::kMaxPlanOperations`,
//     `sync::kMaxStateListBytes` and `kMaxUndeclaredBodyBytes`, each times its
//     row estimate below;
//   * the rom index is `roms::kMaxIndexRoms` times what one rom holds;
//   * the tick's saves are `scan::kMaxSaves` -- which `state::kMaxRecords` and
//     `sync::kMaxPlanOperations` equal -- times what one save costs in the
//     tick's worst phase;
//   * the list pages are `lists::kMaxPlatforms` rows and two built pages of at
//     most `ipc::kMaxPageSize` items;
//   * the thread stacks are `kThreadStackBytes` (`sized_thread.hpp`), which is
//     where the derivation of that number lives;
//   * the tail is `log::kTailLines` times `log::kMaxLineBytes`, and
//     `ipc::kMaxLogLines` is the first of those rather than a second number;
//   * the play-session buffer is twice `play::kMaxBufferBytes` -- the file's
//     text and then the rows it becomes -- and it is the one term here that is
//     *not* held for the life of the process: it is read at `Load` and rewritten
//     once a tick (`play_sessions.hpp`). It is counted anyway, because a peak is
//     a peak.
//
// **Measured, not only added up (M9-15, #204).** Two measurements stand beside
// the arithmetic, each re-taken on every run that could move it:
//
//   * **The reservation**, off the linked image. `scripts/image-budget.sh` runs
//     in the `switch-build` job and in `ctest -R switch.builds`, reads
//     `aarch64-none-elf-size` and `readelf` over this ELF, and fails the build
//     past its budget. When the budget was set the image mapped 2,080,768 bytes
//     of the 2,359,296 it allows, and `.bss` was 1,459,344 bytes -- this heap and
//     17,552 bytes of everything else. It ties `.bss` to the constant below, so
//     the heap the loader maps is the heap this file declares.
//   * **The use**, off the process. `ctest -R alloc.steady` counts every
//     `operator new` while the sysmodule's own `SdEngine` -- this file's
//     `engine.cpp`, its worker and its scheduler -- ticks against the fixture
//     RomM (tests/test_heap_steady.cpp). Between ticks it holds ~17 KiB, and
//     that floor does not move across ticks: not a block, not a byte. A quiet
//     tick peaks ~170 KiB over it, most of it the rom index being fetched and
//     parsed. With a rom download added to every tick the floor held flat
//     across a hundred rounds when this was written, and the nightly soak
//     (.github/workflows/soak.yml) asks the same of a thousand.
//
// What the second one is **not**: a console number. It is counted on the host's
// standard library, it counts C++ allocations only -- libcurl's `malloc`s are
// the host transport's, and the transport here is a different one -- and the
// fixture library is a handful of roms, so the terms that scale with a library
// (`kHeapListResponse`, `kHeapStateBaseline`) are nowhere near their bounds.
// Those stay arithmetic. What it does settle is the two things arithmetic
// cannot: that nothing this table omits is large, and that nothing grows with
// uptime -- which it did, until the log's ring stopped being a `std::deque`.

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

/// What `GET /api/heartbeat` weighs on the wire (M10-2, #228). Measured against
/// the fixture: 1,166 bytes on RomM 5.2.0, about 1.6 KiB on 5.4.0, which added
/// sections. It is the server's whole configuration and grows with its platform
/// folders, so the estimate is ten times the measurement. Like the two above it
/// is not a bound anything enforces; the parse it feeds is
/// (`server::kMaxSystemBytes`).
constexpr size_t kHeartbeatJsonBytes = 16 * 1024;

/// What one negotiate operation weighs on the wire (M9-20, #246): 300 bytes in
/// RomM 5.4.0's own answer, compacted (server/contract/captures/5.4.0/
/// sync-negotiate-download.json), most of it the server's tagged file name, its
/// `reason` sentence and two timestamps. A No-Intro name in place of the
/// capture's `probe` is ~30 bytes longer; 512 is that and room. Not a bound;
/// `sync::kMaxPlanOperations` bounds the count.
constexpr size_t kOperationJsonBytes = 512;

/// What one `StateSchema` row weighs (M9-20): 588 bytes in the 5.4.0 capture
/// (`states-list.json`), rounded up for longer names. Used only for the one row
/// of the listing in flight; the listing's text is `sync::kMaxStateListBytes`,
/// which *is* a bound.
constexpr size_t kStateJsonBytes = 768;

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
/// Two, which over-counts today by one. `card.cpp` reads a directory whole and
/// closes it before anything recurses, and every listing is the worker's
/// (`kMaxListingsHeld` names them) -- the IPC thread's `ListDirectory` this used
/// to cite was never written, M9-19 (#217) found. The second is 4 KiB kept for
/// the day another thread lists, rather than a row re-derived on that day.
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

/// What a parsed row costs while it is read, as a multiple of its text (M9-19,
/// #217).
///
/// A `json::Value` is 104 bytes and a `json::Member` 136 on aarch64's libstdc++,
/// and a RomM row is ~100 of them. Measured with the allocation counter
/// (`tests/count_new.hpp`) over core/ built by devkitA64's own aarch64 g++ and
/// libstdc++, against the fixture RomM 5.2.0: the seven-row index page above
/// held as one document is 163,295 bytes in 213 blocks -- ~11x its text, a
/// platform's ~11.4x -- and read a row at a time (`json::ParseEach`) its peak is
/// 30,111, ~14x the widest row's 2,109 bytes, because a row's member vector is
/// mid-reallocation at the top. 16 rounds that up and pays newlib's ~16 bytes a
/// block, so one row in flight is its text times this.
constexpr size_t kJsonDocumentPerTextByte = 16;

/// What one short `std::string` costs on the heap: a name past libstdc++'s
/// 15-byte inline buffer, in one newlib chunk.
///
/// 64 is a name of up to 55 bytes (the chunk is the request plus an 8-byte
/// header, rounded to 16). The fixture's longest, `Synthetic Two Disc Game`, is
/// a 32-byte chunk; No-Intro names with their region tags run 30 to 50 bytes,
/// and a longer one is the estimate being wrong in a direction this margin
/// absorbs. Platform slugs (`gba`, `psx`) stay inside the buffer and cost
/// nothing here.
constexpr size_t kHeapNameBytes = 64;

/// What one rom in the index holds for the length of a tick: the `Rom`, its two
/// lookup offsets (`RomIndex::BuildLookup`), and its two match keys' strings.
/// `sizeof(Rom)` is 112 on aarch64 -- an `int64`, three `std::string`s and a
/// flag -- and pinned below, so a field added to it re-derives this row.
constexpr size_t kRomIndexEntryBytes =
    sizeof(rommsync::roms::Rom) + 2 * sizeof(std::uint32_t) + 2 * kHeapNameBytes;

/// What one directory entry costs at the bound: the `fs::Entry` (56 bytes on
/// aarch64, pinned below) half again for the last time the vector doubled into
/// it -- both buffers are live while it moves, and `kMaxDirectoryEntries` is a
/// power of two so that is the last -- and its name's string.
constexpr size_t kDirectoryEntryBytes = sizeof(rommsync::fs::Entry) * 3 / 2 + kHeapNameBytes;

/// How many listings the worker can hold at once: **two**, because
/// `fs::Directories` reads the next folder before the listing it held for the
/// last one is freed (`file_system.cpp`). Every listing is the worker's -- the
/// save scan, the staging sweep, and `fs::Directories` in the finish and in the
/// state sync -- and none of them holds a listing while another reads one.
constexpr size_t kMaxListingsHeld = 2;

/// Rows a rom page holds: the index's and the overlay's are the same schema and
/// the larger of the two is the one the heap pays for.
constexpr size_t kRomPageRows =
    std::max<size_t>(rommsync::roms::kDefaultPageSize, rommsync::ipc::kMaxPageSize);

// What a tick holds per save, **measured** rather than added up from struct
// sizes (M9-20, #246): the allocation counter (`tests/count_new.hpp`) over core/
// built by the devkitpro image's native aarch64 g++ and libstdc++, a card of
// `scan::kMaxSaves` saves scanned through `scan::ScanSaves` -- No-Intro-length
// names, a 63-byte SD path, `scan::kMaxRetroArchFolders` core folders -- and
// then each later structure built the way the tick builds it. Re-measured
// at 128 saves once the rom scope (#241) was in.
// Each figure is the bytes held per save plus newlib's ~16 a block, rounded up;
// the struct sizes they rest on are pinned below, so a field added to any of
// them is a red build here rather than a row that quietly undercounts.

/// One `scan::SaveFile` in `ScanResult::saves`: 505 bytes and 4.6 blocks a save,
/// 579 with the blocks -- seven strings, three of them past the inline buffer,
/// and the scan's list of folders read spread over the saves it found (at 512
/// saves the same was 529).
constexpr size_t kScannedSaveBytes = 592;

/// What the engine builds out of it for the tick (`engine.cpp`): one
/// `sync::ClientSaveState` and one `sync::SaveTarget`, 481 bytes and 4.0 blocks,
/// 545 with them.
constexpr size_t kReportedSaveBytes = 560;

/// One `sync::SyncOperation` of the plan, read off the 5.4.0 capture's
/// operation: 444 bytes and 5.0 blocks, 524 with them.
constexpr size_t kPlannedSaveBytes = 544;

/// One `sync::OperationResult` of what the plan did, with the message and the
/// two paths a download over an existing save carries: 545 bytes and 4.0 blocks,
/// 609 with them -- 200 bytes of struct since #240 gave it a conflict's
/// archival upload to report.
constexpr size_t kExecutedSaveBytes = 624;

/// A `sync::ClientSaveState` copied into a chunk's request when the scope is
/// several chunks: half of `kReportedSaveBytes`'s measurement (the
/// `SaveTarget` is not copied), rounded up.
constexpr size_t kOfferedCopyBytes = 288;

/// The scan's plan of folders while it walks (`PlanFolders`, #230): measured as
/// the scan's peak with `scan::kMaxRetroArchFolders` RetroArch core folders
/// against the same scan with none -- 28,194 bytes -- and rounded up.
constexpr size_t kFolderPlanBytes = 32 * 1024;

/// The tick's four phases, each what it holds of every save at once, at
/// `scan::kMaxSaves`. The two vectors that are not reserved -- the scan's, and
/// the plan's while `json::ParseEach` hands it an operation at a time -- hold
/// both buffers for the copy when they last double, which is the half again.
///
///   scan    the records being found, and the plan of folders being walked;
///   build   the records, what is reported and targeted, and the two lists of
///           observations the play-session derivation reads (`ObservationsOf`);
///   plan    the records freed (`engine.cpp`), the plan arriving beside what
///           was reported -- and, when the scope is several chunks, the copy of
///           a chunk's share of it that is sent (`sync::RunTick`);
///   run     the plan and what each operation did, to the end of its run.
///
/// **One plan at a time, however many chunks a tick negotiates** (#246): each
/// chunk's plan is executed, finished and freed before the next is asked, and
/// one larger than `sync::kMaxPlanOperations` is split until it fits or is one
/// rom (`sync::TickOptions::rom_scope`). That is what makes `kMaxSaves`
/// operations the plan's bound here rather than the library's.
constexpr size_t kTickScanBytes =
    rommsync::scan::kMaxSaves * (kScannedSaveBytes + sizeof(rommsync::scan::SaveFile) / 2) +
    kFolderPlanBytes;
constexpr size_t kTickBuildBytes =
    rommsync::scan::kMaxSaves * (kScannedSaveBytes + kReportedSaveBytes +
                                 2 * sizeof(rommsync::play::SaveObservation));
constexpr size_t kTickPlanBytes =
    rommsync::scan::kMaxSaves *
    (kReportedSaveBytes + kPlannedSaveBytes + sizeof(rommsync::sync::SyncOperation) / 2 +
     kOfferedCopyBytes);
constexpr size_t kTickRunBytes =
    rommsync::scan::kMaxSaves * (kReportedSaveBytes + kPlannedSaveBytes + kExecutedSaveBytes);

/// What one row of a rom page becomes while the worker builds the overlay's page
/// out of it (`lists::RomPage`): 446 bytes and 2 blocks a row with every string
/// at `lists::kMaxRowTextBytes`, measured the same way.
constexpr size_t kListRomRowBytes = 512;

/// What one platform costs in the list service's snapshot: 161 bytes a row at
/// 60-byte names, measured as above over 256 rows, and rounded up. RomM decides
/// the name's width; `lists::kMaxPlatforms` bounds the count.
constexpr size_t kPlatformRowBytes = 168;

/// What one built page costs: a `ListPage` of `ipc::kMaxPageSize` items, each a
/// vector of tagged fields (`ipc::ListField` is 88 bytes on aarch64). The
/// largest measured was a platform page, 63 items at 60-byte names in 51,763
/// bytes with its blocks; a rom page, whose rows are wider and so fewer under
/// `ipc::kMaxPayloadBytes`, peaked at 40,186. 52 KiB covers both.
constexpr size_t kListPageBytes = 52 * 1024;

/// The table's rows are whole KiB, so a term computed from widths is rounded up
/// to one rather than written as a literal that would drift from them.
constexpr size_t RoundUpToKiB(size_t bytes) { return (bytes + 0x3FF) & ~size_t{0x3FF}; }

// The terms of the table above, as constants the compiler adds up. Each is
// pinned to its row by a `static_assert` below, so a bound that moves is a red
// build here rather than a table nobody re-totalled.
constexpr size_t kHeapSocketMemory = rommsync::sysmodule::ExpectedBsdTransferMemory({});
constexpr size_t kHeapStateBaseline = 2 * rommsync::state::kMaxStateBytes;
constexpr size_t kHeapTransferBuffer =
    rommsync::sysmodule::kDownloadBufferSize + rommsync::sysmodule::kTransferBufferSize;
// The largest thing the worker holds one at a time, not the first one anybody
// wrote down: until M9-2 (#207) this term named only the platforms list, and
// until M9-19 (#217) it counted each response's text and not the document it was
// parsed into, which was ten times larger. Each response is its text, once
// (`kMaxReservedBodyBytes`), and one row of it in flight (`json::ParseEach`).
constexpr size_t kHeapPlatformsResponse =
    rommsync::lists::kMaxPlatforms * kPlatformJsonBytes +
    kPlatformJsonBytes * kJsonDocumentPerTextByte;
constexpr size_t kHeapRomPageResponse =
    kRomPageRows * kRomJsonBytes + kRomJsonBytes * kJsonDocumentPerTextByte;
constexpr size_t kHeapDirectoryListing =
    kMaxListingsHeld * rommsync::fs::kMaxDirectoryEntries * kDirectoryEntryBytes;
// The heartbeat a tick reads first (M10-2, #228): its body, and the one member
// of it that is parsed -- the walk past the rest builds nothing. Read on the
// worker at the top of a tick and freed before the rom index is fetched, so it
// is one more thing held one at a time.
constexpr size_t kHeapHeartbeat =
    kHeartbeatJsonBytes + rommsync::server::kMaxSystemBytes * kJsonDocumentPerTextByte;
// The three M9-20 (#246) gave a bound: the plan's text at its operation bound
// and one operation in flight -- its rows are `kHeapTickSaves`'s -- the state
// listing's text at its byte bound and one row, and a body that declared no
// length at its cap, half again while its last step copies.
constexpr size_t kHeapNegotiateResponse =
    rommsync::sync::kMaxPlanOperations * kOperationJsonBytes +
    kOperationJsonBytes * kJsonDocumentPerTextByte;
constexpr size_t kHeapStateListResponse =
    rommsync::sync::kMaxStateListBytes + kStateJsonBytes * kJsonDocumentPerTextByte;
constexpr size_t kHeapUndeclaredBody = rommsync::sysmodule::kMaxUndeclaredBodyBytes * 3 / 2;
constexpr size_t kHeapListResponse = RoundUpToKiB(std::max(
    {kHeapPlatformsResponse, kHeapRomPageResponse, kHeapDirectoryListing, kHeapHeartbeat,
     kHeapNegotiateResponse, kHeapStateListResponse, kHeapUndeclaredBody}));
// Beside whichever of those the tick holds: the index, and the page whose rows
// are on their way into it, before it is freed.
constexpr size_t kHeapRomIndex =
    RoundUpToKiB(rommsync::roms::kMaxIndexRoms * kRomIndexEntryBytes +
                 rommsync::roms::kDefaultPageSize * sizeof(rommsync::roms::Rom) * 3 / 2);
// Beside those too, for the length of a tick: every save, the largest of the
// four ways the tick holds it. The worker builds a list page's rows only when
// it is not ticking (`RunWorker`, the rule `kHeapListResponse` rests on), so a
// page's rows are held here, never beside a tick's.
constexpr size_t kHeapTickSaves = RoundUpToKiB(
    std::max({kTickScanBytes, kTickBuildBytes, kTickPlanBytes, kTickRunBytes}));
// Held between pumps, beside everything -- a tick included: the one platform
// snapshot, the one built page waiting for its `ListNext`, and the page the IPC
// thread is handing over, built on that thread out of the snapshot.
constexpr size_t kHeapListPages =
    RoundUpToKiB(rommsync::lists::kMaxPlatforms * kPlatformRowBytes + 2 * kListPageBytes);
constexpr size_t kHeapThreadStacks =
    2 * (rommsync::sysmodule::kThreadStackBytes + rommsync::sysmodule::kThreadHeapOverheadBytes);
constexpr size_t kHeapWatcherStack =
    rommsync::sysmodule::kWatcherStackBytes + rommsync::sysmodule::kThreadHeapOverheadBytes;
constexpr size_t kHeapLogTail = rommsync::log::kTailLines * rommsync::log::kMaxLineBytes;
constexpr size_t kHeapPlaySessions = 2 * rommsync::play::kMaxBufferBytes;
constexpr size_t kHeapOpenDirectories = kMaxOpenDirectories * kOpenDirectoryBytes;
constexpr size_t kHeapNewlibOverhead = 0x8000;

constexpr size_t kHeapPeak = kHeapSocketMemory + kHeapStateBaseline + kHeapTransferBuffer +
                             kHeapListResponse + kHeapRomIndex + kHeapTickSaves +
                             kHeapListPages + kHeapThreadStacks + kHeapWatcherStack +
                             kHeapLogTail + kHeapPlaySessions + kHeapOpenDirectories +
                             kHeapNewlibOverhead;

// The arithmetic above, checked by the compiler rather than by a reader. It is
// the transfer memory that this is really about: the trimmed socket config is
// the difference between a working engine and one that dies at
// `socketInitialize`, and the number is easy to change by accident and
// impossible to notice off a console. The rest are here because M9-2 (#207)
// found every one of them able to move without anything noticing.
static_assert(kHeapSocketMemory == 0x1D000,
              "the bsd transfer memory is not the trimmed 116 KiB M0-1 measured");
static_assert(kHeapStateBaseline == 0x10000, "state::kMaxStateBytes moved; retotal the table");
static_assert(kHeapTransferBuffer == 0x14000,
              "kDownloadBufferSize or kTransferBufferSize moved; retotal the table");
// A download's read chunk is the receive window, and no more: a read returns only
// what the socket has buffered, so a chunk past `tcp_rx_buf_max_size` is heap
// with nothing to fill it (`http_wire.hpp`, M9-10, #201). Raising the window is
// what would make a bigger chunk worth its bytes, and it costs transfer memory
// out of this same heap -- `kHeapSocketMemory`, pinned above.
static_assert(rommsync::sysmodule::kDownloadBufferSize <=
                  rommsync::sysmodule::SocketBudget{}.tcp_rx_buf_max_size,
              "a download chunk larger than the receive window buys nothing");
// The struct sizes the two per-entry costs above are built from, measured on
// this target rather than assumed off the host's standard library.
static_assert(sizeof(rommsync::roms::Rom) == 112, "roms::Rom changed size; retotal the table");
static_assert(sizeof(rommsync::fs::Entry) == 56, "fs::Entry changed size; retotal the table");
// The structs a tick's saves were measured in (M9-20, #246). A field added to
// one of these changes what a save costs, and the measurement beside its
// constant above is then of a struct that no longer exists.
static_assert(sizeof(rommsync::scan::SaveFile) == 248,
              "scan::SaveFile changed size; re-measure kScannedSaveBytes");
static_assert(sizeof(rommsync::sync::ClientSaveState) == 176 &&
                  sizeof(rommsync::sync::SaveTarget) == 112,
              "ClientSaveState or SaveTarget changed size; re-measure kReportedSaveBytes");
static_assert(sizeof(rommsync::sync::SyncOperation) == 296,
              "sync::SyncOperation changed size; re-measure kPlannedSaveBytes");
static_assert(sizeof(rommsync::sync::OperationResult) == 200,
              "sync::OperationResult changed size; re-measure kExecutedSaveBytes");
static_assert(sizeof(rommsync::play::SaveObservation) == 48,
              "play::SaveObservation changed size; retotal the table");
static_assert(sizeof(rommsync::lists::PlatformRow) == 80 &&
                  sizeof(rommsync::ipc::ListField) == 88,
              "a list row changed size; re-measure kPlatformRowBytes and kListPageBytes");
// The responses that compete for `kHeapListResponse`, each pinned so the one that
// wins can be seen to, and the two decisions M9-19 (#217) made against it: a rom
// page and a listing at their bounds both fit under the platforms list the heap
// already pays for, so neither costs a byte of its own. Past that, every row of
// a page and every entry of a listing is heap.
static_assert(kHeapPlatformsResponse == 217600, "lists::kMaxPlatforms moved; retotal the table");
static_assert(kHeapRomPageResponse == 204800,
              "roms::kDefaultPageSize or ipc::kMaxPageSize moved; retotal the table");
static_assert(kHeapDirectoryListing == 151552,
              "fs::kMaxDirectoryEntries moved; retotal the table");
static_assert(kHeapRomPageResponse <= kHeapPlatformsResponse,
              "a rom page is now the largest response; it was chosen not to be (M9-19)");
static_assert(kHeapDirectoryListing <= kHeapPlatformsResponse,
              "a listing is now the largest buffer; it was chosen not to be (M9-19)");
static_assert(kHeapHeartbeat <= kHeapPlatformsResponse,
              "the heartbeat is now the largest response the worker holds (#228)");
// M9-20's three (#246), and the decision each was made against: none of them
// costs a byte of its own. The undeclared body is the one that came closest,
// and why its cap is 128 KiB and not the 256 KiB a declared body is reserved to.
static_assert(kHeapNegotiateResponse == 73728,
              "sync::kMaxPlanOperations moved; retotal the table");
static_assert(kHeapStateListResponse == 143360,
              "sync::kMaxStateListBytes moved; retotal the table");
static_assert(kHeapUndeclaredBody == 196608,
              "kMaxUndeclaredBodyBytes moved; retotal the table");
static_assert(kHeapNegotiateResponse <= kHeapPlatformsResponse &&
                  kHeapStateListResponse <= kHeapPlatformsResponse,
              "the plan or the state listing is now the largest response (M9-20)");
static_assert(kHeapUndeclaredBody <= kHeapPlatformsResponse,
              "a body with no Content-Length now costs more than any response the row "
              "counts; its cap was chosen so it would not (M9-20)");
static_assert(rommsync::sync::kMaxPlanOperations * kOperationJsonBytes <=
                      rommsync::sysmodule::kMaxReservedBodyBytes &&
                  rommsync::sync::kMaxStateListBytes <= rommsync::sysmodule::kMaxReservedBodyBytes,
              "the plan or the state listing is larger than the body the wire reserves");
static_assert(kHeapListResponse == 0x35400,
              "a response or listing bound moved; retotal the table");
// The reservation that makes "its text, once" true has to reach every response
// the row budgets, or the largest of them doubles its way in after all.
static_assert(rommsync::lists::kMaxPlatforms * kPlatformJsonBytes <=
                      rommsync::sysmodule::kMaxReservedBodyBytes &&
                  kRomPageRows * kRomJsonBytes <= rommsync::sysmodule::kMaxReservedBodyBytes,
              "a budgeted response is larger than the body the wire reserves");
static_assert(kHeapRomIndex == 0x40C00, "roms::kMaxIndexRoms moved; retotal the table");
// The tick's row is its last phase, and the others are pinned under it so the
// one that wins can be seen to. A save is one plan operation and one baseline
// row, so the three bounds are one number; `state_db.hpp` asserts the second.
static_assert(rommsync::scan::kMaxSaves == rommsync::sync::kMaxPlanOperations,
              "a plan may name more saves than one tick was budgeted for");
static_assert(kTickScanBytes <= kTickRunBytes && kTickBuildBytes <= kTickRunBytes &&
                  kTickPlanBytes <= kTickRunBytes,
              "a tick's worst phase is no longer the one this row was derived from (M9-20)");
static_assert(kRomPageRows * kListRomRowBytes <= kHeapTickSaves,
              "a list page's rows no longer fit where a tick's saves are held (M9-20)");
static_assert(kHeapTickSaves == 0x36000, "scan::kMaxSaves moved; retotal the table");
static_assert(kHeapListPages == 0x24800,
              "lists::kMaxPlatforms or ipc::kMaxPageSize moved; retotal the table");
static_assert(kHeapThreadStacks == 0x44000, "kThreadStackBytes moved; retotal the table");
static_assert(kHeapWatcherStack == 0xA000, "kWatcherStackBytes moved; retotal the table");
static_assert(kHeapLogTail == 0x1800, "log::kTailLines moved; retotal the table");
static_assert(kHeapPlaySessions == 0x8000, "play::kMaxBufferBytes moved; retotal the table");
static_assert(kHeapOpenDirectories == 0x2000,
              "the direntry cache or the open-directory bound moved; retotal the table");
// The one term with no bound behind it, so this pin is what binds the constant
// to its row rather than a second reader of a bound. Changing the term is a red
// build here, which is the point: it is the term a reader is likeliest to nudge.
static_assert(kHeapNewlibOverhead == 0x8000, "the newlib arena term moved; retotal the table");
static_assert(kHeapPeak == 0x173000, "the table above no longer sums to its peak row");

// 0x180000 leaves 0xD000 -- 52 KiB -- over that peak, which is the margin a
// process nobody can attach a debugger to needs. It is the only margin stated
// here on purpose: #207's prose quoted two, 94 KiB and 78 KiB, and the
// arithmetic gave neither. M9-4 (#208) took 40 KiB of it for the PSC watcher's
// thread, which is the row above.
//
// **It grew by 0x20000 in M9-20 (#246)**, from 0x160000, when the two rows it
// added -- a tick's saves and the list service's pages, 360 KiB between them --
// could not be paid for by the baseline row shrinking with `kMaxRecords` (192
// KiB back) and a margin that was already holding both. 128 KiB of resident
// `.bss`, all of it cost that was always there and never counted, and the
// margin gave up 42 KiB beside it: the image then maps 2,322,432 of the
// 2,359,296 bytes `scripts/image-budget.sh` allows, so the next growth here is
// a decision about that ceiling first.
//
// **It grew by 0x10000 in M9-10 (#201)**, from 0x150000, when the transfer row
// went from 16 KiB to 80: 48 KiB of it is the download's bigger read chunk and
// 16 KiB is the pairing thread's buffer, which was always allocated and never
// counted. The margin could not absorb that and stay a margin. It is 64 KiB of
// resident `.bss` bought for, at best, a quarter of the `ssl` round trips per
// rom -- "at best" being #44's to measure (`http/http_wire.hpp`).
//
// **It grew by 0x90000 in M9-2**, from 0xC0000: 576 KiB more `.bss` in a
// resident image that was ~2.00 MiB, against an Atmosphere third-party sysmodule
// budget that is 7 MB on HOS 21.0.0+ and shared with everything else the user
// installed (M9-9, #200). None of it is new cost -- the thread stacks were
// always allocated and the rom index page was always fetched -- but all of it is
// newly *declared*, and that is the point: an undeclared allocation on a
// `-fno-exceptions` build is `std::terminate` with no crash report.
//
// **Two thirds of that growth was one term**, `kHeapListResponse`, and M9-19
// (#217) took most of it back -- 500 KiB to 213 -- to pay for a row the table had
// never had: the rom index. M9-9's two unwind flags take ~640 KiB off the same
// image, which is more than the whole of this.
//
// **The three bounds M9-19 derived from this heap**, rather than chose beside
// it, each against what is left once everything else is paid. The nine rows that
// are not M9-19's come to 842 KiB of the 1,408, and the responses' row is fixed
// at 213 KiB by the platforms list, which is none of the three:
//   * `roms::kDefaultPageSize` is 64, the overlay's own page, because at 64 a
//     page (200 KiB) sits under the platforms list and costs nothing of its own.
//     At 200 it was 500 KiB of text and ~4.6 MiB of document, for one request a
//     tick where 64 takes three.
//   * `fs::kMaxDirectoryEntries` is 512, because two listings at 512 (148 KiB)
//     sit under it too. At 4,096 two were ~1.2 MiB, and `List` doubled past the
//     bound before it popped back. It was `state::kMaxRecords` then; M9-20 moved
//     that and left this, since a folder is read to find saves among other files.
//   * `roms::kMaxIndexRoms` is 1,024 because that was what was left of the
//     1,408 KiB heap M9-19 had (M9-20's figures are below): 1,408 - 842 - 213
//     was 353 KiB, of which 1,024 roms and the page on its way in take 259 and
//     the margin was the rest. Each further 256 roms is ~62 KiB of resident
//     `.bss` -- the heap growing with it, against an image ceiling
//     (`scripts/image-budget.sh`) with 36 KiB to spare since M9-20 -- and that
//     trade is this table's to make again rather than a constant's to drift into.
//
// **The bound M9-20 (#246) derived**, the same way: `state::kMaxRecords` -- and
// with it `scan::kMaxSaves`, `scan::kMaxStates` and `sync::kMaxPlanOperations`
// -- is 128, from 512, with `state::kMaxStateBytes` 32 KiB from 128. At 512 a
// tick's saves alone were ~856 KiB, and with the baseline at 256 the two were
// 1.1 MiB of what was a 1.4 MiB heap; nothing that keeps the image under its
// ceiling pays for that. At 128 the tick is 216 KiB and the baseline 64. Each
// further 64 saves is ~108 KiB of tick and 32 KiB of baseline -- ~140 KiB of
// resident `.bss` per 64 saves, against the same ceiling as the roms above -- and how many saves
// a console may hold is that trade, made here, rather than a number the heap
// was never asked about. (There is no console in the field to migrate: a larger
// `state.db` is refused and rebuilt, `state_db.hpp`.)
//
// **Still not in this table**, found while M9-20 measured the rest and left for
// an issue of its own (#247): the states half of a tick (`sync::SyncStates`) --
// the state scan at `scan::kMaxStates`, the listing's rows at
// `sync::kMaxStateListBytes`, and what each state operation did, all beside the
// saves' rows above; the baseline held twice at the finish (`sync::RunTick`
// copies what it is handed, and the engine still holds the original) and again
// for the states half (`LoadBaselineFor` beside `loaded`); and the download
// queue, held whole for the life of the process (`kMaxQueueEntries` entries, up
// to ~1.9 KiB each at their path bounds) and serialised whole -- up to
// `kMaxQueueBytes` of text -- on every write, from the IPC thread as well as the
// worker. And three smaller ones: the negotiate request's body (a save's entry,
// `kMaxSaves` of them), a device list whose rows nothing counts, and a body that
// declares a `Content-Length` past `kMaxReservedBodyBytes`, which still grows
// the way an undeclared one used to. They live in the margin today, which is
// what a margin is not for. The responses that are one row -- a save row, an
// upload's answer, a rom's detail, the play-session answer -- are one row's
// document beside their text, under `kHeapListResponse` with room.
constexpr size_t kInnerHeapSize = 0x180000;
constexpr size_t kHeapMargin = kInnerHeapSize - kHeapPeak;

static_assert(kHeapPeak < kInnerHeapSize, "the heap no longer covers the peak in the table above");
static_assert(kHeapMargin == 0xD000,
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

/// What libnx can say about the SD card's format, which is less than it sounds.
///
/// `fsIsExFatSupported` answers whether the *console* has the exFAT driver, not
/// what the card is. A console without one can only be reading FAT32, so a "no"
/// is certain; a "yes" -- nearly every console -- leaves the card unknown, and
/// `card.cpp` holds an unknown card to FAT32's 4 GiB ceiling (#202). A call that
/// fails is the same unknown.
rommsync::fs::Format CardFormat() {
  bool exfat = true;
  if (R_SUCCEEDED(fsIsExFatSupported(&exfat)) && !exfat) {
    return rommsync::fs::Format::kFat32;
  }
  return rommsync::fs::Format::kUnknown;
}

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

/// The same, for `pmdmntInitialize()` (M10-16, #242). It also decides whether
/// the engine gets a foreground probe at all: one on a session that was never
/// opened would answer "no game" on every call, which is the old behaviour
/// with nothing saying so.
bool g_pm_up = false;

/// Whether an application is in the foreground (M10-16, #242): a retail game,
/// Tico, or RetroArch launched over a title -- anything pm launched as *the*
/// application. The engine holds every tick while this says yes.
///
/// `pm` answers `ResultProcessNotFound` when there is none, and anything that
/// is not a pid is read as none: a probe that failed closed would be a console
/// that never syncs again over one bad answer, where failing open is the
/// client as it was before #242. A library applet -- RetroArch started from the
/// album -- is not the application, and is not seen; #242 records that.
///
/// A poll and not a hook, deliberately: `pmdmntHookToCreateApplicationProcess`
/// is the one hook `dmnt:cht` holds, and pm does not start a hooked application
/// until the hooker says so (`boot.foreground` refuses a hook here).
bool ApplicationRunning() {
  u64 pid = 0;
  return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid));
}

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
// libnx opens for homebrew. The count is `handle_budget.hpp`'s, because each
// session is a row in the handle table the npdm declares (#210).
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = rommsync::sysmodule::handles::kFsSessions;

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
  // Not fatal, for the reason a card that refuses `CreateDirectory` is not
  // (below): the overlay and the in-memory log still work without one. Noted
  // rather than discarded, so the first line a user is asked for says why every
  // read after it answers "missing" (#202).
  rc = fsdevMountSdmc();
  if (R_FAILED(rc)) {
    rommsync::sysmodule::boot::Note(g_boot, "rommsync: fsdevMountSdmc", rc);
  }

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

  // **`pm:dmnt`, so the engine can tell a game is running** (M10-16, #242). A
  // tick under a running game is the one that loses a save: the game writes its
  // in-memory copy over a download when it closes. One session, polled only
  // while a tick is held (`kForegroundPollInterval`).
  //
  // Not fatal, for `psc:m`'s reason: without it the client syncs as it did
  // before #242, which is worse than with it and far better than an overlay
  // that cannot reach a sysmodule that refused to start. `pm` is one of the
  // processes the kernel itself launches, so this does not wait in practice.
  if (WaitForService("pm:dmnt")) {
    rc = pmdmntInitialize();
    if (R_FAILED(rc)) {
      rommsync::sysmodule::boot::Note(g_boot, "rommsync: pmdmntInitialize", rc);
    }
    g_pm_up = R_SUCCEEDED(rc);
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
  if (g_pm_up) pmdmntExit();
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
  // **A card that refuses is not fatal**: the log's in-memory tail still
  // answers `GetLog`, and a client that stopped syncing over a log file it
  // could not write would have the tail wagging the dog. It *is* reported since
  // M9-13 (#202), once the log exists -- to the tail, which is the one place
  // left that can hold it. `fsdevMountSdmc` has already run, in `__appInit`.
  //
  // **What this costs at boot**, stated rather than left to be measured: one
  // `mkdir` and one append of a few dozen bytes. It is SD I/O on the boot path
  // and it is bounded -- `engine.Load()` a few lines below already reads five
  // files off the same card, and the rule that matters is that nothing here
  // touches the *network* (CLAUDE.md, "Never block boot"), which is still true.
  // Nothing in this block waits on anything.
  g_card = rommsync::sysmodule::MakeSdCard(CardFormat());
  const rommsync::fs::MakeDirResult config_dir =
      g_card->CreateDirectory(rommsync::sysmodule::kConfigSdDir);
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
  if (!config_dir.ok()) {
    Log(rommsync::log::Level::kWarn, "rommsync: " + config_dir.message);
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
  // Before the worker, so the boot tick is already held if the console came up
  // straight into a game (#242).
  if (g_pm_up) {
    engine.UseForegroundProbe(&ApplicationRunning);
  } else {
    Log(rommsync::log::Level::kWarn,
        "rommsync: no pm:dmnt; a sync can run while a game is open (#242)");
  }
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
  //
  // **Gated on `g_psc_up`**, which is this file's pattern rather than a
  // precaution about this call: `timeExit` is gated the same way and
  // `NetworkInitialize` runs only behind the four `WaitForService`s that
  // precede it. Without the gate `pscmGetPmModule` would go out on a session
  // `pscmInitialize` never opened -- which `Open()` handles, and which is
  // exactly the kind of "handled" a reader has to go and check.
  std::unique_ptr<rommsync::sysmodule::power::Subscription> psc;
  if (g_psc_up) {
    psc = rommsync::sysmodule::power::Subscribe(engine);
  }
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

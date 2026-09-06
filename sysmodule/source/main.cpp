// sys-rommsync entry point.
//
// It initialises the services the engine will need, registers the `rommsync`
// IPC service, and spends the rest of the process's life answering it (M4-1,
// #23). Since M1-7 (#126) that includes a **transport**: `sys-rommsync` holds an
// `http::HttpClient` of its own -- the Horizon `ssl` one under `http/` -- so the
// engine that was proven against a real RomM on a laptop can reach a server from
// here too. M1-6 (#123) installs it in the pairing seam below, so `StartPair` is
// answered here rather than refused. There is still no scheduler (M7-2, #37), so
// the commands behind that answer `ipc::Error::kUnavailable`; what is real today
// is the console's configuration -- read *and* written, since M5-3 (#30) --
// whether it has ever paired, the pairing itself, and the build
// (see `engine.hpp`).
//
// The service is registered inside `__appInit`, while `sm` is still up, because
// a registered port outlives the session that registered it: a resident process
// should not hold an `sm` handle for the life of the console just to keep its
// own name (`ipc/server.hpp`).
//
// What this also proves, every CI run, is that core/ still builds for aarch64:
// every translation unit under core/src is compiled into this target
// (../switch.mk), so one that quietly became host-only breaks the build here
// rather than months later.
//
// None of it has run. It is exercised in Ryujinx as a manually-launched build
// before the M8-1 gate, never on hardware (sysmodule/AGENTS.md).

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "engine.hpp"
#include "http/http_wire.hpp"
#include "http/ssl_http_client.hpp"
#include "ipc/server.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/core.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/state_db.hpp"

namespace {

// The whole heap this process ever has. Derived, since M1-7 (#126), rather than
// inherited from devkitPro's template -- the transport is what made the old
// 0x80000 a number nobody had added up:
//
//   | term                                     | bytes   |         |
//   |------------------------------------------|---------|---------|
//   | bsd transfer memory, trimmed config      | 0x1D000 | 116 KiB |
//   | `state.db` baseline at its bound         | 0x3C000 | 240 KiB |
//   | one in-flight transfer buffer            | 0x4000  |  16 KiB |
//   | the largest buffered list response        | 0x32000 | 200 KiB |
//   | two worker thread stacks (M1-6, M7-2)    | 0x10000 |  64 KiB |
//   | newlib arena overhead and fragmentation  | 0x8000  |  32 KiB |
//   | **peak**                                 | 0xA7000 | 668 KiB |
//
// The old 0x80000 does not cover that, and the two terms it is short by are the
// two that are easiest to miss:
//
//   * **A list response is buffered whole**, because `Send` returns a
//     `std::string` (`http.hpp`) and nothing in this client caps it. The two
//     that can be large are measured against the fixture RomM rather than
//     guessed: `/api/platforms` is an unpaged bare array in 5.2.0 at ~800 bytes
//     a row, so `lists::kMaxPlatforms` of them is ~200 KiB, and
//     `/api/roms?limit=64` at `ipc::kMaxPageSize` is ~2.1 KiB a row, or
//     ~136 KiB. The larger of the two is the term. **It is a bound on what the
//     server sends, not one this client enforces** -- the row counts are
//     bounded (`list_service.hpp`), the row widths are RomM's -- which is why
//     the number is written down here with where it came from.
//   * **Two threads, not one.** M1-6 (#123) starts a pairing thread and M7-2
//     (#37) starts the worker that drives `PumpLists` and the sync tick; each
//     costs its stack out of this heap.
//
// 0xC0000 leaves 108 KiB over that peak, which is the margin a process nobody
// can attach a debugger to needs: a `bad_alloc` here is `std::terminate`, since
// the sysmodule builds `-fno-exceptions` (`switch.mk`).
//
// Every term is a bound something else enforces, and each of them moves with
// this constant rather than independently:
//   * the transfer memory is `SocketBudget` in `http/ssl_http_client.hpp`, and
//     `socketInitializeDefault()` -- 2.25 MiB -- is the one call this process
//     must never make (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision);
//   * the baseline is `state::kMaxStateBytes` + `state::kMaxRecords`, the one
//     term that grows with the library rather than being a fixed buffer;
//   * the transfer buffer is `kTransferBufferSize` in `http/http_wire.hpp` --
//     one per in-flight request, because roms stream to file and never sit in
//     RAM whole;
//   * the list response is `lists::kMaxPlatforms` times the row estimate below.
constexpr size_t kInnerHeapSize = 0xC0000;

// What one platform's JSON weighs on the wire. Measured against the fixture
// RomM 5.2.0 (`GET /api/platforms`: 3184 bytes for four rows) and rounded up.
// Not a bound anything enforces -- `kMaxPlatforms` bounds the count and RomM
// decides the width -- which is exactly why it is written down here with its
// provenance rather than folded into a total: a reader can disagree with this
// number, and cannot disagree with a constant that has no origin.
constexpr size_t kPlatformJsonBytes = 800;

// The arithmetic above, checked by the compiler rather than by a reader. The
// baseline term is written as twice `kMaxStateBytes` -- the file's text and then
// the parsed rows it becomes, which cost more than the text they came from
// (`state_db.hpp`) -- so the check is a little tighter than the table. It is the
// transfer memory that this is really about: the trimmed socket config is the
// difference between a working engine and one that dies at `socketInitialize`,
// and the number is easy to change by accident and impossible to notice off a
// console.
static_assert(rommsync::sysmodule::ExpectedBsdTransferMemory({}) == 0x1D000,
              "the bsd transfer memory is not the trimmed 116 KiB M0-1 measured");
static_assert(rommsync::sysmodule::ExpectedBsdTransferMemory({}) +
                      2 * rommsync::state::kMaxStateBytes +
                      rommsync::sysmodule::kTransferBufferSize +
                      rommsync::lists::kMaxPlatforms * kPlatformJsonBytes +
                      2 * 0x8000 /* worker thread stacks */ +
                      0x8000 /* newlib arena overhead */ <
                  kInnerHeapSize,
              "the heap no longer covers the peak in the table above");

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

/// The Horizon half of `io::FileSync` (#16): make what was just written
/// durable, before the rename that publishes it.
///
/// `fsdevCommitDevice` rather than a per-file sync, because devkitA64's newlib
/// exports no `fsync` and libnx offers no way to reach the `FsFile` behind a
/// `FILE*`. `fsFsCommit` on `sdmc:` is the primitive Horizon does have, and it
/// covers the staged file the way the contract in `atomic_file.hpp` allows: the
/// path is ignored because everything on this card is committed together.
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

void Log(const std::string& line) { svcOutputDebugString(line.c_str(), line.size()); }

}  // namespace

extern "C" {

// A sysmodule has no applet session and wants one FS session, not the several
// libnx opens for homebrew.
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
  extern void* fake_heap_start;
  extern void* fake_heap_end;

  fake_heap_start = g_inner_heap;
  fake_heap_end = g_inner_heap + sizeof(g_inner_heap);
}

void __appInit(void) {
  Result rc = smInitialize();
  if (R_FAILED(rc)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
  }

  // hosversionSet before anything version-gated is called; libnx assumes it.
  // Aborting rather than carrying on is the point: an unset host version reads
  // as 0, so every hosversionAtLeast() gate after this -- including the ones
  // inside fsInitialize() below -- silently takes the pre-1.0.0 path. A wrong
  // answer everywhere is worse than a refusal to start.
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
  rc = fsInitialize();
  if (R_FAILED(rc)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
  }
  fsdevMountSdmc();

  // The transport, here rather than on first use, because `socketInitialize`
  // and `sslInitialize` are `sm` lookups and `sm` is about to close. Its
  // failure is deliberately *not* fatal: a console with no network is one the
  // overlay still has to be able to open, read its settings on and see its
  // queue on, so the engine gets a client that answers `kConnectFailed` rather
  // than a process that refuses to start (`http/ssl_http_client.hpp`).
  //
  // It is also not a boot-time wait: nothing here talks to a network. The bsd
  // transfer memory this allocates out of `g_inner_heap` is the dominant term
  // in the budget above.
  rommsync::sysmodule::NetworkInitialize();

  // Claimed before `sm` goes away, and aborting rather than carrying on: a
  // sysmodule that runs without its service is a process nothing can reach and
  // nothing can diagnose -- the overlay would report it as not running, which
  // is the one thing it would not be.
  rc = rommsync::sysmodule::RegisterPort(&g_service_port);
  if (R_FAILED(rc)) {
    diagAbortWithResult(rc);
  }

  smExit();
}

void __appExit(void) {
  rommsync::sysmodule::NetworkExit();
  fsdevUnmountAll();
  fsExit();
}

}  // extern "C"

int main(int, char**) {
  // A crash dump or a debug log that cannot say which build produced it costs
  // an afternoon, and version() is the cheapest possible answer.
  const char* ua = rommsync::version();
  svcOutputDebugString(ua, std::strlen(ua));

  // Before anything writes, and once: `io::SetFileSync` is process-wide and is
  // not meant to be swapped while a write is in flight (atomic_file.hpp).
  rommsync::io::SetFileSync(&HorizonFileSync);

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

  // The other seam, `UseServer`, is deliberately **not** used, and the reason is
  // worth reading before anyone tries it: `lists::Service` answers a page that
  // needs a request with `kOk` and `ListPage::pending`, and makes the request in
  // `Pump()` -- on a thread this build does not have (`list_service.hpp`).
  // Handing it a client without also starting that worker would turn "offline",
  // which the browser draws, into "pending" forever, which it cannot. Starting
  // the worker is M7-2 (#37), and it is written there. That is also why the two
  // setters are still two: this one is safe to call the moment a client exists,
  // and that one is not.

  rommsync::ipc::ServiceCore core(engine);
  rommsync::sysmodule::ServiceServer server(core, g_service_port);
  // Does not return. A sysmodule that fell out of its service loop would sit in
  // the process list answering nothing.
  server.Run();
  return 0;
}

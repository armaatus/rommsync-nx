// sys-rommsync entry point.
//
// It initialises the services the engine will need, registers the `rommsync`
// IPC service, and spends the rest of the process's life answering it (M4-1,
// #23). There is still no scheduler and no networking -- those are M2 and M7 --
// so the commands behind them answer `ipc::Error::kUnavailable`; what is real
// today is the console's configuration -- read *and* written, since M5-3 (#30)
// -- whether it has ever paired, and the build (see `engine.hpp`).
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

#include <cstring>
#include <string>

#include "engine.hpp"
#include "ipc/server.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"

namespace {

// Sized for the skeleton, not for the engine. The real budget is one in-flight
// download buffer, a TLS context, and the `state.db` baseline -- which is the
// one item that grows with the library rather than being a fixed buffer: the
// file's text plus the parsed rows, bounded by `state::kMaxStateBytes` and
// `state::kMaxRecords`, which are sized against *this* constant and have to be
// revisited with it. It is set when those exist
// (docs/DEVELOPMENT.md#tls-in-a-sysmodule); streaming to file is what keeps it
// from having to grow with the size of a rom.
constexpr size_t kInnerHeapSize = 0x80000;

alignas(16) u8 g_inner_heap[kInnerHeapSize];

/// The registered port, claimed in `__appInit` and served in `main`. A file
/// scope variable because `__appInit` takes no arguments and returns nothing --
/// it is libnx's hook, not ours.
Handle g_service_port = INVALID_HANDLE;

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
  setsysExit();

  // config.ini, token.dat, save staging and the download destinations all live
  // on the SD card, so fs is not optional for this process.
  rc = fsInitialize();
  if (R_FAILED(rc)) {
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
  }
  fsdevMountSdmc();

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

  // Read once, here rather than per request: `GetStatus` and `GetConfig` are
  // documented never to fail and are polled every frame by the status screen,
  // so neither may go near the SD card (`ipc.hpp`). Since M5-3 (#30) the one
  // command that *does* -- `SetConfig` -- re-reads the file it just wrote and
  // swaps the live `Config`, which is what makes a setting changed from the
  // overlay take effect without a reboot.
  rommsync::sysmodule::SdEngine engine;
  engine.Load();

  rommsync::ipc::ServiceCore core(engine);
  rommsync::sysmodule::ServiceServer server(core, g_service_port);
  // Does not return. A sysmodule that fell out of its service loop would sit in
  // the process list answering nothing.
  server.Run();
  return 0;
}

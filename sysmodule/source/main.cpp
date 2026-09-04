// sys-rommsync entry point.
//
// Skeleton: it initialises the services the engine will need, records the build
// version where a crash dump would show it, and exits. There is no scheduler,
// no IPC service and no networking yet -- those are M1 onward, and none of them
// runs on hardware before the M8-1 gate.
//
// What it does prove, every CI run, is that core/ still builds for aarch64:
// every translation unit under core/src is compiled into this target
// (../switch.mk), so one that quietly became host-only breaks the build here
// rather than months later.

#include <switch.h>

#include <cstring>

#include "rommsync/core.hpp"

namespace {

// Sized for the skeleton, not for the engine. The real budget is one in-flight
// download buffer plus a TLS context, and it is set when those exist
// (docs/DEVELOPMENT.md#tls-in-a-sysmodule); streaming to file is what keeps it
// from having to grow with the size of a rom.
constexpr size_t kInnerHeapSize = 0x80000;

alignas(16) u8 g_inner_heap[kInnerHeapSize];

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

  smExit();
}

void __appExit(void) {
  fsdevUnmountAll();
  fsExit();
}

}  // extern "C"

int main(int, char**) {
  // Every core/ object is in this link whether or not main names it, so the
  // build already covers the engine. This call is here for the other half: a
  // crash dump or a debug log that cannot say which build produced it costs an
  // afternoon, and version() is the cheapest possible answer.
  const char* ua = rommsync::version();
  svcOutputDebugString(ua, std::strlen(ua));
  return 0;
}

// A thread whose stack this process chose, rather than one devkitA64 chose for
// it (M9-2, #207).
//
// `std::thread` cannot be given a stack size, and on devkitA64 that is not a
// detail: `pthread_create` with a null attribute reaches libnx's
// `__syscall_thread_create`, which turns a zero `stack_size` into `128*1024`
// and hands it to `threadCreate` with a null `stack_mem` -- so libnx calls
// `__libnx_aligned_alloc(0x1000, stack_sz + tls_sz + reent_sz)` and the stack
// comes out of `g_inner_heap` (nx/source/kernel/thread.c). Two of those cost
// this process ~257 KiB, against a table in `main.cpp` that budgeted 64 KiB for
// both and a margin smaller than the difference. With `-fno-exceptions` an
// allocation that does not fit is `std::terminate` on a console with no crash
// report.
//
// devkitPro's own `pthread_create` reads `stackaddr` and `stacksize` straight
// out of the attribute and passes them down (libsysbase/pthread.c), so setting
// the size is all it takes to make the cost a number this repo owns. The other
// way out is the one every reference sysmodule takes -- sys-tune's
// `alignas(0x1000) u8 tuneThreadBuffer[0x6000]` with a direct `threadCreate` --
// and it is not available here: `engine.hpp` names no libnx type, which is what
// lets `engine.commands` drive this class on a laptop, and a static buffer
// would be shared by every `SdEngine` a host test constructs.
//
// It is deliberately not a `std::thread` drop-in. `std::thread`'s constructor
// *throws* when a thread cannot be created, which under `-fno-exceptions` is
// `std::terminate`; `Start` returns false instead, and each caller says what a
// console that could not start that thread does -- two in `engine.cpp`, and the
// PSC watcher's in `power_psc.cpp` (M9-4, #208).
#pragma once

#include <pthread.h>

#include <cstddef>

namespace rommsync::sysmodule {

/// The stack each of this process's own threads gets.
///
/// **128 KiB, which is what they already had** -- this constant does not shrink
/// anything, it makes the number visible so that shrinking it later is one edit
/// and one table row rather than an archaeology exercise. What it is measured
/// against, from `-fstack-usage` over the whole linked sysmodule plus the direct
/// call graph of `sys-rommsync.elf` (#207):
///
///   * the deepest single frame the build compiles is 4,288 bytes
///     (`io::CopyAtomically`), and `SdEngine::RunOneTick` alone is 3,136;
///   * `json::Parser` is the one recursion in the tree, bounded by
///     `json::kMaxDepth = 64` at ~496 bytes a level, so ~31 KiB of the worst
///     case is JSON nesting the *server* chooses;
///   * the deepest chain reachable from `RunWorker` totals ~59 KiB, and from
///     `DrivePairing` ~39 KiB.
///
/// That bound is an over-approximation of our own code and an *under*-count of
/// everything else: 244 indirect call sites (`http::HttpClient` and
/// `fs::FileSystem` are interfaces) are edges no static walk follows. 64 KiB
/// would leave the worker under 10% headroom over a figure that is already
/// known to be incomplete, and overflowing a Horizon thread stack is a data
/// abort, not a diagnosable failure.
///
/// Must be a multiple of the page size, which is what the `static_assert` below
/// checks: `__syscall_thread_create` refuses a `stack_size` with any of the low
/// twelve bits set, and that is the one of these three constraints that is
/// silent on a console. The hosts differ and neither matches it -- macOS wants a
/// multiple of *its* page size, and glibc has no alignment rule at all, only a
/// `PTHREAD_STACK_MIN` floor that 128 KiB sits exactly on. `Start` handles a
/// host that refuses this size; see the comment there.
inline constexpr std::size_t kThreadStackBytes = 0x20000;

/// What one thread costs the inner heap *besides* its stack.
///
/// `threadCreate` allocates `stack_sz + tls_sz + reent_sz` in one
/// `__libnx_aligned_alloc(0x1000, ...)`, and `__syscall_thread_create` allocates
/// a `struct __pthread_t` beside it. Measured against this build: `tls_sz` is
/// `__tls_end - __tls_start` rounded to 16, which is 0x430; `reent_sz` is
/// `sizeof(struct _reent)` rounded to 16, which is 0x240. The rest of this term
/// is the up-to-0x1000 a `memalign` of a page-aligned block wastes at the front
/// and the two chunk headers.
inline constexpr std::size_t kThreadHeapOverheadBytes = 0x2000;

/// The PSC watcher's stack (M9-4, #208), and the reason it is not the number
/// above.
///
/// That one is measured against `RunWorker` and `DrivePairing`, whose deepest
/// chains reach ~59 KiB and ~39 KiB. This thread's whole call graph is
/// `power::Watcher::Run` -> `SdEngine::Quiesce`/`Resume` -> a lock, two
/// condition-variable waits, a `timed_mutex`, and -- only when the quiesce ran
/// out of budget -- one `log::Warn`, which is where the deepest frame under it
/// lives (`log::Redact`, and newlib's `fopen`/`fwrite` under `FileSink`). None
/// of it recurses and none of it parses JSON, which is what makes the worker's
/// number what it is.
///
/// 32 KiB is ~8x the deepest single frame the whole build compiles -- 4,288
/// bytes, in `io::CopyAtomically`, which this thread does not reach -- and it is
/// 96 KiB the inner heap does not have to find for a thread that spends its life
/// blocked on an event.
inline constexpr std::size_t kWatcherStackBytes = 0x8000;

static_assert(kThreadStackBytes % 0x1000 == 0,
              "a thread stack must be page-aligned or __syscall_thread_create returns EINVAL");
static_assert(kWatcherStackBytes % 0x1000 == 0,
              "a thread stack must be page-aligned or __syscall_thread_create returns EINVAL");

/// One thread, started with an explicit stack and joined by its owner.
///
/// Not copyable and not movable: two are members of `SdEngine` and are joined in
/// its destructor, and the third is the PSC watcher's, joined by the
/// subscription that owns it (`power_psc.cpp`) -- which is the whole of the
/// lifetime this needs to model. **A `SizedThread` that is started and never
/// joined leaves a thread running against a destroyed owner** -- the same hazard
/// `std::thread` answers by calling `std::terminate`, which is not an answer
/// available here.
class SizedThread {
 public:
  SizedThread() = default;
  SizedThread(const SizedThread&) = delete;
  SizedThread& operator=(const SizedThread&) = delete;

  /// Start `Method` on `self`, on a thread with `stack_bytes` of stack -- or,
  /// where a host refuses that size, with the platform's default. False when
  /// the thread could not be created at all, which is the caller's to report:
  /// there is nothing to throw and nobody to catch it.
  ///
  /// `stack_bytes` is a parameter rather than the constant because the three
  /// threads this process starts are not the same size of job: two run the
  /// engine and one waits on a PSC event (`kWatcherStackBytes`). It must be
  /// page-aligned, which each constant asserts at its own declaration -- there
  /// is nothing here that can check a value handed in.
  ///
  /// **`joinable()` is what tells the two falses apart.** A second `Start` on a
  /// running thread also answers false rather than replacing the one that is
  /// there, and a caller that has not checked `joinable()` first would read that
  /// as a failure. Both callers in `engine.cpp` check, because both are also
  /// answering "has this already been started" -- so the guard here is the one
  /// that stops a double call leaking a thread, not the one anybody reads.
  template <auto Method, typename T>
  bool Start(T* self, std::size_t stack_bytes = kThreadStackBytes) {
    if (started_) {
      return false;
    }
    ::pthread_attr_t attributes;
    if (::pthread_attr_init(&attributes) != 0) {
      return false;
    }
    // **A refused size falls back to the platform's default rather than to no
    // thread at all.** On Horizon it cannot be refused: the only reason
    // `__syscall_thread_create` returns `EINVAL` for a size is the low twelve
    // bits, and the `static_assert` above rules that out -- so the fallback is
    // unreachable there and the console always gets the budgeted stack. Off the
    // console it is reachable: glibc's floor is `PTHREAD_STACK_MIN`, which on
    // aarch64 Linux is exactly 128 KiB, and it carves the static TLS block out
    // of whatever it is given. Degrading a host test run to "the worker never
    // started, nothing syncs" over that would be a worse failure than the
    // platform default `std::thread` used to take.
    const bool sized = ::pthread_attr_setstacksize(&attributes, stack_bytes) == 0;
    bool ok = sized && ::pthread_create(&handle_, &attributes, &Enter<Method, T>, self) == 0;
    if (!sized) {
      ok = ::pthread_create(&handle_, nullptr, &Enter<Method, T>, self) == 0;
    }
    ::pthread_attr_destroy(&attributes);
    started_ = ok;
    return ok;
  }

  /// Whether there is a thread to join. `joinable` and `join` are named as
  /// `std::thread` names them, because both call sites in `engine.cpp` were
  /// written against that spelling and mean exactly the same thing by it.
  bool joinable() const { return started_; }

  /// Wait for the thread to finish. Safe on one that was never started, and
  /// leaves this no longer joinable -- so a second `join` is a no-op rather than
  /// undefined, which is what `std::thread` would have made it.
  void join() {
    if (started_) {
      ::pthread_join(handle_, nullptr);
      started_ = false;
    }
  }

 private:
  /// The C entry point `pthread_create` wants, one per method it is given.
  template <auto Method, typename T>
  static void* Enter(void* self) {
    (static_cast<T*>(self)->*Method)();
    return nullptr;
  }

  ::pthread_t handle_{};
  bool started_ = false;
};

}  // namespace rommsync::sysmodule

// Every `operator new` and `operator delete` this process makes, counted (M9-15,
// #204).
//
// Modelled on llvm's `libcxx/test/support/count_new.h`: all twenty replaceable
// C++20 signatures are replaced (`count_new.cpp`), each allocation carries its
// own size so an unsized `delete` can be counted in bytes too, and the counters
// are what a test reads. What this adds is the one number llvm's does not keep
// and this project needs: **outstanding bytes and their peak**, because the
// question asked of the sysmodule is not "did it leak" but "does what it holds
// stay flat as ticks go by" -- and a growing *reachable* map is invisible to
// LSan and valgrind alike, which only report blocks nothing points at.
//
// Two things defeat a counter like this, and both are handled where they arise:
//
//   * **A sanitizer.** ASan interposes `operator new` itself, so under one this
//     file replaces nothing and `kActive` is false; a scenario reads it and skips
//     rather than asserting over counts that are not ours. The host suite runs
//     unsanitized, which is where these are asserted.
//   * **Allocation elision**, which [expr.new]/14 permits: a `new` whose result
//     is unused may simply not happen, and a test asserting on it then passes for
//     the wrong reason. tests/CMakeLists.txt builds the counting target with
//     `-fno-builtin-operator-new` (Clang) or `-fno-allocation-dce` (GCC), and
//     `alloc.counts` checks that a bare `delete new int` is still seen.
//
// Process-wide and thread-safe: the sysmodule's worker allocates on its own
// thread, and that is the allocation this exists to see.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ROMMSYNC_COUNT_NEW_DISABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
#define ROMMSYNC_COUNT_NEW_DISABLED 1
#endif
#endif

namespace count_new {

#if defined(ROMMSYNC_COUNT_NEW_DISABLED)
inline constexpr bool kActive = false;
#else
inline constexpr bool kActive = true;
#endif

/// The counters. Relaxed atomics: each is a tally, never a synchronisation.
struct Counters {
  std::atomic<std::int64_t> allocations{0};   ///< every `new`, ever
  std::atomic<std::int64_t> deallocations{0}; ///< every non-null `delete`, ever
  std::atomic<std::int64_t> outstanding{0};   ///< blocks allocated and not yet freed
  std::atomic<std::int64_t> outstanding_bytes{0};
  std::atomic<std::int64_t> peak_bytes{0};    ///< highest `outstanding_bytes` since `ResetPeak`
};

Counters& Get();

/// What the counters said at one instant.
struct Snapshot {
  std::int64_t allocations = 0;
  std::int64_t outstanding = 0;
  std::int64_t outstanding_bytes = 0;
  std::int64_t peak_bytes = 0;
};

inline Snapshot Now() {
  Counters& counters = Get();
  Snapshot snapshot;
  snapshot.allocations = counters.allocations.load(std::memory_order_relaxed);
  snapshot.outstanding = counters.outstanding.load(std::memory_order_relaxed);
  snapshot.outstanding_bytes = counters.outstanding_bytes.load(std::memory_order_relaxed);
  snapshot.peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed);
  return snapshot;
}

/// Start a new peak from what is outstanding now, so the next `Now().peak_bytes`
/// is the high-water mark of whatever runs in between.
inline void ResetPeak() {
  Counters& counters = Get();
  counters.peak_bytes.store(counters.outstanding_bytes.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
}

/// llvm's `RequireAllocationGuard`, as a value rather than an RAII assertion:
/// how many allocations happened since it was taken. A window that saw none is
/// the case where elision, or a replacement that is not installed, makes a
/// "nothing grew" pass for the wrong reason -- so the caller asserts a minimum.
class Window {
 public:
  Window() : start_(Now()) {}
  std::int64_t allocations() const { return Now().allocations - start_.allocations; }

 private:
  Snapshot start_;
};

}  // namespace count_new

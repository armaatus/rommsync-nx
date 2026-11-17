// Every C++ allocation in this process, counted (the footprint row).
//
// The same measurement `alloc.*` makes of the host suite (tests/count_new.hpp):
// global operator new and delete replaced, the size kept in a header in front
// of each block. Not newlib's mallinfo, which tlsprobe samples: a sample taken
// between rows misses the peak inside one, and the peak is the number the
// sysmodule's heap has to hold.
#include <cstdlib>
#include <new>

#include <atomic>

#include "selftest.hpp"

namespace {

std::atomic<std::size_t> g_outstanding{0};
std::atomic<std::size_t> g_peak{0};
std::atomic<std::uint64_t> g_allocations{0};

// Room for the size in front of the block, keeping malloc's 16-byte alignment.
constexpr std::size_t kHeader = 16;

void Note(std::size_t size) {
  const std::size_t now = g_outstanding.fetch_add(size, std::memory_order_relaxed) + size;
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  std::size_t peak = g_peak.load(std::memory_order_relaxed);
  while (now > peak && !g_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
  }
}

void* Allocate(std::size_t size, std::size_t alignment) {
  const std::size_t header = alignment > kHeader ? alignment : kHeader;
  void* block = alignment > kHeader ? std::aligned_alloc(alignment, header + size)
                                    : std::malloc(header + size);
  // -fno-exceptions: there is no bad_alloc to throw, and the sysmodule's answer
  // to running out is the same abort (switch.mk says why).
  if (block == nullptr) std::abort();
  unsigned char* user = static_cast<unsigned char*>(block) + header;
  reinterpret_cast<std::size_t*>(user)[-1] = size;
  reinterpret_cast<std::size_t*>(user)[-2] = header;
  Note(size);
  return user;
}

void Release(void* pointer) {
  if (pointer == nullptr) return;
  unsigned char* user = static_cast<unsigned char*>(pointer);
  const std::size_t size = reinterpret_cast<std::size_t*>(user)[-1];
  const std::size_t header = reinterpret_cast<std::size_t*>(user)[-2];
  g_outstanding.fetch_sub(size, std::memory_order_relaxed);
  std::free(user - header);
}

}  // namespace

void* operator new(std::size_t size) { return Allocate(size, 0); }
void* operator new[](std::size_t size) { return Allocate(size, 0); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return Allocate(size, 0); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return Allocate(size, 0);
}
void* operator new(std::size_t size, std::align_val_t align) {
  return Allocate(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return Allocate(size, static_cast<std::size_t>(align));
}
void operator delete(void* pointer) noexcept { Release(pointer); }
void operator delete[](void* pointer) noexcept { Release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { Release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { Release(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { Release(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { Release(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { Release(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  Release(pointer);
}

namespace selftest {

HeapCount HeapNow() {
  HeapCount count;
  count.outstanding_bytes = g_outstanding.load(std::memory_order_relaxed);
  count.peak_bytes = g_peak.load(std::memory_order_relaxed);
  count.allocations = g_allocations.load(std::memory_order_relaxed);
  return count;
}

void HeapResetPeak() { g_peak.store(g_outstanding.load(std::memory_order_relaxed)); }

}  // namespace selftest

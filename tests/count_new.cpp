// The twenty replaceable allocation functions of C++20, counted. See
// count_new.hpp for why, and for what this deliberately does not do under a
// sanitizer.
//
// Each block is allocated with a header in front of it that holds the size the
// caller asked for, so that the eight `delete`s which are not told a size can
// still take the right number of bytes off `outstanding_bytes`. The header is
// as wide as the block's alignment (never less than the default new alignment),
// which keeps the pointer handed out aligned the way the caller asked.
#include "count_new.hpp"

namespace count_new {

Counters& Get() {
  // Constant-initialised, so it exists before the first static constructor that
  // allocates -- which runs before `main`, and before any function-local static
  // could be trusted to.
  static constinit Counters counters;
  return counters;
}

}  // namespace count_new

#if !defined(ROMMSYNC_COUNT_NEW_DISABLED)

#include <cstdlib>
#include <limits>
#include <new>

namespace count_new {
namespace {

constexpr std::size_t kDefaultAlignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

std::size_t HeaderFor(std::size_t alignment) {
  return alignment < kDefaultAlignment ? kDefaultAlignment : alignment;
}

void* Allocate(std::size_t size, std::size_t alignment) noexcept {
  const std::size_t header = HeaderFor(alignment);
  if (size > std::numeric_limits<std::size_t>::max() - header) {
    return nullptr;
  }
  void* block = nullptr;
  if (::posix_memalign(&block, header, header + (size == 0 ? 1 : size)) != 0) {
    return nullptr;
  }
  auto* user = static_cast<unsigned char*>(block) + header;
  *reinterpret_cast<std::size_t*>(user - sizeof(std::size_t)) = size;

  Counters& counters = Get();
  counters.allocations.fetch_add(1, std::memory_order_relaxed);
  counters.outstanding.fetch_add(1, std::memory_order_relaxed);
  const std::int64_t now =
      counters.outstanding_bytes.fetch_add(static_cast<std::int64_t>(size),
                                           std::memory_order_relaxed) +
      static_cast<std::int64_t>(size);
  std::int64_t peak = counters.peak_bytes.load(std::memory_order_relaxed);
  while (now > peak &&
         !counters.peak_bytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
  }
  return user;
}

void Release(void* pointer, std::size_t alignment) noexcept {
  if (pointer == nullptr) {
    return;
  }
  auto* user = static_cast<unsigned char*>(pointer);
  const std::size_t size = *reinterpret_cast<std::size_t*>(user - sizeof(std::size_t));
  Counters& counters = Get();
  counters.deallocations.fetch_add(1, std::memory_order_relaxed);
  counters.outstanding.fetch_sub(1, std::memory_order_relaxed);
  counters.outstanding_bytes.fetch_sub(static_cast<std::int64_t>(size),
                                       std::memory_order_relaxed);
  std::free(user - HeaderFor(alignment));
}

void* AllocateOrThrow(std::size_t size, std::size_t alignment) {
  void* pointer = Allocate(size, alignment);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

}  // namespace
}  // namespace count_new

using count_new::Allocate;
using count_new::AllocateOrThrow;
using count_new::Release;
using count_new::kDefaultAlignment;

// --- the eight allocating forms -----------------------------------------------

void* operator new(std::size_t size) { return AllocateOrThrow(size, kDefaultAlignment); }
void* operator new[](std::size_t size) { return AllocateOrThrow(size, kDefaultAlignment); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return Allocate(size, kDefaultAlignment);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return Allocate(size, kDefaultAlignment);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateOrThrow(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateOrThrow(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return Allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return Allocate(size, static_cast<std::size_t>(alignment));
}

// --- the twelve deallocating forms --------------------------------------------

void operator delete(void* pointer) noexcept { Release(pointer, kDefaultAlignment); }
void operator delete[](void* pointer) noexcept { Release(pointer, kDefaultAlignment); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  Release(pointer, kDefaultAlignment);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  Release(pointer, kDefaultAlignment);
}
void operator delete(void* pointer, std::size_t) noexcept { Release(pointer, kDefaultAlignment); }
void operator delete[](void* pointer, std::size_t) noexcept {
  Release(pointer, kDefaultAlignment);
}
void operator delete(void* pointer, std::align_val_t alignment) noexcept {
  Release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
  Release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  Release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
  Release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
  Release(pointer, static_cast<std::size_t>(alignment));
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
  Release(pointer, static_cast<std::size_t>(alignment));
}

#endif

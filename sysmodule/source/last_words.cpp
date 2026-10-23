#include "last_words.hpp"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <string_view>
#include <typeinfo>

#include <cxxabi.h>  // __cxa_current_exception_type: what is in flight, without RTTI

#include "rommsync/log.hpp"

namespace rommsync::sysmodule {
namespace {

/// `strncat` with the bound stated as the buffer's size, never past it.
void Append(char* out, std::size_t size, std::size_t* used, std::string_view text) {
  if (*used + 1 >= size) {
    return;
  }
  const std::size_t room = size - 1 - *used;
  const std::size_t take = text.size() < room ? text.size() : room;
  std::memcpy(out + *used, text.data(), take);
  *used += take;
  out[*used] = '\0';
}

}  // namespace

void DescribeTermination(const char* mangled, char* out, std::size_t size) noexcept {
  if (out == nullptr || size == 0) {
    return;
  }
  out[0] = '\0';
  std::size_t used = 0;
  if (mangled == nullptr) {
    Append(out, size, &used, "std::terminate with no exception in flight");
    return;
  }
  // The Itanium name rather than a demangled one: demangling allocates, and
  // the commonest reason to be here is that there is nothing to allocate.
  // `St9bad_alloc` is as greppable as `std::bad_alloc`.
  Append(out, size, &used, "an uncaught ");
  Append(out, size, &used, mangled);
  if (std::strstr(mangled, "bad_alloc") != nullptr) {
    Append(out, size, &used, ": the heap is spent, and the sysmodule stopped");
  } else {
    Append(out, size, &used, ": the sysmodule stopped");
  }
}

void LastWords() noexcept {
  const std::type_info* thrown = abi::__cxa_current_exception_type();
  char detail[160];
  DescribeTermination(thrown != nullptr ? thrown->name() : nullptr, detail, sizeof detail);
  log::WriteLast(log::Event::kTerminated, detail);
  std::abort();
}

void InstallLastWords() { std::set_terminate(&LastWords); }

}  // namespace rommsync::sysmodule

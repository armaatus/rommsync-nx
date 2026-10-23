// What the sysmodule says on its way out of `std::terminate` (#250).
//
// The sysmodule is built `-fno-exceptions` (`switch.mk`), so anything the
// standard library throws -- a `bad_alloc` when the 1.5 MiB heap is spent above
// all, but a `length_error` or a `bad_optional_access` too -- is caught by
// nobody and ends in `std::terminate`. The default handler calls `abort`, and
// a console that aborts a custom sysmodule leaves no crash report anybody can
// read: the sysmodule is simply not running any more, and the overlay says so.
//
// So the handler is replaced with one that writes a `process.terminated` line
// to the log first -- what was thrown, by its type -- and then aborts exactly as
// the default would. It allocates nothing (`log::WriteLast`), because the most
// likely reason to be here is that nothing is left to allocate.
//
// No libnx type is named, so this compiles on the host as well, where
// tests/test_oom.cpp installs it in a child process, fails an allocation, and
// reads the line the child left before it died.
#pragma once

#include <cstddef>

namespace rommsync::sysmodule {

/// Make `LastWords` the terminate handler, process-wide. Called from `main`
/// once the log has a sink; before that there is nowhere to write to and the
/// default handler is as good.
void InstallLastWords();

/// The handler itself: write the line, then `std::abort`.
[[noreturn]] void LastWords() noexcept;

/// The detail `LastWords` writes, for the exception type named `mangled` --
/// what `std::type_info::name()` answers, or null when terminate was called
/// with nothing in flight. Into `out`, which it fills and terminates, and which
/// is never longer than `size` including the terminator. Allocates nothing.
/// Exposed so the sentence can be checked without dying for it.
void DescribeTermination(const char* mangled, char* out, std::size_t size) noexcept;

}  // namespace rommsync::sysmodule

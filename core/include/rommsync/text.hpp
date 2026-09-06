// Cutting a string a human will read, on a UTF-8 boundary.
//
// One function, and it exists because there were four copies of it: the list
// row projection (`list_service.cpp`), a `Diagnostic` message and a rom's
// `fs_name` on the way to an IPC payload (`ipc.cpp`, `ipc_service.cpp`), and a
// conflict entry's rom name (`conflict_log.cpp`, M7-1). Every one of them was
// the same six lines with a different constant.
//
// **The boundary is the whole point.** These strings are the user's data --
// a rom's name, a save's file name, a path off a card -- and every one of them
// crosses `ipc::kMaxPayloadBytes` to a renderer. Half a code point there is a
// row the console draws as a replacement glyph, or a decoder that refuses the
// page it is on.
//
// What this is **not** is a length limit anything derives from. A download is
// asked for by `rom_id` and a restore by an entry id; nothing downstream reads
// one of these strings back. Cutting a value something *opens* -- an SD path --
// is a different and worse bug, which is why `conflicts::Entry` refuses a path
// that does not fit rather than shortening one.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace rommsync::text {

/// What a cut string ends with, so the eye can tell one from a short name.
inline constexpr const char* kEllipsis = "...";

/// Cut `text` to at most `limit` bytes on a UTF-8 boundary, `kEllipsis` in
/// place of the rest. Returns it unchanged when it already fits.
///
/// The result is `limit` bytes of text plus the ellipsis, so it can exceed
/// `limit` by three. Every caller's bound is a bound on a *page* or a *file*
/// with room to spare, not on one string to the byte; a cut that had to fit
/// `limit` exactly would have to decide what the ellipsis displaces.
std::string Shorten(std::string_view text, std::size_t limit);

/// The same, in place. For the caller that already holds a `std::string` it is
/// about to move on.
void ShortenInPlace(std::string* text, std::size_t limit);

}  // namespace rommsync::text

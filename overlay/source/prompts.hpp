// The button prompts a screen labels its controls with.
//
// Their own header, and not `screen_frame.hpp`'s, because that one used to
// include `tesla.hpp` and a prompt is text: the half of a screen that lays one
// out gets everything it needs from files that name no libultrahand type
// (M9-7, #198). Not `draw_list.hpp`'s either -- that is a rendering seam, and a
// glyph table is not part of one.
//
// Every screen still reaches these through `screen_frame.hpp`, which includes
// this file, so nothing had to change at a call site.
#pragma once

#include <string>

namespace rommsync::overlay {

/// The button glyphs libtesla draws from the Switch's own font.
///
/// Here rather than in each screen: they were written out in `sync_screen.cpp`,
/// `library_screen.cpp` and `pairing_screen.cpp` before `screen_frame.hpp`
/// existed, and a private-use codepoint typed from memory in four places is
/// four chances to get one wrong.
inline constexpr const char* kGlyphA = "\uE0E0";
inline constexpr const char* kGlyphB = "\uE0E1";
inline constexpr const char* kGlyphX = "\uE0E2";
inline constexpr const char* kGlyphY = "\uE0E3";

/// A control's prompt: the glyph, two spaces, and what pressing it does.
///
/// The two spaces are the whole of it, and they are why this is a function
/// rather than a convention: the glyph is a square in the console's font, and a
/// prompt that spaced it differently from the screen next door reads as a
/// different control.
inline std::string Prompt(const char* glyph, const std::string& label) {
  return std::string(glyph) + "  " + label;
}

}  // namespace rommsync::overlay

// The three things every screen in this directory does before it can draw
// anything: pick a colour for a `Tone`, complete the version handshake, and
// decide *not running* from *unreachable* when a call fails.
//
// It exists because those were written out twice -- in `status_screen.cpp`
// (M4-1, #23) and in `pairing_screen.cpp` (M4-5, #27) -- and #24 is the third
// screen. #27 left the note asking whichever of M4-2..M4-4 landed next to lift
// them here rather than type a third copy: a palette or a handshake change is
// otherwise a four-file edit, and the fourth file is the one that gets missed.
// It was not done in #27 because three M4 screens were in flight in parallel
// worktrees at the time and touching `status_screen.cpp` would have been a
// merge conflict bought for nothing.
//
// What is *not* here is any screen's layout. The geometry constants stay beside
// the `Draw` that uses them, because they are the one thing that will be
// adjusted against a real panel in M8-2 (#44) and a person doing that should
// have one block per screen to look at.
//
// Nothing here has ever run: overlay UI is verified after the M8-1 gate
// (overlay/AGENTS.md). What is checked today is that it cross-compiles.
#pragma once

#include <tesla.hpp>

#include <cstdint>
#include <string>

#include "ipc_client.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// The renderer's palette for a `Tone`. `core/` names no colour (hard rule 4),
/// so this is the only place the two vocabularies meet -- and it uses
/// libultrahand's theme variables rather than literals so a user's theme still
/// applies.
///
/// Through `Renderer::a`, which folds in the overlay's fade animation alpha.
/// Without it the frame's chrome fades on open and close while everything a
/// screen draws stays fully opaque and pops (libtesla's own convention).
tsl::Color ColorFor(Tone tone);

/// The colour a label, a hint or a caption is drawn in -- the quiet half of
/// every row, and not a `Tone`: it is a role rather than a judgement.
tsl::Color MutedColor();

/// The button glyphs libtesla draws from the Switch's own font.
///
/// Here rather than in each screen for `ColorFor`'s reason: they were written
/// out in `sync_screen.cpp`, `library_screen.cpp` and `pairing_screen.cpp`
/// before this file existed, and a fourth copy in `settings_screen.cpp` (#26)
/// is a private-use codepoint typed from memory in four places.
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
std::string Prompt(const char* glyph, const std::string& label);

/// The session state a screen keeps between frames, and the two questions it
/// asks of it.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open and every screen shares it (overlay/AGENTS.md).
class ScreenFrame {
 public:
  explicit ScreenFrame(IpcClient& client) : client_(client) {}

  ScreenFrame(const ScreenFrame&) = delete;
  ScreenFrame& operator=(const ScreenFrame&) = delete;

  /// Open the port if it is not open, and complete the version handshake if it
  /// has not been completed on this session.
  ///
  /// `Link::kOk` means a command may be sent. Anything else is what the screen
  /// must render instead, with `sysmodule_interface()` filled in for
  /// `kIncompatible`.
  ///
  /// The handshake is command 0 and it comes first, always: its encoding is
  /// frozen, so it is the only call that is safe to make before knowing whether
  /// this build can decode the others (`ipc::Command`). A mismatch is "update
  /// the sysmodule", not a decode failure, and telling those apart is the whole
  /// reason it exists.
  Link Ready();

  /// What a failed typed call means.
  ///
  /// `MalformedResponse()` is a payload this build cannot read -- the sysmodule
  /// is there and the two halves disagree. Anything else is the transport, and
  /// which of the two unreachable sentences applies is established by dropping
  /// the session and trying the port again, here rather than on the next frame:
  /// whether the port is still there is exactly what decides which sentence the
  /// user gets, and deferring it would draw one of them for a frame on no
  /// evidence.
  Link Diagnose(Result rc);

  /// What `GetInterfaceVersion` last answered. Only meaningful for
  /// `kIncompatible`, where the two numbers are the whole diagnosis.
  std::uint32_t sysmodule_interface() const { return sysmodule_interface_; }

 private:
  /// Drop the session, try the port again, and answer with whichever of the two
  /// unreachable states that establishes.
  Link Reopen();

  IpcClient& client_;

  /// Whether the contract check has passed on the current session. Reset with
  /// every session, because a session that came back is a sysmodule that may
  /// have been replaced since the last one.
  bool version_checked_ = false;

  std::uint32_t sysmodule_interface_ = 0;
};

}  // namespace rommsync::overlay

// The two things every screen in this directory does before it can draw
// anything: complete the version handshake, and decide *not running* from
// *unreachable* when a call fails.
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
// the painter that uses them, because they are the one thing that will be
// adjusted against a real panel in M8-2 (#44) and a person doing that should
// have one block per screen to look at. Nor is the palette: `ColorFor` and
// `MutedColor` moved to `palette.hpp` in M9-7 (#198), because they are the half
// of a frame that needs libultrahand and this half does not.
//
// **This file runs.** `ctest -R 'overlay.(version|errors)'` drives `Ready()` and
// `Diagnose()` against a real `ipc::Dispatch` through the real `IpcClient`,
// which is what naming no libultrahand type here buys. `prompts.hpp` is
// included for the button glyphs every screen reaches for through this header;
// it names no libultrahand type either.
#pragma once

#include <cstdint>
#include <string>

#include "ipc_client.hpp"
#include "prompts.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

// `ColorFor`, `MutedColor` and `CurrentPalette` are `palette.hpp`'s, and
// `kGlyphA`..`kGlyphY` and `Prompt` are `prompts.hpp`'s -- both moved in M9-7
// (#198) so that what is left here compiles on a host. A screen that draws
// includes `palette.hpp`; the prompts still arrive through this header.

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

  /// `Ready()` in the two halves a screen that may not wait needs (#299): the
  /// first opens the port and never asks the sysmodule anything; the second
  /// judges the handshake's answer, however it was sent. `Ready()` is the
  /// two of them with a waited-for `GetInterfaceVersion` between.
  ///
  /// `Connect` answers `kOk` or `kNotRunning`; `checked()` then says whether a
  /// handshake is still owed on this session.
  Link Connect();
  bool checked() const { return version_checked_; }
  Link Admit(Result rc, std::uint32_t sysmodule_interface);

  /// What a failed typed call means.
  ///
  /// `MalformedResponse()` is a payload this build cannot read -- the sysmodule
  /// is there and the two halves disagree. `NotResponding()` is a call that ran
  /// out of budget, which is `kNotResponding` and leaves the session alone
  /// (#209). Anything else is the transport, and
  /// which of the two unreachable sentences applies is established by dropping
  /// the session and trying the port again, here rather than on the next frame:
  /// whether the port is still there is exactly what decides which sentence the
  /// user gets, and deferring it would draw one of them for a frame on no
  /// evidence.
  Link Diagnose(Result rc);

  /// From here on, never open a session: `Ready()` answers `kNotRunning` when
  /// none is open, and a transport failure drops the session without trying
  /// the port again.
  ///
  /// For a screen's destructor, which is all that calls it (#285). The gui
  /// stack is libtesla's and is emptied by `~Overlay`, after `exitServices`
  /// has stopped the client, so a destructor that opened a session would open
  /// one nothing ever closes -- and nx-ovlloader's process never exits, so
  /// three of those are the sysmodule's whole session table. A destructor may
  /// use the session that is already open, and nothing else.
  void Retire() { retired_ = true; }

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

  /// `Retire()` was called.
  bool retired_ = false;
};

}  // namespace rommsync::overlay

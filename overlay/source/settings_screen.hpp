// The settings screen: the whole effective configuration, everything the parser
// thought of it, the root menu, and "Re-pair".
//
// M4-4 (#26), and the place a bad `config.ini` stops being invisible. What it
// holds is the *drawing* and the input map; every decision -- which sentence a
// console with no server gets, what `interval_min = 0` reads as, which `roms`
// folder is marked as the write target, whether "Re-pair" may be pressed --
// is `rommsync::overlay::SettingsView`'s, in `core/`, where a host test can
// reach it (`ctest -R overlay.settings`). The same split the other four screens
// have, for the same reason.
//
// **This screen does not write `config.ini`.** The sysmodule owns writes
// (docs/ARCHITECTURE.md): a row the view model gives a `change` -- `[sync]
// on_server_delete` and `[sources]`, since M10-23 (#264) -- sends that one
// assignment over `SetConfig` (M5-3, #30) and draws the answer, and every other
// row is marked, not offered. `overlay.settings`, `overlay.library` and
// `overlay.sync_actions` all grep this directory for the write path rather than
// leaving it reviewed.
//
// **It is also the overlay's root menu.** Until this screen there was no
// navigation at all: `main.cpp` opened on the status screen and nothing pushed
// any other, so `SyncScreen` (#24), `LibraryScreen` (#25) and `PairingScreen`
// (#27) compiled and `--gc-sections` dropped them from the image. The status
// screen reaches this one and this one reaches the rest.
//
// **Nothing here has ever run.** Overlay UI is one of the few things an emulator
// cannot exercise, so it is verified last, on hardware, after the M8-1 gate
// (overlay/AGENTS.md). Reading the folder map off a television is M8-2 (#44).
#pragma once

#include <tesla.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ipc_client.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "settings_pump.hpp"

namespace rommsync::overlay {

/// One `tsl::Gui` drawing one `SettingsView`.
///
/// The client is borrowed rather than owned: the overlay holds one session for
/// as long as it is open, and every screen shares it (overlay/AGENTS.md).
class SettingsScreen : public tsl::Gui {
 public:
  explicit SettingsScreen(IpcClient& client);

  tsl::elm::Element* createUI() override;

  /// Called once per frame by Tesla, and it is *not* one `GetConfig` per frame.
  ///
  /// Unlike a `Status`, a configuration changes only when somebody edits it, and
  /// this payload is the whole folder map -- up to `ipc::kMaxPayloadBytes` to
  /// decode and a few hundred rows to rebuild. So it is polled on a cadence
  /// (`SettingsPump::kFramesBetweenReads`) rather than on every draw, and
  /// immediately after anything this screen does that could have changed it --
  /// and since M11-24 (#329) no frame waits for the answer.
  void update() override;

  /// Up/Down scroll the list, A opens the screen under the cursor or steps the
  /// setting under it to its next value, and X starts -- and then confirms -- a
  /// re-pair.
  ///
  /// Only a menu row or a row with a `change` answers A. A row that highlighted
  /// but did nothing would be a control that lies (#24's rule).
  bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch,
                   HidAnalogStickState left_stick, HidAnalogStickState right_stick) override;

 private:
  /// One drawn line of the scrolling half of the screen.
  ///
  /// The view is sections of rows and the panel is a flat window, so the two are
  /// flattened once per answer rather than re-walked on every frame -- and the
  /// cursor is an index into *this*, which is what makes Up/Down scroll the
  /// complaints and the folder map as well as move the selection.
  struct Entry {
    enum class Kind { kComplaint, kSectionTitle, kRow };
    Kind kind = Kind::kRow;
    std::size_t section = 0;
    std::size_t index = 0;
  };

  /// Rebuild `entries_` from the pump's view, keeping the cursor where it can
  /// be.
  void Rebuild();

  const SettingsView& view() const { return pump_.view(); }

  /// The row the cursor is on, or nullptr when there is none.
  const SettingsRow* CursorRow() const;

  /// How many prompt rows this view will draw, reserved before the list for
  /// `library_screen.cpp`'s reason: a list longer than the panel would
  /// otherwise push every control off the bottom.
  s32 PromptRows() const;

  /// Draw the pump's view into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  /// Borrowed, for the screens this one pushes.
  IpcClient& client_;

  /// The poll, the two presses and the view (`settings_pump.hpp`). "Re-pair"
  /// sends `StartPair` first and `Unpair` after it, the opposite of the order
  /// docs/AUTH.md describes: it was #26's gate on a `SdEngine::StartPairing`
  /// that answered `kUnavailable` unconditionally, and it outlives M1-6 (#123)
  /// building that -- `StartPair` writes nothing and touches no token when it
  /// refuses, so asking it first is free, and the token is discarded only once
  /// a pairing is genuinely starting. `overlay_settings_view.hpp` carries the
  /// rest of the reasoning.
  SettingsPump pump_;

  /// The pump's `revision()` `entries_` was built at.
  std::uint64_t rendered_ = 0;

  std::vector<Entry> entries_;

  /// Where the cursor is in `entries_`. `-1` when there is nothing to scroll.
  int cursor_ = -1;
};

}  // namespace rommsync::overlay

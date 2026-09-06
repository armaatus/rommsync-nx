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
// **This screen does not edit `config.ini`.** The sysmodule owns writes
// (docs/ARCHITECTURE.md); live edits are M5-3 (#30) and the row is marked, not
// offered. `overlay.settings`, `overlay.library` and `overlay.sync_actions` all
// grep this directory for the write path rather than leaving it reviewed.
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
#include <vector>

#include "ipc_client.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "screen_frame.hpp"

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
  /// (`kPollFrames`) rather than on every draw, and immediately after anything
  /// this screen does that could have changed it.
  void update() override;

  /// Up/Down scroll the list, A opens the screen under the cursor, and X starts
  /// -- and then confirms -- a re-pair.
  ///
  /// Only a menu row answers A. Everything else on this screen is read-only in
  /// this build, and a row that highlighted but did nothing would be a control
  /// that lies (#24's rule).
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

  /// Ask for the configuration, and turn whatever came back -- including
  /// nothing -- into `view_`.
  void Poll();

  /// Draw the sysmodule's absence, and drop everything that belonged to the
  /// session that is gone.
  ///
  /// One function rather than the five call sites that reach it, because four
  /// of them are error arms: the `repair_` reset is the easy half to forget,
  /// and forgetting it leaves a half-pressed "Re-pair" armed across a sysmodule
  /// restart.
  void ShowUnreachable(Link link);

  /// Re-render `view_` from the last answer and rebuild the flat list.
  void Refresh();

  /// Rebuild `entries_` from `view_`, keeping the cursor where it can be.
  void Rebuild();

  /// X: ask once, then do it.
  ///
  /// **`StartPair` goes first and `Unpair` follows it**, which is the opposite
  /// of the order docs/AUTH.md describes. `SdEngine::StartPairing` is still
  /// `kUnavailable` and no issue owns it, so a button that discarded the token
  /// first would leave a console that cannot pair again from the overlay at all
  /// -- #26's own words. `StartPair` writes nothing and touches no token when it
  /// refuses, so asking it first is free, and the token is discarded only once a
  /// pairing is genuinely starting. `overlay_settings_view.hpp` carries the
  /// rest of the reasoning.
  void PressRepair();

  /// The row the cursor is on, or nullptr when there is none.
  const SettingsRow* CursorRow() const;

  /// How many prompt rows this view will draw, reserved before the list for
  /// `library_screen.cpp`'s reason: a list longer than the panel would
  /// otherwise push every control off the bottom.
  s32 PromptRows() const;

  /// Draw `view_` into the bounds `CustomDrawer` hands us.
  void Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) const;

  IpcClient& client_;

  /// The session handshake, and which of the two unreachable sentences a failed
  /// call means. Shared with every other screen (`screen_frame.hpp`).
  ScreenFrame frame_{client_};

  /// The last answer, kept so a press can re-render without a round trip.
  ipc::ConfigView config_;

  /// Where the button is between presses. Not in the view, for the reason
  /// `LastCommand` is not (`overlay_sync_actions.hpp`).
  RepairState repair_;

  /// What the last render produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns, and
  /// a blank one there is indistinguishable from a broken overlay.
  SettingsView view_ = RenderSettingsUnreachable(Link::kNotRunning);

  std::vector<Entry> entries_;

  /// Where the cursor is in `entries_`. `-1` when there is nothing to scroll.
  int cursor_ = -1;

  /// Frames since the last `GetConfig`. Starts past the cadence so the first
  /// `update()` asks rather than waiting half a second to draw anything.
  int since_poll_ = 0;
};

}  // namespace rommsync::overlay

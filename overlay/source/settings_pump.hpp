// The settings screen's IPC: what `SettingsScreen::update()` and its two
// presses send, without the `tsl::Gui` around them (M11-24, #329).
//
// Split out of `settings_screen.cpp` for `status_pump.hpp`'s reason. The screen
// used to wait, within the budget, for a `GetConfig` twice a second and for
// `SetConfig` -- a `config.ini` written to the card under its mutex, hundreds
// of milliseconds while a download writes -- on the draw thread. Here they go
// through a `Poller`; a write out past the budget says *still applying*, and
// `ctest -R overlay.frames` times every frame. It names no libultrahand type,
// which `overlay.portable` holds it to.
#pragma once

#include <cstdint>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

/// The settings screen's frame and its two presses.
class SettingsPump {
 public:
  /// Frames between two `GetConfig`s: half a second at 60 Hz.
  ///
  /// A configuration changes when somebody edits it, which on this console is
  /// never while the overlay is up -- unlike a `Status`, which moves under the
  /// status screen every second. What one poll costs is the whole folder map
  /// decoded and a few hundred rows rebuilt, so it is asked on a cadence and
  /// immediately after anything this screen does.
  static constexpr int kFramesBetweenReads = 30;

  explicit SettingsPump(IpcClient& client);

  SettingsPump(const SettingsPump&) = delete;
  SettingsPump& operator=(const SettingsPump&) = delete;

  /// `update()`. Never waits on the sysmodule.
  void Frame();

  /// A on a row with a `change`: send that one assignment to `SetConfig`, and
  /// read what is in force once it answers. A press while one is out goes
  /// nowhere: the first is still being answered.
  void PressChange(config::Assignment assignment);

  /// X: ask once, then do it -- `StartPair`, and `Unpair` once a pairing is
  /// genuinely starting. `settings_screen.hpp` carries why in that order.
  void PressRepair();

  const SettingsView& view() const { return view_; }

  /// Bumped whenever `view()` changes, so the screen rebuilds its flat list
  /// on an answer rather than on every frame.
  std::uint64_t revision() const { return revision_; }

  /// A re-pair has started a pairing and discarded the old token: the screen
  /// pushes the pairing screen, which shows the code. True once per re-pair.
  bool TakePairing();

 private:
  void Read(Result rc);
  void Answered(ipc::Command command, Result rc);

  /// Draw the sysmodule's absence, and drop everything that belonged to the
  /// session that is gone -- a half-pressed "Re-pair" surviving a sysmodule
  /// that went away would fire on the first press after it came back.
  void ShowUnreachable(Link link);

  /// `view_` from the last answer.
  void Refresh();

  IpcClient& client_;
  ScreenFrame frame_{client_};
  Poller poller_{frame_, client_, ipc::Command::kGetConfig, kFramesBetweenReads};

  /// The last answer, kept so a press can re-render without a round trip.
  ipc::ConfigView config_;
  RepairState repair_;
  ChangeState change_;

  /// The assignment `SetConfig` is out with.
  config::Assignment pending_;

  /// What the last render produced. Starts as "not running" rather than as an
  /// empty screen: the first frame is drawn before the first poll returns.
  SettingsView view_ = RenderSettingsUnreachable(Link::kNotRunning);
  std::uint64_t revision_ = 0;

  bool pairing_ = false;
};

}  // namespace rommsync::overlay

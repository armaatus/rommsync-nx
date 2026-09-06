// ovl-rommsync entry point.
//
// One `tsl::Overlay` holding one session on `sys-rommsync` and, for now, one
// screen loaded at start: the status screen (M4-1, #23). The library / queue,
// sync and settings screens are M4-2..M4-4 and are added beside it.
//
// `PairingScreen` (M4-5, #27) is built and nothing pushes it yet. The settings
// screen's "Re-pair" (M4-4, #26) is the one place the flow is reached from --
// an overlay that opened on the pairing screen would be asking a paired console
// for a code it does not need -- so until #26 lands it compiles here and
// `--gc-sections` drops it from the image.
//
// The session is owned here rather than by a screen because every screen shares
// it: `smGetService` per screen would be a handle per screen, and an overlay
// that leaked one would take the sysmodule's session table with it. Opening is
// left to the screen's own poll -- a sysmodule that is not running is a state
// the status screen draws (`overlay_status_view.hpp`), not a reason for the
// overlay to fail to load.
//
// Nothing in this directory has ever run. Overlay UI is one of the few things an
// emulator cannot exercise, so it is verified last, on hardware, after the M8-1
// gate; overlay/README.md carries the script that does it.

#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <memory>

#include "ipc_client.hpp"
#include "status_screen.hpp"

namespace {

class RommsyncOverlay : public tsl::Overlay {
 public:
  std::unique_ptr<tsl::Gui> loadInitialGui() override {
    return initially<rommsync::overlay::StatusScreen>(client_);
  }

  /// Closed here rather than in the screen: Tesla tears the gui down and
  /// rebuilds it whenever the overlay is hidden, and a session dropped on every
  /// hide would be re-opened on every show for no reason.
  void exitServices() override { client_.Close(); }

 private:
  rommsync::overlay::IpcClient client_;
};

}  // namespace

int main(int argc, char** argv) { return tsl::loop<RommsyncOverlay>(argc, argv); }

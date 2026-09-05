#---------------------------------------------------------------------------------
# ovl-rommsync -- the Ultrahand/Tesla overlay, packaged as an .ovl for
# sdmc:/switch/.overlays/.
#
# Everything that is not specific to this target lives in ../switch.mk; see the
# comment at the top of that file.
#
# The overlay compiles core/, and links almost none of it. It is a thin client
# that renders state and sends IPC commands (overlay/AGENTS.md), and the IPC
# codecs it needs to do that are core/'s -- `rommsync/ipc.hpp`, shared with the
# sysmodule so one copy of the field names serves both halves and they cannot
# disagree about one. A second copy over here is exactly the drift M5-2 exists to
# prevent. What else it takes from core/ is the screens' view models
# (`rommsync/overlay_status_view.hpp`, `rommsync/overlay_pairing_view.hpp`),
# which are the half of a screen that is a decision rather than a draw call and
# are therefore the half a host test can check. `--gc-sections` drops the engine
# the screens never reference; the rule that matters is unchanged, and it is
# about ownership rather than about the link map: no sync, download or auth
# logic lives in this directory.
#
# libultrahand is the library the screens are drawn with, pinned as a submodule
# at lib/libultrahand and compiled from source -- upstream ships no library. Its
# sources are vendored rather than ours, so ../switch.mk builds them with
# -isystem headers and without the warning flags they were never held to.
#---------------------------------------------------------------------------------
TOPDIR ?= $(CURDIR)

TARGET := ovl-rommsync

# Append the ULTR signature and name the result .ovl rather than .nro.
ROMMSYNC_OVERLAY := 1

# The IPC codecs and the status screen's view model. See the note above.
ROMMSYNC_USE_CORE := 1

# lib/libultrahand, and the vendored-source rules that come with it.
ROMMSYNC_ULTRAHAND := 1

# Tesla reads the NACP for the name in the overlay list.
APP_TITLE  := ovl-rommsync
APP_AUTHOR := armaatus

include $(TOPDIR)/../switch.mk

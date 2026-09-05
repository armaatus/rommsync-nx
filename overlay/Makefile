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
# prevent. `--gc-sections` drops the engine the screens never reference; the rule
# that matters is unchanged, and it is about ownership rather than about the
# link map: no sync, download or auth logic lives in this directory.
#
# libultrahand is not a dependency yet either. It arrives with the first real
# screen in M4-1; until then this is a libnx .nro with the ULTR signature, which
# is what an .ovl is -- enough for CI to prove the cross-compile links and to
# publish an artifact, and not a submodule nobody exercises for four milestones.
#---------------------------------------------------------------------------------
TOPDIR ?= $(CURDIR)

TARGET := ovl-rommsync

# Append the ULTR signature and name the result .ovl rather than .nro.
ROMMSYNC_OVERLAY := 1

# The IPC codecs. See the note above: compiled, mostly gc-sectioned away.
ROMMSYNC_USE_CORE := 1

# Tesla reads the NACP for the name in the overlay list, so these are user
# visible even though nothing here draws a frame yet.
APP_TITLE  := ovl-rommsync
APP_AUTHOR := armaatus

include $(TOPDIR)/../switch.mk

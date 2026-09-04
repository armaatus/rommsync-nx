#---------------------------------------------------------------------------------
# ovl-rommsync -- the Ultrahand/Tesla overlay, packaged as an .ovl for
# sdmc:/switch/.overlays/.
#
# Everything that is not specific to this target lives in ../switch.mk; see the
# comment at the top of that file.
#
# The overlay does NOT build core/. It is a thin client that renders state and
# sends IPC commands (overlay/AGENTS.md); the engine lives behind the sysmodule,
# and linking it in here would make that easy to forget.
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

# Tesla reads the NACP for the name in the overlay list, so these are user
# visible even though nothing here draws a frame yet.
APP_TITLE  := ovl-rommsync
APP_AUTHOR := armaatus

include $(TOPDIR)/../switch.mk

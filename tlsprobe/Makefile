#---------------------------------------------------------------------------------
# rommsync-tlsprobe -- the M0-1 de-risking spike, packaged as a plain .nro that
# a human launches from the homebrew menu (or from Ryujinx) and closes again.
#
# It is deliberately NOT a sysmodule and deliberately NOT on the boot path: an
# .nro cannot run unless someone starts it, which is the property that makes
# this experiment safe to run on a console at all (issue M0-1, CLAUDE.md hard
# rule 1). Nothing here is installed; nothing here survives a reboot.
#
# Everything that is not specific to this target lives in ../switch.mk. Two
# things are specific:
#
#   * core/ is NOT compiled in. The sysmodule already proves core/ links for
#     aarch64, and this target's whole output is a *measurement* -- every
#     kilobyte of .text and .bss in the image has to be attributable to the TLS
#     path being measured, not to an engine the probe never calls.
#   * ROMMSYNC_WANT_VERSION asks switch.mk for version.hpp anyway, because a
#     heap measurement that cannot say which build produced it is an anecdote.
#---------------------------------------------------------------------------------
TOPDIR ?= $(CURDIR)

TARGET := rommsync-tlsprobe

ROMMSYNC_WANT_VERSION := 1

# Shown by hbmenu, which is how the person running this tells it apart from the
# other homebrew on the SD card they are testing from.
APP_TITLE  := rommsync TLS probe
APP_AUTHOR := armaatus

include $(TOPDIR)/../switch.mk

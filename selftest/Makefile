#---------------------------------------------------------------------------------
# rommsync-selftest -- M12-1 (#306), the on-console selftest: a plain .nro a
# person launches from hbmenu against a disposable RomM, which runs the real
# Horizon backends and writes a report scripts/selftest-judge.py reads.
#
# Like tlsprobe/, and for tlsprobe's reason, it is NOT a sysmodule and NOT on
# the boot path: an .nro runs only when someone starts it, and nothing here is
# installed or survives a reboot (CLAUDE.md hard rule 1). Building it is fine
# before the v1 gate; running it is M12's (docs/HARDWARE_TEST_PLAN.md, PRE-02).
#
# Unlike tlsprobe/, it is the engine under test, not a measurement of TLS
# alone, so:
#
#   * core/ is compiled in (ROMMSYNC_USE_CORE), the way the sysmodule compiles
#     it -- the protocol rows run the same RunTick, Negotiate and PairingSession.
#   * the sysmodule's console backends are compiled from where they live, never
#     copied: `../sysmodule/source/http` whole (the `ssl` transport), and
#     card.cpp through source/sysmodule_card.cpp, which includes it -- the rest
#     of sysmodule/source is the sysmodule's process (main, IPC, PSC), and
#     SOURCES can only take a directory whole.
#---------------------------------------------------------------------------------
TOPDIR ?= $(CURDIR)

TARGET := rommsync-selftest

ROMMSYNC_USE_CORE := 1

SOURCES  += ../sysmodule/source/http
INCLUDES += ../sysmodule/source ../sysmodule/source/http

# Shown by hbmenu, which is how the person running it tells it apart from the
# other homebrew on the card.
APP_TITLE  := rommsync selftest
APP_AUTHOR := armaatus

include $(TOPDIR)/../switch.mk

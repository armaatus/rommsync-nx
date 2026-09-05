#---------------------------------------------------------------------------------
# sys-rommsync -- the background engine, packaged as an ExeFS .nsp for
# Atmosphere (sdmc:/atmosphere/contents/<title id>/exefs.nsp).
#
# Everything that is not specific to this target lives in ../switch.mk; see the
# comment at the top of that file. Nothing built here is run on hardware before
# the M8-1 gate.
#---------------------------------------------------------------------------------
TOPDIR ?= $(CURDIR)

TARGET      := sys-rommsync
CONFIG_JSON := sys-rommsync.json

# Compile and link core/ into the sysmodule. That is the point of this target
# today: the .nsp is a skeleton, but proving the portable engine links for
# aarch64 is what stops core/ from drifting host-only between here and M2.
ROMMSYNC_USE_CORE := 1

# The IPC binding (M5-2). A separate directory because it is the one part of this
# target that is *not* portable engine code -- it names `Result`, `HipcRequest`
# and the tls buffer, which core/ may not (hard rule 4). Object files are named
# by basename, so nothing in here may share one with core/src.
SOURCES += source/ipc

include $(TOPDIR)/../switch.mk

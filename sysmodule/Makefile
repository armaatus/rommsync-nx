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

# The IPC binding (M5-2) and the Horizon HTTP backend (M1-7, #126). Separate
# directories because they are the parts of this target that are *not* portable
# engine code -- `ipc/` names `Result`, `HipcRequest` and the tls buffer, and
# `http/ssl_http_client.cpp` names the `ssl` service, none of which core/ may
# (hard rule 4). Object files are named by basename, so nothing in either may
# share one with core/src -- which is why the backend is `http_wire.cpp` and
# `ssl_http_client.cpp` rather than the `http.cpp` core/src already has.
SOURCES += source/ipc source/http

include $(TOPDIR)/../switch.mk

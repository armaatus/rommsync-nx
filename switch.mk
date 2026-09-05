#---------------------------------------------------------------------------------
# Shared devkitPro build rules for the two Switch targets.
#
# sysmodule/ and overlay/ are built by devkitA64 Makefiles rather than by the
# top-level CMake build (docs/DEVELOPMENT.md#toolchain): the .nsp packaging and
# .nro/.ovl steps are Makefile-native. Both of those Makefiles are otherwise the
# same 150 lines of devkitPro template, so the template lives here once and each
# target sets only what makes it different.
#
# This is devkitPro's own `examples/switch/templates/{sysmodule,application}`
# Makefile, split into a shared half, plus five project-specific changes:
#
#   * -Wall -Wextra -Wpedantic -Werror, the repo standard, not the template's
#     bare -Wall (docs/DEVELOPMENT.md#coding-standards).
#   * ROMMSYNC_USE_CORE pulls core/ into the build, so the portable engine is
#     compiled and linked for Horizon rather than only syntax-checked.
#   * ROMMSYNC_ULTRAHAND pulls the libultrahand submodule in the same way, and
#     with it the vendored-source rules: upstream's headers reached with
#     -isystem and its objects compiled without the three warning flags it was
#     never held to, so ours stay -Werror clean either way.
#   * version.hpp, which CMake generates for the host build, is generated here
#     from the same VERSION file and the same template.
#   * ROMMSYNC_OVERLAY appends the ULTR signature to the .nro and names the
#     result .ovl (overlay/README.md).
#
# A target's Makefile sets its variables and then includes this file last:
#
#   TOPDIR ?= $(CURDIR)
#   TARGET := sys-rommsync
#   ROMMSYNC_USE_CORE := 1
#   include $(TOPDIR)/../switch.mk
#
# TOPDIR is what makes that work from the recursive inner make too: the outer
# make exports it as the project directory, so `$(TOPDIR)/..` is the repo root
# in both halves, while $(CURDIR) is the build directory in the inner one.
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITPRO)/libnx/switch_rules

ROMMSYNC_ROOT := $(TOPDIR)/..

# Single-sourced with the host build; see the comment on file(READ) in
# CMakeLists.txt. Reading it wrong is worth stopping for -- a sysmodule that
# reports an empty version is a support problem nobody can debug remotely.
ROMMSYNC_VERSION := $(strip $(shell cat $(ROMMSYNC_ROOT)/VERSION 2>/dev/null))
ifeq ($(ROMMSYNC_VERSION),)
$(error could not read a version from $(ROMMSYNC_ROOT)/VERSION)
endif

# The version in the NACP, which is what the overlay list shows. switch_rules
# has already defaulted it to devkitPro's 1.0.0 by this point, so this overrides
# rather than fills in -- otherwise the half of the build a user can see would
# be the one half not tracking VERSION.
APP_VERSION := $(ROMMSYNC_VERSION)

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
BUILD    := build
SOURCES  += source
INCLUDES += include

# The portable engine, compiled into the target rather than linked as a prebuilt
# library: every core/ translation unit is compiled by this build, so one that
# quietly became host-only is a red build here. What the skeleton does not
# reference is then dropped from the image by --gc-sections below, which is the
# right trade for a resident process -- the compile is the part that has to
# cover everything, not the image. core/ stays buildable for aarch64 only
# because it may not include a libnx header; the `static` CI job enforces that
# half.
ifneq ($(strip $(ROMMSYNC_USE_CORE)),)
	SOURCES  += ../core/src
	INCLUDES += ../core/include
endif

# libultrahand -- the overlay library ovl-rommsync draws with (overlay/README.md),
# built from the pinned submodule rather than linked as a prebuilt: upstream
# ships sources and a .mk, and there is no library to link.
#
# Its own ultrahand.mk is deliberately not included. It appends to SOURCES and
# INCLUDES without distinguishing its directories from ours, and that
# distinction is the whole point of ROMMSYNC_VENDOR_* below -- upstream's code
# was never held to this repo's warnings, and holding it to them would make an
# upstream bump a broken build here.
ifneq ($(strip $(ROMMSYNC_ULTRAHAND)),)
	ULTRAHAND_DIR := lib/libultrahand

	# The submodule may be absent from a shallow checkout, and the failure that
	# causes is a page of missing-header errors rather than a sentence. CI clones
	# with `submodules: recursive`; this is what says so when it did not.
	# $(TOPDIR), not $(CURDIR): this file is read again by the inner make, where
	# $(CURDIR) is the build directory and the check would fail on a submodule
	# that is present.
	ifeq ($(wildcard $(TOPDIR)/$(ULTRAHAND_DIR)/ultrahand.mk),)
$(error $(ULTRAHAND_DIR) is empty -- run `git submodule update --init --recursive`)
	endif

	ROMMSYNC_VENDOR_SOURCES  += $(ULTRAHAND_DIR)/common \
				$(ULTRAHAND_DIR)/libultra/source $(ULTRAHAND_DIR)/libtesla/source
	ROMMSYNC_VENDOR_INCLUDES += $(ULTRAHAND_DIR)/common \
				$(ULTRAHAND_DIR)/libultra/include $(ULTRAHAND_DIR)/libtesla/include

	# What libultrahand itself pulls in: HTTPS for its updater, zip for its
	# package handling. The overlay calls none of it -- everything ovl-rommsync
	# does over the network happens in the sysmodule (overlay/AGENTS.md) -- but
	# libultra's objects reference them, and --gc-sections cannot drop a symbol a
	# linked object still names.
	LIBS += -lcurl -lz -lminizip -lmbedtls -lmbedx509 -lmbedcrypto

	# Where libultrahand keeps the overlay's theme and wallpaper. Ours rather
	# than Ultrahand's own, so uninstalling one does not take the other's
	# settings with it (libultra/include/global_vars.hpp).
	DEFINES += -DUI_OVERRIDE_PATH="\"/config/rommsync/\""
endif

# Third-party sources compiled into this target.
#
# They are compiled rather than linked, so "it is a library, not our problem" is
# not available; and they are somebody else's code, so this repo's
# -Wextra -Wpedantic -Werror is not a standard they were ever held to. libtesla
# alone answers it with 213 warnings. Two consequences, both deliberate:
#
#   * their headers are reached with -isystem (see INCLUDE below), so OUR
#     translation units stay -Werror clean while including them;
#   * their objects compile with VENDOR_CFLAGS/VENDOR_CXXFLAGS, which drop
#     exactly the three flags they fail and keep every other one.
#
# What is NOT relaxed is anything that changes the code generated -- the
# architecture, the standard, -fno-exceptions, -ffunction-sections. A vendored
# object built against a different ABI than the rest of the image is a crash on
# a console with no debugger attached, which is the one class of bug this build
# cannot afford to make easy.
SOURCES += $(ROMMSYNC_VENDOR_SOURCES)

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# Named rather than spelled inline because VENDOR_CFLAGS below is defined as
# "these, minus the ones upstream code cannot meet" -- one list, so the two flag
# sets cannot drift apart on anything else.
ROMMSYNC_WARNINGS := -Wall -Wextra -Wpedantic -Werror
ROMMSYNC_VENDOR_DROPS := -Wextra -Wpedantic -Werror

CFLAGS := -g $(ROMMSYNC_WARNINGS) -O2 -ffunction-sections -fdata-sections \
			$(ARCH) $(DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__

# -std=c++20, not gnu++20: core/ is compiled here and by CMake (which sets
# CXX_EXTENSIONS OFF), so the dialect the portable engine is held to is the same
# in both builds.
#
# -fno-exceptions is a real divergence, and worth knowing about: core/ throws
# nothing today, but a standard-library path that would raise on the host
# (bad_alloc, bad_optional_access, length_error) calls std::terminate here
# instead. Host tests stay green while the sysmodule dies, so keep core/ free of
# anything that can throw rather than relying on the tests to notice.
CXXFLAGS := $(CFLAGS) -std=c++20 -fno-rtti -fno-exceptions

# filter-out rather than a second list: whatever CFLAGS grows, a vendored object
# gets it too unless it is one of the three warning flags above.
VENDOR_CFLAGS   := $(filter-out $(ROMMSYNC_VENDOR_DROPS),$(CFLAGS))
VENDOR_CXXFLAGS := $(filter-out $(ROMMSYNC_VENDOR_DROPS),$(CXXFLAGS))

ASFLAGS := -g $(ARCH)
# --gc-sections pairs with -ffunction-sections above. A sysmodule is resident in
# a tight heap, so dropping unreferenced code is not a micro-optimisation here.
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections -Wl,-Map,$(notdir $*.map)

LIBS += -lnx

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS += $(PORTLIBS) $(LIBNX)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

# Object files are named after the source basename with no directory, so two
# sources with the same name in different SOURCES directories would collide.
CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

# ...which is why this is an error rather than a comment. The sysmodule builds
# two source directories, and sysmodule/README.md plans an `http/` next to the
# core/src/http.cpp that already exists. VPATH resolves the first match, so the
# loser is not compiled at all -- and if it holds only internal-linkage symbols
# the link still succeeds, quietly dropping a translation unit from the image.
DUPLICATE_BASENAMES := $(strip $(shell printf '%s\n' $(CFILES) $(CPPFILES) $(SFILES) \
			| sort | uniq -d))
ifneq ($(DUPLICATE_BASENAMES),)
$(error two sources share a basename ($(DUPLICATE_BASENAMES)); objects are named \
	by basename, so one would shadow the other -- rename one)
endif

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)

# Which of those objects are somebody else's, so the inner make can compile them
# with the relaxed flags. Derived from the same wildcard the object list is, so a
# source upstream adds is covered without an edit here. It has to be computed on
# this side: $(CURDIR) is the build directory in the inner make, and these paths
# are relative to the target.
export VENDOR_OFILES := \
	$(patsubst %.cpp,%.o,$(foreach dir,$(ROMMSYNC_VENDOR_SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))) \
	$(patsubst %.c,%.o,$(foreach dir,$(ROMMSYNC_VENDOR_SOURCES),$(notdir $(wildcard $(dir)/*.c))))

# Ours with -I, vendored and toolchain headers with -isystem. That is what lets
# -Wpedantic stay on: libnx's headers are C headers full of anonymous structs and
# flexible array members, none of which ISO C++ allows, and -Wpedantic -Werror on
# them fails on the first `#include <switch.h>`; libultrahand's headers answer it
# with 213 warnings of their own. -isystem holds our own code to the repo
# standard without holding anyone else's to it.
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(ROMMSYNC_VENDOR_INCLUDES),-isystem $(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-isystem $(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# A CONFIG_JSON is what turns this from a homebrew .nro into an ExeFS .nsp; see
# the CONFIG_JSON block in devkitPro's own template for the autodetect rules we
# deliberately do not reproduce -- our targets always name the file.
ifneq ($(strip $(CONFIG_JSON)),)
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

# CMake writes this one into its own build tree; the devkitPro build gets the
# same header from the same template and the same VERSION file. It is a
# prerequisite of the recursive make below, not of the objects, so the inner
# half only ever finds it complete.
VERSION_HEADER := $(CURDIR)/$(BUILD)/rommsync/version.hpp

# Only the targets that ask for it generate it, so the overlay does not grow a
# dependency on the engine's version header just by existing. Compiling core/
# implies asking, since core/version.cpp includes it; ROMMSYNC_WANT_VERSION is
# for a target that wants the string without the engine (tlsprobe/Makefile).
ifneq ($(strip $(ROMMSYNC_USE_CORE))$(strip $(ROMMSYNC_WANT_VERSION)),)
GENERATED := $(VERSION_HEADER)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD): $(GENERATED)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# Defined after `all` on purpose: make takes the first target in the file as the
# default goal, and a stray rule above it would make `make` regenerate a header
# and build nothing.
$(VERSION_HEADER): $(ROMMSYNC_ROOT)/VERSION $(ROMMSYNC_ROOT)/core/include/rommsync/version.hpp.in
	@mkdir -p $(dir $@)
	@sed 's/@PROJECT_VERSION@/$(ROMMSYNC_VERSION)/g' \
		$(ROMMSYNC_ROOT)/core/include/rommsync/version.hpp.in > $@
	@echo generated ... $(notdir $@)

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm \
		$(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf \
		$(TARGET).map $(TARGET).lst

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

# The vendored objects, and only those, compile with the relaxed warnings. A
# target-specific variable rather than a second pattern rule, so devkitPro's own
# %.o rules stay the only place a compile is spelled out -- and so a source that
# stops being vendored gets the strict flags back by leaving one list.
$(VENDOR_OFILES): CFLAGS   := $(VENDOR_CFLAGS)
$(VENDOR_OFILES): CXXFLAGS := $(VENDOR_CXXFLAGS)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
ifneq ($(strip $(APP_JSON)),)

all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso: $(OUTPUT).elf

else ifneq ($(strip $(ROMMSYNC_OVERLAY)),)

all: $(OUTPUT).ovl

# An .ovl is an .nro with a four-byte ULTR signature appended, which is what
# makes Ultrahand list it rather than ignore it (overlay/README.md).
$(OUTPUT).ovl: $(OUTPUT).nro
	@cp $< $@
	@printf 'ULTR' >> $@
	@echo built ... $(notdir $@)

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

else

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

endif

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------

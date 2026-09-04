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
# Makefile, split into a shared half, plus four project-specific changes:
#
#   * -Wall -Wextra -Wpedantic -Werror, the repo standard, not the template's
#     bare -Wall (docs/DEVELOPMENT.md#coding-standards).
#   * ROMMSYNC_USE_CORE pulls core/ into the build, so the portable engine is
#     compiled and linked for Horizon rather than only syntax-checked.
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

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS := -g -Wall -Wextra -Wpedantic -Werror -O2 -ffunction-sections -fdata-sections \
			$(ARCH) $(DEFINES)

CFLAGS += $(INCLUDE) -D__SWITCH__

# -std=c++20, not gnu++20: core/ is compiled here and by CMake (which sets
# CXX_EXTENSIONS OFF), and the one thing that must not differ between the two is
# the dialect the portable engine is held to.
CXXFLAGS := $(CFLAGS) -std=c++20 -fno-rtti -fno-exceptions

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

# Ours with -I, libnx and the portlibs with -isystem. That is what lets
# -Wpedantic stay on: libnx's headers are C headers full of anonymous structs
# and flexible array members, none of which ISO C++ allows, and -Wpedantic
# -Werror on them fails on the first `#include <switch.h>`. -isystem holds our
# own code to the repo standard without holding the toolchain to it.
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
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

# Only the targets that compile core/ generate it, so the overlay does not grow
# a dependency on the engine's version header just by existing.
ifneq ($(strip $(ROMMSYNC_USE_CORE)),)
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

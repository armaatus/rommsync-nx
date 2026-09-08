# Host build for rommsync-nx.
#
# This builds the portable core library and its tests with the NATIVE toolchain,
# so the engine (auth, sync, downloads, config, state) is developed and proven on
# a laptop and in CI. The Switch targets are built separately with devkitPro
# Makefiles under sysmodule/ and overlay/ -- see docs/DEVELOPMENT.md.
#
# Usage:
#   cmake -S . -B build && cmake --build build
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.20)

# The version lives in a plain VERSION file rather than in this call, because the
# devkitPro Makefiles need the same string and cannot read CMake. Both build
# systems read that file; version.hpp.in is substituted from it either way.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ROMMSYNC_VERSION)
string(STRIP "${ROMMSYNC_VERSION}" ROMMSYNC_VERSION)

# file(READ) leaves no dependency behind, so without this a version bump would
# rebuild nothing: the sysmodule would report the new version (switch.mk makes
# the header a real prerequisite) and the host binaries the old one -- the exact
# drift the shared VERSION file is here to remove.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# `project(VERSION)` takes numbers and nothing else: `1.0.0-rc1` is rejected
# outright, which fails configure and so fails every build and every test.
# docs/DEVELOPMENT.md documents an rc as an ordinary thing to cut, so the suffix
# is stripped for CMake's own use and kept for ours -- the numbers go to
# `project()`, and ROMMSYNC_VERSION stays the string the VERSION file holds.
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" ROMMSYNC_VERSION_NUMERIC "${ROMMSYNC_VERSION}")
if(NOT ROMMSYNC_VERSION_NUMERIC)
  message(FATAL_ERROR "VERSION must start with a number, got '${ROMMSYNC_VERSION}'")
endif()

project(rommsync-nx
  VERSION ${ROMMSYNC_VERSION_NUMERIC}
  DESCRIPTION "On-device RomM sync for the modded Nintendo Switch"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Require the docker RomM rig instead of skipping when it is unreachable. CI
# turns this on, because there the fixture is always brought up first; locally it
# stays off so `ctest` is still useful with Docker stopped.
option(ROMMSYNC_REQUIRE_RIG "Fail rather than skip when the RomM rig is unreachable" OFF)

# Warnings-as-errors is a coding standard (docs/DEVELOPMENT.md). Keep it here so
# it applies to every target rather than being restated per directory.
if(NOT MSVC)
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# The shared ccache lives outside the worktree (scripts/orca/env.sh), so a warm
# cache carries across worktrees -- it is content-addressed and safe to share.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

add_subdirectory(core)
add_subdirectory(host)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# The portable engine. Everything here must compile for BOTH the host and the
# devkitPro targets, so it may not include libnx or any host-only header --
# platform-specific work belongs behind an interface (see the HttpClient split
# described in docs/DEVELOPMENT.md#tls-in-a-sysmodule).
#
# Sources are globbed with CONFIGURE_DEPENDS rather than listed. That is
# deliberate: several agents work in parallel worktrees, and a hand-maintained
# source list is the file they would all conflict on.

file(GLOB_RECURSE ROMMSYNC_CORE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_library(rommsync_core ${ROMMSYNC_CORE_SOURCES})
add_library(rommsync::core ALIAS rommsync_core)

target_include_directories(rommsync_core PUBLIC include)
target_compile_features(rommsync_core PUBLIC cxx_std_20)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/rommsync/version.hpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/rommsync/version.hpp"
  @ONLY)
target_include_directories(rommsync_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")

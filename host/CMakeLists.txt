# Host-only backends for the interfaces core/ declares.
#
# This is the desktop half of rung 1 in docs/TESTING.md: the portable engine
# needs a concrete HttpClient to talk to the docker RomM, and on a laptop or in
# CI that one is libcurl. It is a separate target from rommsync_core on purpose
# -- the devkitPro Makefiles build core/ and must never see this directory, or
# the "nothing in core/ may include a host-only header" rule would be enforced
# by nothing but good intentions.
#
# Sources are globbed for the same reason as in core/: parallel worktrees should
# not have to merge a hand-maintained source list.

# 7.56 is where curl_mime landed, which the multipart path uses. Older releases
# would fall back to the deprecated curl_formadd, and a second code path for a
# libcurl no supported distribution still ships is not worth carrying.
find_package(CURL 7.56 REQUIRED)

file(GLOB_RECURSE ROMMSYNC_HOST_SOURCES CONFIGURE_DEPENDS "src/*.cpp")

add_library(rommsync_host ${ROMMSYNC_HOST_SOURCES})
add_library(rommsync::host ALIAS rommsync_host)

target_include_directories(rommsync_host PUBLIC include)
target_link_libraries(rommsync_host PUBLIC rommsync::core PRIVATE CURL::libcurl)
target_compile_features(rommsync_host PUBLIC cxx_std_20)

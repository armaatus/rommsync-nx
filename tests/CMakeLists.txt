# Tests run against the REAL RomM fixture in server/testing/docker-compose.yml,
# reached through the fault proxy so failure paths can be forced. See
# docs/TESTING.md.
#
# .env is generated per worktree by scripts/orca/env.sh and carries this
# worktree's ports. Reading it here means a test binary never hardcodes a port
# and two worktrees never collide.

set(ENV_FILE "${CMAKE_SOURCE_DIR}/.env")
set(PROXY_BASE_URL "http://127.0.0.1:23515")
if(EXISTS "${ENV_FILE}")
  file(STRINGS "${ENV_FILE}" ENV_LINES REGEX "^PROXY_BASE_URL=")
  foreach(line IN LISTS ENV_LINES)
    string(REGEX REPLACE "^PROXY_BASE_URL=" "" PROXY_BASE_URL "${line}")
  endforeach()
else()
  message(WARNING "no .env found; run ./scripts/orca/env.sh. Falling back to ${PROXY_BASE_URL}")
endif()
message(STATUS "rig: ${PROXY_BASE_URL}")

# --- unit tests: no external services ---------------------------------------
add_executable(test_version test_version.cpp)
target_link_libraries(test_version PRIVATE rommsync::core)
add_test(NAME core.version COMMAND test_version)

# --- rig smoke test: proves docker RomM + fault proxy + ctest all work -------
# Uses libcurl directly on purpose. It tests the RIG, not our code, and must
# keep working while M0-2 replaces it as the way production code speaks HTTP.
find_package(CURL REQUIRED)

add_executable(test_rig_smoke test_rig_smoke.cpp)
target_link_libraries(test_rig_smoke PRIVATE rommsync::core CURL::libcurl)
target_compile_definitions(test_rig_smoke PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}")

add_test(NAME rig.smoke COMMAND test_rig_smoke)
if(NOT ROMMSYNC_REQUIRE_RIG)
  # 77 is the long-standing "skipped" convention. Locally, a stopped Docker
  # should not look like a broken build; in CI the fixture is always up, so
  # -DROMMSYNC_REQUIRE_RIG=ON turns the same condition into a failure.
  set_tests_properties(rig.smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(rig.smoke PROPERTIES TIMEOUT 120)

# --- the native HttpClient against the real rig ------------------------------
# One CTest entry per scenario, selected by argv[1], so a red run names the
# behaviour that broke. The scratch directory is per-build-tree, because three
# worktrees run these concurrently and a shared /tmp path would have them
# overwrite each other's downloads.
add_executable(test_http_native test_http_native.cpp)
target_link_libraries(test_http_native PRIVATE rommsync::host)
target_compile_definitions(test_http_native PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
find_package(Threads REQUIRED)
target_link_libraries(test_http_native PRIVATE Threads::Threads)

set(HTTP_SCENARIOS
  get               # a real RomM response, and the accessors around it
  status            # an HTTP error status is a response, not a transport error
  post_json         # the JSON POST the device-code flow starts on
  post_form         # form-urlencoded, and the login every other scenario needs
  multipart         # a file part streamed from disk
  download          # stream a whole resource to disk
  range             # fetch a slice and prove it is the right slice
  drop              # a TCP reset mid-body: an error, and no file at the destination
  resume            # pick that interrupted download back up
  resume_no_range   # ...even when the server ignores Range and starts over
  resume_empty_body # ...and a bodyless 2xx never promotes the stale partial
  resume_stale_range # a partial the server rejects with 416 does not wedge retries
  range_expected_size # the caller's known size is checked for a slice too
  not_found         # a 404 reaches the caller and leaves no debris
  truncate          # a clean short close, caught only by the caller's known size
  stall             # total and no-progress timeouts both fire
  cancel            # cancellation from another thread, and nothing left behind
)

foreach(scenario IN LISTS HTTP_SCENARIOS)
  add_test(NAME http.${scenario} COMMAND test_http_native ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(http.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # The fault proxy is a single armed scenario shared by every client, so two of
  # these running at once would inject each other's faults.
  set_tests_properties(http.${scenario} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()

# --- worktree environment: the `romm` tab in orca.yaml -----------------------
# Shell, not C++, because the thing under test is a shell script. Splitting the
# two phases keeps the regression (`waits`) meaningful with Docker stopped --
# only `follows` needs the rig, so only it can skip.
find_program(BASH_PROGRAM bash REQUIRED)
set(ROMM_TAB_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_romm_tab.sh")

add_test(NAME orca.romm_tab_waits COMMAND "${BASH_PROGRAM}" "${ROMM_TAB_TEST}" wait)
set_tests_properties(orca.romm_tab_waits PROPERTIES TIMEOUT 60)

add_test(NAME orca.romm_tab_follows COMMAND "${BASH_PROGRAM}" "${ROMM_TAB_TEST}" follow)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(orca.romm_tab_follows PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(orca.romm_tab_follows PROPERTIES TIMEOUT 60)

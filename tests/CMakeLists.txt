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

# --- the fixture is provisioned, not just running ---------------------------
# rig.smoke asks "is RomM up?"; this asks "is it usable?". Staged roms that were
# never scanned leave a green health check over an empty library, which then
# fails whichever test needs a rom and looks like a bug in that test's code.
add_executable(test_rig_provisioned test_rig_provisioned.cpp)
target_link_libraries(test_rig_provisioned PRIVATE rommsync::host)
target_compile_definitions(test_rig_provisioned PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

add_test(NAME rig.provisioned COMMAND test_rig_provisioned)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(rig.provisioned PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(rig.provisioned PROPERTIES TIMEOUT 120)
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

# --- the committed API captures still match a live RomM ----------------------
# server/contract/captures/ is what the docs quote and what M1/M2 will be coded
# against. Re-running the probe here is what stops a RomM upgrade from being
# discovered on a console: the shapes are compared, not the ids and timestamps.
# Python, because the probe it drives is python and shares its redaction rules.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME contract.captures
         COMMAND ${Python3_EXECUTABLE}
                 "${CMAKE_CURRENT_SOURCE_DIR}/test_contract_captures.py"
                 --repo "${CMAKE_SOURCE_DIR}"
                 --scratch "${CMAKE_CURRENT_BINARY_DIR}/captures")
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(contract.captures PROPERTIES SKIP_RETURN_CODE 77)
endif()
# Uploads throwaway saves to the fixture, so it must not interleave with another
# test reading the same library.
set_tests_properties(contract.captures PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

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

# --- the romm tab actually exists: scripts/orca/ensure-romm-tab.sh -----------
# `defaultTabs` in orca.yaml is a request to Orca, and Orca has shipped builds
# that create the tabs and dispatch none of their commands -- leaving a live
# fixture with nothing showing it. These never skip: they stub the orca CLI and
# need neither Docker nor a workspace, so the guarantee stays checked in CI.
add_test(NAME orca.romm_tab_pidfile COMMAND "${BASH_PROGRAM}" "${ROMM_TAB_TEST}" pidfile)
add_test(NAME orca.romm_tab_pidfile_streaming
         COMMAND "${BASH_PROGRAM}" "${ROMM_TAB_TEST}" pidfile_streaming)
add_test(NAME orca.romm_tab_kept COMMAND "${BASH_PROGRAM}" "${ROMM_TAB_TEST}" ensure_noop)
add_test(NAME orca.romm_tab_created COMMAND "${BASH_PROGRAM}" "${ROMM_TAB_TEST}" ensure_creates)
set_tests_properties(orca.romm_tab_pidfile orca.romm_tab_pidfile_streaming
                     orca.romm_tab_kept orca.romm_tab_created
                     PROPERTIES TIMEOUT 60)

# --- the worktree lands ready to look at, and ready to work ------------------
# `romm-browser.sh` opens RomM in a browser tab and signs it in; the log tab
# proves the server is alive, this is where you can see what it scanned.
# `agent-autostart.sh` sends the issue prompt Orca leaves drafted but unsent in a
# new worktree's agent. Both stub the orca CLI and need neither Docker nor a
# workspace, so they never skip.
set(ORCA_BROWSER_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_orca_browser.sh")

foreach(phase opens reuses foreign no_romm submits no_draft unstable
              watch_needs_issue watch_late_draft watch_grace watch_submits watch_single)
  add_test(NAME orca.browser_${phase}
           COMMAND "${BASH_PROGRAM}" "${ORCA_BROWSER_TEST}" ${phase})
  set_tests_properties(orca.browser_${phase} PROPERTIES TIMEOUT 60)
endforeach()

# --- worktree environment: scripts/orca/env.sh -------------------------------
# setup.sh's first step, and not its only caller -- compose.sh generates a
# missing .env too, so the romm tab writes one concurrently. Losing that race
# used to abort setup under `set -e` and leave a worktree with nothing
# provisioned. Pure shell and filesystem: no docker, so these never skip.
set(ORCA_ENV_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_orca_env.sh")

add_test(NAME orca.env_concurrent_writers
         COMMAND "${BASH_PROGRAM}" "${ORCA_ENV_TEST}" concurrent)
add_test(NAME orca.env_never_partial
         COMMAND "${BASH_PROGRAM}" "${ORCA_ENV_TEST}" readable)

# The venv interpreter. setup.sh used to inherit `python3`, which is macOS's
# 3.9.6 under Orca's hook environment -- below the floor server/requirements.txt
# needs, so provisioning died in pip with no usable explanation.
add_test(NAME orca.env_picks_python
         COMMAND "${BASH_PROGRAM}" "${ORCA_ENV_TEST}" python)

set_tests_properties(orca.env_concurrent_writers orca.env_never_partial
                     orca.env_picks_python
                     PROPERTIES TIMEOUT 60)

# --- worktree teardown: the archive hook in orca.yaml -------------------------
# `derives` needs no docker, so the regression it guards -- teardown falling back
# to the compose default and removing nothing -- stays checkable with Docker
# stopped. Only `reap` touches the daemon, so only it can skip.
set(ORCA_TEARDOWN_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_orca_teardown.sh")

add_test(NAME orca.teardown_derives_project
         COMMAND "${BASH_PROGRAM}" "${ORCA_TEARDOWN_TEST}" derives)
set_tests_properties(orca.teardown_derives_project PROPERTIES TIMEOUT 60)

# The autostart watcher outlives setup.sh on purpose, so removal has to stop it
# -- and must not signal a pid the watcher merely left behind.
add_test(NAME orca.teardown_stops_watcher
         COMMAND "${BASH_PROGRAM}" "${ORCA_TEARDOWN_TEST}" watcher)
set_tests_properties(orca.teardown_stops_watcher PROPERTIES TIMEOUT 60)

add_test(NAME orca.reap_spares_live_worktrees
         COMMAND "${BASH_PROGRAM}" "${ORCA_TEARDOWN_TEST}" reap)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(orca.reap_spares_live_worktrees PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(orca.reap_spares_live_worktrees PROPERTIES TIMEOUT 60)

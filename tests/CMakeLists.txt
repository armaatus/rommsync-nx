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

# The JSON reader every response shape is read through. Pure parsing, so this
# never skips -- a body that should have been refused is a bug with or without
# a server to have sent it.
add_executable(test_json test_json.cpp)
target_link_libraries(test_json PRIVATE rommsync::core)
target_include_directories(test_json PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core.json COMMAND test_json)

# The auth structs against the committed captures in server/contract/captures/.
# It reads those files rather than restating them, so the structs are pinned to
# the same bytes the docs quote and contract.captures re-checks against a live
# RomM. No network, so it never skips either.
add_executable(test_auth_shapes test_auth_shapes.cpp)
target_link_libraries(test_auth_shapes PRIVATE rommsync::core)
target_include_directories(test_auth_shapes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_auth_shapes PRIVATE
  ROMMSYNC_CAPTURES_DIR="${CMAKE_SOURCE_DIR}/server/contract/captures")
add_test(NAME auth.shapes COMMAND test_auth_shapes)

# The negotiate request payload, against the pinned OpenAPI snapshot rather than
# against a second copy of the field names. server/contract/romm-openapi-5.2.0.json
# is what M0-4 pinned; this keeps `ClientSaveState` honest against it in both
# directions -- a field the snapshot grew, and a field the encoder invented, are
# both red. No network, so it never skips.
add_executable(test_sync_payload test_sync_payload.cpp)
target_link_libraries(test_sync_payload PRIVATE rommsync::core)
target_include_directories(test_sync_payload PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sync_payload PRIVATE
  ROMMSYNC_OPENAPI="${CMAKE_SOURCE_DIR}/server/contract/romm-openapi-5.2.0.json")
add_test(NAME sync.payload COMMAND test_sync_payload)

# The other direction: the plan RomM sends back, against the captured responses
# in server/contract/captures/ rather than against a second copy of the field
# names. The reason table is the part no capture can pin -- only five of the
# thirteen strings 5.2.0 emits appear in one -- so it is checked against the
# table in docs/API_CONTRACT.md instead. No network, so it never skips.
add_executable(test_sync_plan test_sync_plan.cpp)
target_link_libraries(test_sync_plan PRIVATE rommsync::core)
target_include_directories(test_sync_plan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sync_plan PRIVATE
  ROMMSYNC_CAPTURES_DIR="${CMAKE_SOURCE_DIR}/server/contract/captures")
add_test(NAME sync.plan COMMAND test_sync_plan)

# token.dat. Pure filesystem and parsing, so this never skips either -- and the
# guarantee it exists for (a failed write costs the new token, never the working
# one) is checkable with no server at all.
add_executable(test_token_store test_token_store.cpp)
target_link_libraries(test_token_store PRIVATE rommsync::core)
target_include_directories(test_token_store PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_token_store PRIVATE
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
if(UNIX)
  # "kill the process mid-write" is an acceptance criterion, and forcing it needs
  # fork() and RLIMIT_FSIZE. The host rig is POSIX (docs/TESTING.md); on anything
  # else the scenario fails loudly rather than quietly not running.
  target_compile_definitions(test_token_store PRIVATE ROMMSYNC_CAN_FORK=1)
endif()
add_test(NAME core.token_store COMMAND test_token_store)

# M1-4: what a rejected call means once you count them. No server -- RomM issues
# no refresh token, so there is nothing to refresh and what is left is arithmetic
# over answers -- and it must stay checked with docker stopped, because the
# decision it holds is the one standing between a proxy hiccup and a console sent
# to the pairing screen. The live-server half of that claim is `harness.expired`.
add_executable(test_auth_gate test_auth_gate.cpp)
target_link_libraries(test_auth_gate PRIVATE rommsync::core)
target_include_directories(test_auth_gate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_auth_gate PRIVATE
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")

set(AUTH_GATE_SCENARIOS
  answers   # a status becomes an Answer, and every error enum agrees with it
  counts    # three consecutive rejections is a verdict; one is not
  backoff   # the wait doubles, caps, and is zero while nothing is wrong
  persists  # the verdict round-trips through auth.json, and is cleared
)
foreach(scenario IN LISTS AUTH_GATE_SCENARIOS)
  add_test(NAME auth.gate_${scenario} COMMAND test_auth_gate ${scenario})
  set_tests_properties(auth.gate_${scenario} PROPERTIES TIMEOUT 60)
endforeach()

# The stable client_device_identifier, and the SHA-256 under it. Known-answer
# vectors, a parser and the filesystem, so this never skips either -- and the
# thing it guards (one console, one identifier, across a re-pair) is checkable
# with no server at all.
add_executable(test_device_identity test_device_identity.cpp)
target_link_libraries(test_device_identity PRIVATE rommsync::core)
target_include_directories(test_device_identity PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_device_identity PRIVATE
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_test(NAME core.device_identity COMMAND test_device_identity)

# `DeviceSchema` against the committed capture, and the states the sysmodule
# acts on around it: registered / paired-but-not / unpaired, and whether a
# failure is worth retrying or worth a trip to the pairing screen. Reads one
# capture and the filesystem, so it never skips.
add_executable(test_device_shapes test_device_shapes.cpp)
target_link_libraries(test_device_shapes PRIVATE rommsync::core)
target_include_directories(test_device_shapes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_device_shapes PRIVATE
  ROMMSYNC_CAPTURES_DIR="${CMAKE_SOURCE_DIR}/server/contract/captures"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_test(NAME device.shapes COMMAND test_device_shapes)

# Least privilege, pinned to the document rather than to a second copy of the
# list: the scopes this client asks for are read out of
# docs/API_CONTRACT.md#scopes-to-request and compared. Reads one file, so it
# never skips.
add_executable(test_scopes test_scopes.cpp)
target_link_libraries(test_scopes PRIVATE rommsync::core)
target_include_directories(test_scopes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_scopes PRIVATE
  ROMMSYNC_API_CONTRACT="${CMAKE_SOURCE_DIR}/docs/API_CONTRACT.md")
add_test(NAME auth.scopes COMMAND test_scopes)

# `config.ini`: the defaults, the overrides and the rejections. Reads a file it
# writes itself and docs/CONFIG.md, so it never skips -- and the guarantee it
# checks is that a bad config still boots a working client, which is exactly the
# case a rig could not help with anyway.
add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE rommsync::core)
target_include_directories(test_config PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_config PRIVATE
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  # The documented example is parsed rather than restated: an ```ini block a
  # user copies and the parser rejects is the first thing they meet, and no test
  # of a private string literal here would ever see it.
  ROMMSYNC_CONFIG_DOC="${CMAKE_SOURCE_DIR}/docs/CONFIG.md"
  # ...and the file the release zip actually ships (scripts/package.sh), which
  # is a second copy of that example and can drift from it.
  ROMMSYNC_CONFIG_EXAMPLE="${CMAKE_SOURCE_DIR}/packaging/config.ini.example")
add_test(NAME core.config COMMAND test_config)

# The write half of `config.ini` (M5-3). Pure text in and text out -- no file,
# no server -- because what it has to prove is a claim about *bytes*: the one
# setting changes and every other byte the user typed is the byte it was. A
# fixture on disk would only make the diff harder to read when it fails.
add_executable(test_config_write test_config_write.cpp)
target_link_libraries(test_config_write PRIVATE rommsync::core)
target_include_directories(test_config_write PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set(CONFIG_WRITE_SCENARIOS
  roundtrip  # an edit lands and the rest of the file is byte-identical
  rejects    # a refused edit changes nothing, and never quotes a URL
  absent     # a missing key, and a missing section, both land where they read
  removes    # `remove` drops every occurrence and the default comes back
)
foreach(scenario IN LISTS CONFIG_WRITE_SCENARIOS)
  add_test(NAME config.write_${scenario} COMMAND test_config_write ${scenario})
  set_tests_properties(config.write_${scenario} PROPERTIES TIMEOUT 60)
endforeach()

# MD5, against RFC 1321's own vectors. Pure arithmetic, so it never skips -- and
# it must not: a wrong digest is still 32 plausible hex characters, and the only
# symptom on a console is every unchanged save negotiating as changed forever.
add_executable(test_md5 test_md5.cpp)
target_link_libraries(test_md5 PRIVATE rommsync::core)
target_include_directories(test_md5 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core.md5 COMMAND test_md5)

# SHA-1, against RFC 3174's own vectors. The digest RomM records for a *rom*, so
# a wrong one is 40 plausible hex characters that either refuse every download or
# accept every corrupt one. `vectors` is pure arithmetic and never skips;
# `fixture` streams the 126 MB seeded rom and skips when the library has not been
# fetched, the same way every rig scenario does.
add_executable(test_sha1 test_sha1.cpp)
target_link_libraries(test_sha1 PRIVATE rommsync::core)
target_include_directories(test_sha1 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sha1 PRIVATE
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/server/testing/library")
add_test(NAME core.sha1 COMMAND test_sha1 vectors)
add_test(NAME core.sha1_fixture COMMAND test_sha1 fixture)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(core.sha1_fixture PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(core.sha1_fixture PROPERTIES TIMEOUT 120)

# `state.db`: the format, the commit-window recovery, and the re-hash it skips.
# One temp directory and no network, so it never skips either -- the guarantee
# it checks is that a lost baseline costs a tick of hashing and never a wrong
# comparison, which is a claim about the failures and not about a server.
add_executable(test_state_db test_state_db.cpp)
target_link_libraries(test_state_db PRIVATE rommsync::core)
target_include_directories(test_state_db PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_state_db PRIVATE
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_test(NAME core.state_db COMMAND test_state_db)

# --- the IPC contract: payloads, dispatch table, and ServiceCore -------------
# M5-2. One CTest entry per scenario, same shape as the rest of this file.
#
# Everything but `ipc.engine` needs no server and must stay checked with Docker
# stopped: what those scenarios pin is what a decoder does with a payload written
# by another release -- truncated, over-long, wrong-typed -- and a running RomM
# cannot produce one of those. `ipc.commands` reads docs/DEVELOPMENT.md#ipc so
# the published table and the enum cannot be renumbered apart.
add_executable(test_ipc test_ipc.cpp)
target_link_libraries(test_ipc PRIVATE rommsync::host)
target_include_directories(test_ipc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_ipc PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_DEVELOPMENT_DOC="${CMAKE_SOURCE_DIR}/docs/DEVELOPMENT.md"
  # `ipc.engine` negotiates for real, so it owes the fixture the same session
  # hygiene every other rig test does -- see harness.hpp's CloseOpenSessions and
  # issue #76. That header reads this path.
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")
find_package(Threads REQUIRED)
target_link_libraries(test_ipc PRIVATE Threads::Threads)

set(IPC_UNIT_SCENARIOS
  version    # command 0's encoding is frozen, and this is what freezes it
  commands   # the ids, against the table docs/DEVELOPMENT.md publishes
  roundtrip  # every payload survives its own encoder and decoder, whole
  refuses    # truncated, over-long, wrong-typed: named refusals, no reads past the end
  secrets    # no credential in any payload, and the server URL only where it belongs
  caps       # nothing on this wire grows with the library or with config.ini
  dispatch   # the table, and the decisions ServiceCore makes on the way through it
)
foreach(scenario IN LISTS IPC_UNIT_SCENARIOS)
  add_test(NAME ipc.${scenario} COMMAND test_ipc ${scenario})
  set_tests_properties(ipc.${scenario} PROPERTIES TIMEOUT 120)
endforeach()

# ServiceCore against the live fixture: a real pairing, a real negotiate, and the
# queue commands, driven through Dispatch rather than by calling its methods.
add_test(NAME ipc.engine COMMAND test_ipc engine)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(ipc.engine PROPERTIES SKIP_RETURN_CODE 77)
endif()
# RUN_SERIAL for the reasons pair.* has it: one armed fault proxy for every
# client, and RomM's ten-inits-a-minute limit is per IP, which is the whole suite.
set_tests_properties(ipc.engine PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# --- the three lists the overlay browses -------------------------------------
# M5-4. The producer behind `ipc::ListBegin`/`ListNext`/`ListEnd`, driven through
# `ipc::Dispatch` over a real `SdEngine` -- so what these scenarios assert on is
# a response on the wire rather than a return value, which is the only place the
# payload cap and the field names are visible.
#
# `sysmodule/source/engine.cpp` is compiled in for `engine.*`'s reason: the
# engine names no libnx type, and driving the list through the object that ships
# is worth more than driving the service the object holds.
#
# `caps`, `cursors` and `queue` need no server and must stay checked with Docker
# stopped. The last of those is not incidental: the `queue` kind exists so the
# download screen works with RomM down, and a suite that could only check it
# against a running RomM would never have checked it at all.
add_executable(test_lists test_lists.cpp
  "${CMAKE_SOURCE_DIR}/sysmodule/source/engine.cpp")
target_link_libraries(test_lists PRIVATE rommsync::host)
target_include_directories(test_lists PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/sysmodule/source")
target_compile_definitions(test_lists PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(LIST_UNIT_SCENARIOS
  caps       # nothing on this wire grows with a rom's name, at any page size
  platforms  # an empty list, one shorter than a page, and a rom with no name
  cursors    # the cap, the TTL, and what a reclaimed cursor answers
  queue      # served off the card, and never over the network
)
foreach(scenario IN LISTS LIST_UNIT_SCENARIOS)
  add_test(NAME lists.${scenario} COMMAND test_lists ${scenario})
  set_tests_properties(lists.${scenario} PROPERTIES TIMEOUT 120)
endforeach()

# The two that need this worktree's RomM: paging the seeded library, and the
# three ways a page fails. RUN_SERIAL for the reason every fault test has it --
# one armed proxy for every client in the suite.
set(LIST_RIG_SCENARIOS
  library  # every rom exactly once, in a stable order, with the count RomM reports
  faults   # stall, drop and truncate: a failed page, and a cursor that survives it
)
foreach(scenario IN LISTS LIST_RIG_SCENARIOS)
  add_test(NAME lists.${scenario} COMMAND test_lists ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(lists.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  set_tests_properties(lists.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# --- the overlay's status screen ---------------------------------------------
# M4-1. Everything about the screen that is a decision rather than a draw call --
# which sentence a never-paired console gets, what a download with no declared
# length shows instead of a percentage, what goes where a timestamp would go on a
# console that has never synced. The drawing itself waits for M8-2 (#44); none of
# this does, and none of it needs a console.
add_executable(test_overlay_status test_overlay_status.cpp)
target_link_libraries(test_overlay_status PRIVATE rommsync::core)
target_include_directories(test_overlay_status PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME overlay.status COMMAND test_overlay_status)

# --- the overlay's pairing screen --------------------------------------------
# M4-5. The same split, for the same reason: which of four sentences a dead
# pairing gets, whether a code that ran out of time is still put in front of a
# human, and that the eight characters are drawn exactly as RomM issued them.
# The two source paths are compiled in because one criterion is about the code
# rather than about its behaviour -- the view model may not name a
# `PairingSession` method, and a grep is the only thing that can say so.
add_executable(test_overlay_pairing test_overlay_pairing.cpp)
target_link_libraries(test_overlay_pairing PRIVATE rommsync::core)
target_include_directories(test_overlay_pairing PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_overlay_pairing PRIVATE
  ROMMSYNC_PAIRING_VIEW_HDR="${CMAKE_SOURCE_DIR}/core/include/rommsync/overlay_pairing_view.hpp"
  ROMMSYNC_PAIRING_VIEW_SRC="${CMAKE_SOURCE_DIR}/core/src/overlay_pairing_view.cpp")
add_test(NAME overlay.pairing COMMAND test_overlay_pairing)

# --- the overlay's sync screen -----------------------------------------------
# M4-2. The two controls the overlay exists for, and the one thing about them
# that is a decision: which control may be pressed. The screen greys "Sync now"
# by predicting what `SyncNow` would answer, so the suite holds `PredictSyncNow`
# against a real `ipc::ServiceCore` over a fake `Engine` rather than against a
# second copy of the table -- a prediction one branch out of step is a button
# that refuses a sync the console would have run.
#
# The overlay's source directory is compiled in because one criterion is about
# the code rather than about its behaviour: nothing under `overlay/` may write
# `config.ini` or name a boot flag, and only a grep can say so.
add_executable(test_overlay_sync test_overlay_sync.cpp)
target_link_libraries(test_overlay_sync PRIVATE rommsync::core)
target_include_directories(test_overlay_sync PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_overlay_sync PRIVATE
  ROMMSYNC_OVERLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}/overlay/source")
add_test(NAME overlay.sync_actions COMMAND test_overlay_sync)

# --- the overlay's library browser -------------------------------------------
# M4-3. The one screen with state: a cursor stack, a set of loaded pages, a
# selection and a per-row answer to a press, spread across as many IPC round
# trips as the user scrolls. `LibraryBrowserModel` calls nothing -- it asks for
# one command at a time and is told what came back -- which is what lets this
# suite drive it through paging cases a console would need a ten-thousand-rom
# library to reproduce: an exact page boundary, an empty page, a `pending` one,
# a page that fails mid-scroll, and a cursor the sysmodule reclaimed.
#
# The pages go through `AppendIfItFits` and the real codec rather than being
# hand-filled: the model reads its fields *by name* (`ipc::list_keys`), so a
# suite that skipped the wire would agree with itself while a real page decoded
# to empty rows.
#
# The overlay's source directory and the view model's two paths are compiled in
# because three criteria are about the code rather than about its behaviour: the
# screen may not write `config.ini`, it may not call an engine, and the model
# may not name a libnx or libultrahand type. Only a grep can say so.
add_executable(test_overlay_library test_overlay_library.cpp)
target_link_libraries(test_overlay_library PRIVATE rommsync::core)
target_include_directories(test_overlay_library PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_overlay_library PRIVATE
  ROMMSYNC_OVERLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}/overlay/source"
  ROMMSYNC_LIBRARY_MODEL_HDR="${CMAKE_SOURCE_DIR}/core/include/rommsync/overlay_library_model.hpp"
  ROMMSYNC_LIBRARY_MODEL_SRC="${CMAKE_SOURCE_DIR}/core/src/overlay_library_model.cpp")
add_test(NAME overlay.library COMMAND test_overlay_library)

# --- the overlay's settings screen -------------------------------------------
# M4-4. The whole effective configuration on a console with no keyboard, and
# every complaint the parser had about it -- which is the point of the screen:
# `config.ini` is edited blind, and "line 9 [sync] states: expected true or
# false" is the difference between a fix and an evening.
#
# The configurations go through `config::ParseConfig` and then through the real
# `EncodeConfigView`/`DecodeConfigView`, for the reason `overlay.library`
# round-trips its pages: the screen renders what the wire carried, and a suite
# that skipped the codec would agree with itself while a console drew blanks.
#
# The overlay's source directory and the view model's two paths are compiled in
# because three criteria are about the code rather than about its behaviour: the
# screen may not write `config.ini`, it may not call an engine -- it is the one
# screen that renders `config::` types, so `LoadConfig` is one include away --
# and the view model may not name a libnx or libultrahand type.
add_executable(test_overlay_settings test_overlay_settings.cpp)
target_link_libraries(test_overlay_settings PRIVATE rommsync::core)
target_include_directories(test_overlay_settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_overlay_settings PRIVATE
  ROMMSYNC_OVERLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}/overlay/source"
  ROMMSYNC_SETTINGS_VIEW_HDR="${CMAKE_SOURCE_DIR}/core/include/rommsync/overlay_settings_view.hpp"
  ROMMSYNC_SETTINGS_VIEW_SRC="${CMAKE_SOURCE_DIR}/core/src/overlay_settings_view.cpp")
add_test(NAME overlay.settings COMMAND test_overlay_settings)

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
  progress          # ...reporting how far it has got while it is still going
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
  redirect_credentials # a bearer token does not follow a redirect off this origin
  multipart_grows   # a save still being written does not overrun its Content-Length
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

# --- the console's HttpClient against the same rig ---------------------------
# M1-7 (#126). The Horizon backend is split so that the half that can be got
# wrong is reachable from here: `sysmodule/source/http/http_wire.cpp` speaks
# HTTP/1.1 over an abstract `Connection` and names no libnx type, so
# `sysmodule/AGENTS.md`'s rule applies to it the way it does to `engine.cpp`.
# `posix_connection.cpp` is compiled in beside it for the same reason and is not
# a test double: it is the plain-TCP connector the console itself uses for an
# `http://` origin, so what runs here is the shipping code minus the one layer
# nothing off a console can execute -- `ssl_http_client.cpp`, which the M8-1
# gate (#43) owns and CMake does not build at all.
#
# The scenarios are `test_http_native.cpp`'s, compiled a second time with a
# different backend rather than copied, so the two clients cannot drift on what
# `http.hpp` promises. `wire.*` and `http.*` are therefore the same eighteen
# assertions against the same RomM.
add_executable(test_http_wire test_http_native.cpp
  "${CMAKE_SOURCE_DIR}/sysmodule/source/http/http_wire.cpp"
  "${CMAKE_SOURCE_DIR}/sysmodule/source/http/posix_connection.cpp")
target_link_libraries(test_http_wire PRIVATE rommsync::host Threads::Threads)
target_include_directories(test_http_wire PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/sysmodule/source")
target_compile_definitions(test_http_wire PRIVATE
  ROMMSYNC_TEST_WIRE_BACKEND
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch-wire")

foreach(scenario IN LISTS HTTP_SCENARIOS)
  add_test(NAME wire.${scenario} COMMAND test_http_wire ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(wire.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  set_tests_properties(wire.${scenario} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()

# --- the device-code pairing flow against the real rig -----------------------
# Same shape as the http.* tests: one CTest entry per scenario, so a red run
# names the behaviour. Every code is one RomM issued, and the approvals go
# through RomM's own /api/auth/device/approve -- the endpoint that lets a test
# be the human the grant assumes (docs/TESTING.md).
add_executable(test_pairing test_pairing.cpp)
target_link_libraries(test_pairing PRIVATE rommsync::host)
target_compile_definitions(test_pairing PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")
# `starting` drives Begin() on one thread and status() on another, which is the
# threading contract the header promises the overlay.
target_link_libraries(test_pairing PRIVATE Threads::Threads)

set(PAIR_SCENARIOS
  happy         # init -> approve -> token -> persisted
  starting      # status() answers, and says "starting", while init is in flight
  mid_poll      # approval that lands after several polls, at the server's interval
  denied        # a human refusing it, and the re-pair that follows
  repair        # re-pair with the same identifier: one device in RomM, not two
  expired       # our own deadline, and a code the server considers spent
  retry         # 5xx mid-poll: back off, do not abandon a live code
  unauthorized  # a 401 from something in front of RomM: retry, then diagnose
  rejection_streak # ...and the budget counts consecutive ones, not lifetime ones
  stall         # a poll that hangs does not wedge the state machine
  drop          # ...nor does one whose connection dies
  lost_grant    # the grant that never got home: the code is spent, so stop
  payload       # the IPC payload the overlay decodes, and what it must not carry
)

foreach(scenario IN LISTS PAIR_SCENARIOS)
  add_test(NAME pair.${scenario} COMMAND test_pairing ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(pair.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the same reason as http.*: the fault proxy holds one armed
  # scenario for every client. These also wait out real poll intervals, so the
  # timeout has to cover a few of them.
  set_tests_properties(pair.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# --- registering the console against the real rig ----------------------------
# Same shape as pair.*: one CTest entry per scenario. These assert on what the
# SERVER holds -- they list /api/devices as the fixture user and count the rows
# carrying this console's identifier -- because "pairing twice leaves one device,
# not two" is a claim about RomM that no amount of client-side testing can check.
# `never_post` holds the finding the client is built on: POST /api/devices never
# matches on client_device_identifier, so it always creates a duplicate.
add_executable(test_device_registration test_device_registration.cpp)
target_link_libraries(test_device_registration PRIVATE rommsync::host)
target_compile_definitions(test_device_registration PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch")

set(DEVICE_SCENARIOS
  registered     # a fresh pairing registers one device, and the id it cached is it
  repair         # pairing again is the same device: the list does not grow
  recovers       # a token with no device id finds its device rather than making one
  deleted        # the device removed in RomM's UI, caught before a sync scopes by it
  revoked        # a 401 stays a 401 and does not become "no such device"
  forbidden      # ...and a 403 is a missing scope, never reported as a revocation
  sync_disabled  # the user's own switch, reported as itself
  offline        # a dropped connection is not a verdict on the pairing
  impostor       # a 200 about a different device confirms nothing
  never_post     # POST /api/devices duplicates, whatever it is given
)

foreach(scenario IN LISTS DEVICE_SCENARIOS)
  add_test(NAME device.${scenario} COMMAND test_device_registration ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(device.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the same reasons as pair.*: one armed fault proxy for every
  # client, and RomM's ten-inits-a-minute limit is per IP, which is the whole
  # suite. These also mutate the fixture's device list, so two at once would
  # count each other's rows.
  set_tests_properties(device.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# --- the negotiate payload against the real rig ------------------------------
# sync.payload pins ClientSaveState to the committed snapshot; these ask the
# running server, which is the only thing that can show what a *misnamed*
# optional field costs -- RomM answers 200 and plans something else. One CTest
# entry per scenario, so a red run names the behaviour.
add_executable(test_sync_negotiate test_sync_negotiate.cpp)
target_link_libraries(test_sync_negotiate PRIVATE rommsync::host)
target_include_directories(test_sync_negotiate PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sync_negotiate PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(SYNC_SCENARIOS
  accepted    # a live 5.2.0 takes the encoded body and echoes the entry back
  required    # a renamed REQUIRED field is a 422 that names it
  understood  # a renamed OPTIONAL one is a silent 200 with a different plan
  negotiates  # sync::Negotiate end to end: a session and an operation, typed
  discovers   # an empty `saves` is how a client learns what it is missing
  revoked     # 401 is a revoked token, not a parse failure, and nothing changes
  truncated   # a clean short body is a named json error and no plan
  stalled     # a stall times out and is retried, with the backoff doubling
  refused     # 404 device-gone, 400 sync-disabled, and the 404 that is neither
)

foreach(scenario IN LISTS SYNC_SCENARIOS)
  add_test(NAME sync.${scenario} COMMAND test_sync_negotiate ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(sync.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # `understood` uploads a throwaway save and deletes it again, so it must not
  # interleave with another test reading the same library -- and every scenario
  # here goes through the fault proxy, which holds one armed scenario for all
  # clients.
  set_tests_properties(sync.${scenario} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()

# --- step 2: executing the plan ----------------------------------------------
# M2-5, the issue that carries the project's one hard guarantee. Every rig
# scenario runs `sync::ExecutePlan` inside a `harness::Sandbox`, so the
# backup-before-overwrite audit judges each one on teardown.
#
# `naming`, `staging` and `counting` are deliberately NOT in the skippable list:
# the backup path that used to collide, the two `io` primitives an overwrite
# goes through, and the counts M2-6 reports to `complete` need no server, so
# they must stay checked with docker stopped.
add_executable(test_sync_execute test_sync_execute.cpp)
target_link_libraries(test_sync_execute PRIVATE rommsync::host)
target_include_directories(test_sync_execute PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sync_execute PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(EXECUTE_UNIT_SCENARIOS
  naming   # the backup path carries the slot, so one rom's two saves are two files
  staging  # the streamed copy and the two-rename commit an overwrite goes through
  counting # no-op, a downgraded action, and the two refusals that need no server
)
foreach(scenario IN LISTS EXECUTE_UNIT_SCENARIOS)
  add_test(NAME execute.${scenario} COMMAND test_sync_execute ${scenario})
  set_tests_properties(execute.${scenario} PROPERTIES TIMEOUT 60)
endforeach()


set(EXECUTE_RIG_SCENARIOS
  upload          # overwrite=true, session_id and device_id -- and no 409
  download        # verify, back up, commit, confirm -- in that order
  conflict        # both sides changed: keep both, lose nothing
  same_timestamp  # ...and the no-history conflict, resolved the same way
  truncate        # a clean short body never replaces a save
  corrupted       # ...and neither do the right number of the wrong bytes
  occupied        # overwrite=true, in the state it exists for: a 409 without it
  dropped         # a TCP reset mid-download costs a tick, not a save
  collision       # two saves of one rom, one second, two backups
  revoked         # M1-4: a 401 stops the plan where it stands, and touches no save
)
foreach(scenario IN LISTS EXECUTE_RIG_SCENARIOS)
  add_test(NAME execute.${scenario} COMMAND test_sync_execute ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(execute.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the reason every rig test needs it: the fault proxy holds one
  # armed scenario for all clients, and these upload and delete saves on the
  # shared fixture device.
  set_tests_properties(execute.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()
# execute.occupied is intermittently red and the cause is NOT understood (#85).
#
# It sees `overwrite=true` add a second save row rather than move the occupying
# one forward, and comes back with an id naming the occupant's bytes instead of
# the uploaded ones -- 41 bytes where 36 were expected. The issue records eight
# consecutive passes afterwards, alone and in groups, so it is a race rather
# than a regression, and it is currently what makes `main` red.
#
# Same QUARANTINE, same terms, as harness.partial below: retried, named, and
# with the issue on it.
#
# It does NOT come out when #85 closes -- #85 is closed, and this is still here.
# It comes out when someone measures a rate rather than sampling one: #119 is
# that experiment (`--repeat until-fail:50`, proxy unfixed then fixed). Three
# green runs cannot tell "fixed" from "under 1 in 3", which is exactly what a
# retry hides. Green with the retry is
# not evidence the race is gone -- green WITHOUT it is.
set_tests_properties(execute.occupied PROPERTIES REPEAT UNTIL_PASS:3)

# --- step 0: the local scan and its rom matching -----------------------------
# M2-2. The first four scenarios need no server and must stay checked with
# docker stopped: what they pin is the decision that an ambiguous name is
# skipped rather than guessed, and a wrong match is not a failed tick -- it is
# one game's save uploaded over another game's. `library` and `paging` need the
# real RomM, because the envelope `GET /api/roms` answers and the fields it
# sends are the two things a fake index cannot be wrong about.
add_executable(test_save_scan test_save_scan.cpp)
target_link_libraries(test_save_scan PRIVATE rommsync::host)
target_include_directories(test_save_scan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_save_scan PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(SCAN_UNIT_SCENARIOS
  names        # base names, emulators, slots, and which folder implies a platform
  ambiguous    # one name on two platforms, in a hintless folder, is skipped
  walked_once  # RetroArch's flat saves/, listed under a dozen platforms
  unusable     # a record sync::Validate refuses costs one file, not the tick
  truncated    # a directory past kMaxDirectoryEntries keeps a set that does not move
  hashing      # the digest is real, reused when nothing changed, null only when unreadable
)
foreach(scenario IN LISTS SCAN_UNIT_SCENARIOS)
  add_test(NAME scan.${scenario} COMMAND test_save_scan ${scenario})
  set_tests_properties(scan.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

set(SCAN_RIG_SCENARIOS
  library  # a Sandbox scanned against the docker library, end to end
  paging   # limit=1 over the seeded library still finds a rom on the last page
)
foreach(scenario IN LISTS SCAN_RIG_SCENARIOS)
  add_test(NAME scan.${scenario} COMMAND test_save_scan ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(scan.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # Every rig test goes through the fault proxy, which holds one armed scenario
  # for all clients.
  set_tests_properties(scan.${scenario} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endforeach()

# --- M3-1: a rom turned into the path it downloads to ------------------------
# The folder map is `core.config`'s; this is the join that hangs off it. Five
# scenarios need no server and must stay checked with docker stopped, because
# what they pin is a refusal -- a `fs_name` is a name off the *server's* disk,
# and one carrying `../..` that this code resolved rather than refused would be
# a write outside the mapped folder, next door to `atmosphere/`. `library` needs
# the real RomM, because the one thing a literal here cannot be right about is
# which field the map is keyed on: `platform_slug` and `platform_fs_slug` read
# the same on the seeded library and only one of them is the directory name.
add_executable(test_rom_path test_rom_path.cpp)
target_link_libraries(test_rom_path PRIVATE rommsync::host)
target_include_directories(test_rom_path PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_rom_path PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(ROM_UNIT_SCENARIOS
  defaults  # the built-in map, and that the key is the fs slug
  override  # a [platform.gba] section moves the destination with it
  unmapped  # a skip with a reason, never a guessed folder
  hostile   # separators, '..', control characters, NUL, 800 characters
  existing  # "already on the card?" reads every roms entry, writes the first
)
foreach(scenario IN LISTS ROM_UNIT_SCENARIOS)
  add_test(NAME rom.${scenario} COMMAND test_rom_path ${scenario})
  set_tests_properties(rom.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

add_test(NAME rom.library COMMAND test_rom_path library)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(rom.library PROPERTIES SKIP_RETURN_CODE 77)
endif()
# Read-only against the shared library, but it goes through the fault proxy,
# which holds one armed scenario for all clients.
set_tests_properties(rom.library PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# --- the conflict history, the restore, and the screen over them -------------
# M7-1 (#36). Six scenarios need no server and must stay checked with docker
# stopped: what they pin is the file format a yanked card leaves behind, the two
# recorders, a restore that owes hard rule 2 the same backup a download does, and
# the screen's decisions. A healthy RomM cannot produce a truncated `conflicts.db`
# and cannot be asked to kill a process mid-write.
#
# `conflicts.recorded` is the one that needs the rig, because the acceptance
# criterion is a *real* conflict producing an entry with both hashes and a backup
# path that exists -- a hand-built plan proves the recorder, not the contract.
#
# `sysmodule/source/engine.cpp` is compiled in for `lists.*`'s reason: the engine
# names no libnx type, and driving the two commands through the object that ships
# is worth more than driving the store the object holds.
#
# The view model's two paths and the overlay source directory are compiled in
# because four criteria are about the code rather than about its behaviour: the
# screen may not write `config.ini`, it may not restore a backup itself, the view
# model may not name a libnx or libultrahand type, and nothing in the recording
# path may read `conflict_show`.
add_executable(test_conflicts test_conflicts.cpp
  "${CMAKE_SOURCE_DIR}/sysmodule/source/engine.cpp")
target_link_libraries(test_conflicts PRIVATE rommsync::host)
target_include_directories(test_conflicts PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/sysmodule/source")
target_compile_definitions(test_conflicts PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env"
  ROMMSYNC_OVERLAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}/overlay/source"
  ROMMSYNC_CONFLICT_LOG_HDR="${CMAKE_SOURCE_DIR}/core/include/rommsync/conflict_log.hpp"
  ROMMSYNC_CONFLICT_LOG_SRC="${CMAKE_SOURCE_DIR}/core/src/conflict_log.cpp"
  ROMMSYNC_CONFLICT_RECORD_HDR="${CMAKE_SOURCE_DIR}/core/include/rommsync/conflict_record.hpp"
  ROMMSYNC_CONFLICT_RECORD_SRC="${CMAKE_SOURCE_DIR}/core/src/conflict_record.cpp"
  ROMMSYNC_CONFLICTS_VIEW_HDR="${CMAKE_SOURCE_DIR}/core/include/rommsync/overlay_conflicts_view.hpp"
  ROMMSYNC_CONFLICTS_VIEW_SRC="${CMAKE_SOURCE_DIR}/core/src/overlay_conflicts_view.cpp")
if(UNIX)
  # "kill the process mid-restore" is an acceptance criterion, and forcing it
  # needs fork() and RLIMIT_FSIZE -- `core.token_store`'s reason, and its
  # mechanism. The host rig is POSIX (docs/TESTING.md); on anything else the
  # scenario fails loudly rather than quietly not running.
  target_compile_definitions(test_conflicts PRIVATE ROMMSYNC_CAN_FORK=1)
endif()

set(CONFLICT_UNIT_SCENARIOS
  store        # the file: round-trip, bounded, survives a reboot, one bad row costs one row
  record       # a tick's reports become entries -- both conflict reasons, and the state kinds
  restore      # the old bytes exactly, and a new backup written FIRST (the sandbox audit checks it)
  interrupted  # a restore killed mid-write leaves old or new, never a splice, and no .tmp
  service      # both commands through ipc::Dispatch over a real SdEngine
  overlay      # the screen's decisions, and the greps overlay/AGENTS.md owes
)
foreach(scenario IN LISTS CONFLICT_UNIT_SCENARIOS)
  add_test(NAME conflicts.${scenario} COMMAND test_conflicts ${scenario})
  set_tests_properties(conflicts.${scenario} PROPERTIES TIMEOUT 120)
endforeach()

add_test(NAME conflicts.recorded COMMAND test_conflicts recorded)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(conflicts.recorded PROPERTIES SKIP_RETURN_CODE 77)
endif()
# RUN_SERIAL for the reason every rig test needs it: the fault proxy holds one
# armed scenario for all clients, and this uploads and deletes saves on the
# shared fixture device.
set_tests_properties(conflicts.recorded PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

# --- the host harness, and the edge cases only the fault proxy can force -----
# M0-5. One CTest entry per scenario, same shape as the rest of this file, so a
# red run names the behaviour rather than "the harness tests".
#
# `harness.sandbox` is deliberately NOT in the skippable list: it is the
# harness's own guarantee -- a per-test SD card that is torn down, and the
# backup-before-overwrite audit built into it -- and it needs no server, so it
# must stay checked with Docker stopped. That is exactly when a change to the
# sandbox is most likely to go in unnoticed.
add_executable(test_harness test_harness.cpp)
target_link_libraries(test_harness PRIVATE rommsync::host)
target_include_directories(test_harness PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_harness PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env"
  # The seeded library RomM serves, so a resumed 120 MiB download is compared
  # against the bytes on the other side of the proxy rather than a hash of them.
  ROMMSYNC_FIXTURE_LIBRARY="${CMAKE_SOURCE_DIR}/server/testing/library")

add_test(NAME harness.sandbox COMMAND test_harness sandbox)
set_tests_properties(harness.sandbox PROPERTIES TIMEOUT 60)

set(HARNESS_SCENARIOS
  disarms         # an armed fault cannot outlive the scope that armed it
  expired         # 401 mid-flow: a response, not a transport error, and not a verdict
  conflict        # both sides moved past the last sync
  same_timestamp  # ...and the no-history conflict a switch on one reason drops
  partial         # the Nth operation fails; the session records it accurately
  resume          # a TCP reset mid-rom, then Range resume, byte for byte
  truncate        # a clean short body, caught only by the caller's expected size
  stall           # a server that says nothing: one tick lost, nothing else
  stall_dropped   # ...and the stalled request never reaches RomM at all (#109)
  multifile       # the two-disc fixture, its traps, and the nested rom beside it
  backup          # back up FIRST, proven by interrupting the overwrite
  content_hash    # the MD5 this client computes is the one RomM compares against
)

foreach(scenario IN LISTS HARNESS_SCENARIOS)
  add_test(NAME harness.${scenario} COMMAND test_harness ${scenario})

  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(harness.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the same reason as http.* and pair.*: the fault proxy holds
  # one armed scenario for every client. These also upload and delete saves on
  # the shared fixture, so two at once would read each other's library.
  set_tests_properties(harness.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# --- M2-8: save states, which the server does not arbitrate ------------------
# The other M2 suites can lean on RomM to be right -- negotiate hands back an
# `action` and the client obeys. There is no negotiate for a state, so what these
# pin is the client's own policy, and the failure it is written against is the
# one an obvious implementation makes: `POST /api/states` upserts on
# `(rom_id, file_name)`, so posting a local state over a name the server already
# holds destroys somebody's session and RomM answers 200.
#
# The eight unit scenarios are deliberately NOT skippable. `silent` is the
# off-by-default promise -- no directory read, no request sent -- and it is the
# one that has to stay checked with docker stopped, because a console reading
# state folders the user never opted into is silent in every other way. `naming`
# is the backup path a state has no slot to build from, `scanning` is the
# ambiguity and duplicate-name rules, and `decides` is the whole decision table
# including every keep-both branch. `trims`, `placing`, `listing` and
# `recording` came out of the self-review and need no server either: the shared
# baseline budget, a state this console lacks being created rather than written
# over, a listing the client cannot work from stopping the run, and an upload's
# row coming from the scan.
add_executable(test_state_sync test_state_sync.cpp)
target_link_libraries(test_state_sync PRIVATE rommsync::host)
target_include_directories(test_state_sync PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_state_sync PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(STATES_UNIT_SCENARIOS
  silent    # sync.states = false reads no directory and sends no request
  naming    # a state has no slot, so its backup name is built from its own name
  scanning  # an ambiguous state is skipped, and two of one name are one state
  decides   # the decision table, keep-both branches included
  trims     # saves and states share one baseline, and states are what gives
  placing   # a state this console lacks is created, never written over
  listing   # a listing this client cannot work from stops the run
  recording # an upload's row comes from the scan, and the next tick is a no-op
)
foreach(scenario IN LISTS STATES_UNIT_SCENARIOS)
  add_test(NAME states.${scenario} COMMAND test_state_sync ${scenario})
  set_tests_properties(states.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

set(STATES_RIG_SCENARIOS
  upsert     # POST /api/states replaces the row with that name, in place
  roundtrip  # local -> server -> a second sandbox -> identical bytes
  overwrite  # the server's copy moved: back up first, then replace
  truncate   # a clean short body never replaces a local state
  keeps_both # both sides moved: nothing is transferred, both survive
)
foreach(scenario IN LISTS STATES_RIG_SCENARIOS)
  add_test(NAME states.${scenario} COMMAND test_state_sync ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(states.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the reason every rig test has it: the fault proxy holds one
  # armed scenario for every client, and these upload and delete states on the
  # shared fixture, so two at once would read each other's listing -- which for
  # states is the whole input to the decision.
  set_tests_properties(states.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# --- step 3: completing the session and committing the baseline --------------
# M2-6. `complete` is accounting rather than a commit point -- the transfers
# already landed on the server -- so what these prove is that the counts are
# honest and that the baseline survives a completion that does not.
#
# `counts`, `parse`, `stamps` and `advance` are deliberately NOT in the skippable
# list: the accounting, the response read against the committed capture, the
# timestamp reader a row's server half goes through, and the rules about which
# operations may move a row all need no server, so they must stay checked with
# docker stopped.
add_executable(test_sync_complete test_sync_complete.cpp)
target_link_libraries(test_sync_complete PRIVATE rommsync::host)
target_include_directories(test_sync_complete PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sync_complete PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_CAPTURES_DIR="${CMAKE_SOURCE_DIR}/server/contract/captures"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(COMPLETE_UNIT_SCENARIOS
  counts   # an ExecutionReport becomes two integers, not_understood included
  parse    # the captured SyncCompleteResponse, and the bodies that are not one
  stamps   # the RFC 3339 a baseline row's server_updated_at is read from
  advance  # which outcomes move a baseline row, and which keep the previous one
)
foreach(scenario IN LISTS COMPLETE_UNIT_SCENARIOS)
  add_test(NAME complete.${scenario} COMMAND test_sync_complete ${scenario})
  set_tests_properties(complete.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

set(COMPLETE_RIG_SCENARIOS
  session    # negotiate -> execute -> complete, and the baseline afterwards
  unchanged  # a second tick plans no work, and is planned 0 without being an error
  reported   # a partly failed plan sends the real counts; the failed row stands
  revoked    # M1-4: a 401 mid-tick ends it cleanly, and the baseline survives
  refused    # a 500 on complete loses the tick, never the baseline
  truncated  # a clean short body is a named parse error, not a half-read session
)
foreach(scenario IN LISTS COMPLETE_RIG_SCENARIOS)
  add_test(NAME complete.${scenario} COMMAND test_sync_complete ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(complete.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the reasons every rig test here has: one armed fault proxy for
  # every client, and RomM keeps ONE active sync session per device -- two of
  # these at once would cancel each other's sessions (#76).
  set_tests_properties(complete.${scenario} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endforeach()

# --- M7-2: when a tick runs ---------------------------------------------------
# The scheduler and the one backoff curve it shares with `auth::PairingSession`.
# Every scenario drives an injected steady clock and an injected wall clock, so
# an interval, a suspend and a day of retries are arithmetic rather than a wait.
#
# **None of these may ever need a server**, and that is the point rather than a
# convenience: what they pin is that the schedule does *not* fire -- a claim
# about requests that were never made, which no running RomM can witness.
# `sched.idle` is the battery one and `sched.suspend` the one a console produces
# several times a day.
add_executable(test_scheduler test_scheduler.cpp)
target_link_libraries(test_scheduler PRIVATE rommsync::core)
target_include_directories(test_scheduler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set(SCHEDULER_SCENARIOS
  backoff   # doubles, caps, jitters, and never falls below the floor
  interval  # exactly one tick per interval, whatever the poll rate
  ondemand  # interval_min = 0: boot and Sync now, and no timer
  disabled  # the switch: never fires, and refuses a request rather than queueing it
  suspend   # a clock advanced past several intervals is ONE catch-up tick
  skew      # a wall clock corrected in either direction: no storm, no stall
  failures  # backoff after a failed tick, floor again after a success
  tls       # a handshake failure is capped rather than retried forever
  idle      # a day switched off costs no wakeups; a day offline costs tens of ticks
)
foreach(scenario IN LISTS SCHEDULER_SCENARIOS)
  add_test(NAME sched.${scenario} COMMAND test_scheduler ${scenario})
  set_tests_properties(sched.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

# --- M2-7: an interrupted tick ------------------------------------------------
# The stages are individually safe since M2-4/5/6; what these check is what is
# left on the card *between* them. Every rig scenario runs `sync::RunTick` inside
# a `harness::Sandbox`, so the backup-before-overwrite audit judges it on
# teardown -- which is the whole reason the fault proxy exists, because the
# guarantee only ever matters on the failure path.
#
# `recovery`, `durable`, `backupdir`, `offline`, `rescan` and `canceled` are
# deliberately NOT in the skippable list: the sweep a crash makes necessary, the
# platform fsync hard rule 2 depends on, the directory `core/` could not create,
# the tick that must write nothing at all, the restored save that must not be
# negotiated without, and the shutdown that must cost no request all need no
# server, so they must stay checked with docker stopped.
add_executable(test_sync_tick test_sync_tick.cpp)
target_link_libraries(test_sync_tick PRIVATE rommsync::host)
target_include_directories(test_sync_tick PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_sync_tick PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")

set(TICK_UNIT_SCENARIOS
  recovery   # the .part / .tmp / .old a crash leaves, and the backup it must not touch
  durable    # the backup's bytes are on the card before the rename publishes them
  backupdir  # .backup/ made on a first boot, which core/ could not do until now
  offline    # an unreachable server: the card is byte-for-byte what it was
  rescan     # a restored save stops the tick rather than negotiating without it
  canceled   # a token already fired ends the tick without a single request
)

# M6-2 (#33). These are the same executables, registered under `toggle.` because
# what they check is one issue's acceptance rather than one module's: the
# sysmodule under ovl-sysmodules' two switches. `ctest -R toggle` is the whole of
# it, and docs/DEVELOPMENT.md#the-two-switches says so.
add_test(NAME toggle.disabled COMMAND test_sync_tick disabled)
set_tests_properties(toggle.disabled PROPERTIES TIMEOUT 60)
foreach(scenario IN LISTS TICK_UNIT_SCENARIOS)
  add_test(NAME tick.${scenario} COMMAND test_sync_tick ${scenario})
  set_tests_properties(tick.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

# One entry per (stage, fault mode), so a red run names the stage and the way it
# broke rather than "the tick".
set(TICK_RIG_SCENARIOS
  negotiate_5xx    # a 5xx past the retry budget: nothing is written at all
  negotiate_drop   # ...and neither is anything by a reset mid-plan
  upload_5xx       # refused before RomM saw it: no row, no save touched
  upload_truncate  # a short 200 is a *completed* upload whose save_id is unknown
  upload_drop      # the row exists and the client cannot know -- and must not re-post
  upload_stall     # a stall never reaches RomM, and the next tick runs normally
  download_5xx     # a refusal never replaces a save
  download_stall   # ...and neither does a transfer that stopped moving; next tick recovers
  download_401     # M1-4's stop, met at a download rather than at an upload
  complete_drop    # the accounting is lost and the baseline is not
  complete_stall   # the same on the timeout, and the next tick finds no work to redo
  resumes          # a reset mid-download, then a second tick: one save, one row
)

# M7-2 (#37), registered under `sched.` because what they check is the schedule
# around a tick rather than the tick. They live in this executable because the
# two acceptance cases they cover cannot be produced with an injected outcome: a
# tick has to really fail on the stall timeout, and a connection has to really be
# reset, before "reschedules with backoff rather than spinning" means anything.
set(SCHEDULER_RIG_SCENARIOS
  sched_stall  # a stalled negotiation: backoff, no spin, and the next tick recovers
  sched_drop   # a reset mid-plan: transient, nothing written, one row afterwards
)
foreach(scenario IN LISTS SCHEDULER_RIG_SCENARIOS)
  string(REPLACE "sched_" "" name "${scenario}")
  add_test(NAME sched.${name} COMMAND test_sync_tick ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(sched.${name} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  set_tests_properties(sched.${name} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()
# M6-2 (#33), and a rig scenario for the same reason `resumes` is one: the kill
# has to happen to a real transfer. `resumes` interrupts a tick; this interrupts
# the *process*, throwing away the client, the filesystem and the baseline before
# the second tick, which is what `pmshellTerminateProgram` does.
add_test(NAME toggle.tick COMMAND test_sync_tick relaunched)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(toggle.tick PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(toggle.tick PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)

foreach(scenario IN LISTS TICK_RIG_SCENARIOS)
  add_test(NAME tick.${scenario} COMMAND test_sync_tick ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(tick.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the two reasons every rig scenario here has: the fault proxy
  # holds ONE armed scenario for all clients, and RomM keeps ONE active sync
  # session per device -- two of these at once would cancel each other's
  # sessions (#76).
  set_tests_properties(tick.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

# harness.partial is intermittently red and the cause is NOT understood yet.
#
# It fails when a SECOND sync session appears mid-scenario and RomM cancels the
# one the test opened -- planned 3, completed 0, killed a second after it was
# created. The diagnostics in the test print every session the device has, and
# the evidence is on issue #76. What creates that second session is still open;
# the leading theory is that a device-attributed save upload opens one, and
# `partial` does three of those inside its own session.
#
# So this is a QUARANTINE, not a fix: retry it rather than leave `main` red on a
# race nobody has explained. It is deliberately narrow -- one test, named, with
# the issue on it -- because a repo that retries tests by default has stopped
# being able to tell flaky from broken.
#
# Do NOT remove this because #76 is closed. It is closed, and the retry is still
# here: closing the issue did not measure anything. #119 is the experiment that
# would justify removing it, and #109 -- the fault proxy replaying a stalled
# request into a later test -- is a real defect that was fixed WITHOUT being
# shown to cause this one.
set_tests_properties(harness.partial PROPERTIES REPEAT UNTIL_PASS:3)

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
# new worktree's agent -- completing it first when Orca drafted only the issue
# URL, which is what an untrusted orca.yaml `issueCommand` falls back to, and
# reaching the agent at all through a wrapper that cannot find Orca.app. Both
# stub the orca CLI and need neither Docker nor a workspace, so they never skip.
set(ORCA_BROWSER_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_orca_browser.sh")

foreach(phase opens reuses foreign no_romm submits no_draft unstable
              watch_needs_issue watch_late_draft watch_grace watch_submits watch_single
              watch_bare_url watch_full_draft_untouched cli_broken
              await_reports_failed_review await_reports_a_red_build
              await_finds_the_failed_run_under_newer_skipped_ones
              await_throttles_the_lookup_when_the_run_is_never_found
              fleet_notices_a_stalled_agent
              review_status_shows_the_open_thread brief_never_lists_threads_over_rest
              brief_queues_the_merge_at_step_four
              await_costs_nothing_while_the_review_is_healthy
              await_stops_paying_once_the_review_recovers
              reap_judges_removal_by_the_directory
              brief_warns_about_human_merge_paths)
  add_test(NAME orca.browser_${phase}
           COMMAND "${BASH_PROGRAM}" "${ORCA_BROWSER_TEST}" ${phase})
  set_tests_properties(orca.browser_${phase} PROPERTIES TIMEOUT 60)
endforeach()

# --- the agent configuration itself ------------------------------------------
# CLAUDE.md, REVIEW.md, the skills, the subagents and the hooks steer three
# parallel worktrees, and every way they break is silent: a skill whose
# frontmatter does not parse never loads, a hook whose path is wrong never runs,
# a guard whose pattern stopped matching stops blocking. Deterministic, no model
# and no network, so it never skips. Mirrored by the `static` job in
# .github/workflows/agent-config.yml -- this is the same check, one push earlier.
add_test(NAME agent.config
         COMMAND "${BASH_PROGRAM}" "${CMAKE_SOURCE_DIR}/evals/lint.sh")
set_tests_properties(agent.config PROPERTIES TIMEOUT 60)

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

# A venv whose interpreter went away is the case that makes a worktree
# unprovisionable forever: `venv` over a dangling bin/python exits 1, so every
# re-run of setup.sh fails identically.
add_test(NAME orca.env_replaces_dead_venv
         COMMAND "${BASH_PROGRAM}" "${ORCA_ENV_TEST}" venv)

# And setup.sh's own half, which a test of the picker alone cannot see.
add_test(NAME orca.env_setup_fails_fast
         COMMAND "${BASH_PROGRAM}" "${ORCA_ENV_TEST}" setup_fails_fast)

set_tests_properties(orca.env_concurrent_writers orca.env_never_partial
                     orca.env_picks_python orca.env_replaces_dead_venv
                     orca.env_setup_fails_fast
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

# `down` only removes services whose profile is active, so the TLS terminator --
# behind the `tls` profile, and `restart: unless-stopped` like everything else
# here -- is invisible to a teardown that does not name it: it survives, comes
# back on the next docker start, holds that worktree's TLS_PORT, and blocks the
# network removal behind it. Reads the compose file and the two scripts, so it
# needs no docker.
add_test(NAME orca.teardown_covers_profiles
         COMMAND "${BASH_PROGRAM}" "${ORCA_TEARDOWN_TEST}" profiles)
set_tests_properties(orca.teardown_covers_profiles PROPERTIES TIMEOUT 60)

add_test(NAME orca.reap_spares_live_worktrees
         COMMAND "${BASH_PROGRAM}" "${ORCA_TEARDOWN_TEST}" reap)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(orca.reap_spares_live_worktrees PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(orca.reap_spares_live_worktrees PROPERTIES TIMEOUT 60)

# --- the Switch targets: sysmodule/, overlay/ and switch.mk ------------------
# The devkitPro Makefiles are not driven by this build -- CMake builds the host
# half and devkitA64 builds the console half (docs/DEVELOPMENT.md#toolchain) --
# but the guarantee that they still produce something loadable belongs with
# every other guarantee, in `ctest`.
set(SWITCH_BUILD_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_switch_build.sh")

# Reads .github/workflows/ci.yml and the two Makefiles, so it never skips: the
# guards that let the job stay green over a Switch build that never happened
# must not be able to come back unnoticed.
add_test(NAME switch.ci_requires_artifacts
         COMMAND "${BASH_PROGRAM}" "${SWITCH_BUILD_TEST}" ci)
set_tests_properties(switch.ci_requires_artifacts PROPERTIES TIMEOUT 60)

# The real cross-compile, in the same devkitpro/devkita64 image CI uses. It needs
# that image, which the CI host runner does not have and will not pull for 2.7GB.
#
# Read the next line before changing it: this test's SKIP_RETURN_CODE is not
# wrapped in if(NOT ROMMSYNC_REQUIRE_RIG). Every other skippable test flips to a
# failure under that option precisely so CI cannot go green on a skip -- and
# that is the right default, so each exception needs its reason recorded rather
# than inferred. There are three, and only two of them have this one's reason:
# `switch.builds` and `switch.tlsprobe` are enforced by a different CI job --
# `switch-build` in .github/workflows/ci.yml runs the same builds, in the same
# image, on every push, and fails there -- so these entries are its local
# equivalent, not its enforcement. `package.builds` is the third and does NOT
# have that reason; see the note on it below.
#
# So do not "fix" it by wrapping it like the others. That would fail the
# host-tests job on a runner that has no devkitpro image -- which is every runner
# the host job uses.
add_test(NAME switch.builds
         COMMAND "${BASH_PROGRAM}" "${SWITCH_BUILD_TEST}" builds)
set_tests_properties(switch.builds PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# The M0-1 probe (tlsprobe/README.md). Same image, same reason it may skip as
# switch.builds above -- and the same enforcement elsewhere: the `switch-build`
# job builds it on every push and checks the NRO magic there.
add_test(NAME switch.tlsprobe
         COMMAND "${BASH_PROGRAM}" "${SWITCH_BUILD_TEST}" tlsprobe)
set_tests_properties(switch.tlsprobe PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# --- the release zip: scripts/package.sh -------------------------------------
# The install layout is the one thing about this project a user meets before
# anything else runs, and every way it can be wrong is silent on the console: a
# .nsp under its build name installs and does nothing, an .ovl without its
# signature is simply not in the menu, a shipped config.ini overwrites the
# settings an upgrade was supposed to keep.
set(PACKAGE_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_package.sh")

# All four of these stub the two Switch artifacts -- four bytes of magic is all
# the script inspects -- so they need neither Docker nor a cross-compiler and
# none of them ever skips.
foreach(scenario
        layout          # the unpacked tree is exactly the six paths #32 and #33 specify
        refuses         # a wrong .nsp, an unsigned .ovl, a missing input
        deterministic   # the same inputs, the same bytes
        upgrade)        # unzipping over an install keeps config.ini and the rest
  add_test(NAME package.${scenario}
           COMMAND "${BASH_PROGRAM}" "${PACKAGE_TEST}" ${scenario})
  set_tests_properties(package.${scenario} PROPERTIES TIMEOUT 120)
endforeach()

# The real cross-compile, packaged inside the same container the release job
# uses. Same image as switch.builds, and its SKIP_RETURN_CODE is left unwrapped
# for the same reason: the CI host runner has no devkitpro/devkita64 and will
# not pull 2.7GB for one, so wrapping it in if(NOT ROMMSYNC_REQUIRE_RIG) like
# the other skippable entries would make the host-tests job red on every runner
# it uses.
#
# It now has the same enforcement elsewhere that switch.builds has: `switch-build`
# runs scripts/package.sh on every push, and the `release` job runs it again on a
# tag (#34). That is what keeps a failure specific to packaging in the container
# -- a missing `zip`, say -- from first appearing on a tag. `release.ci` is what
# says that step is still there.
add_test(NAME package.builds
         COMMAND "${BASH_PROGRAM}" "${PACKAGE_TEST}" builds)
set_tests_properties(package.builds PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

# --- the install guide: docs/INSTALL.md --------------------------------------
# The one page in this repo nobody may verify by following it -- hard rule 1
# puts a console on the far side of M8-1 -- so what it can be held to is that it
# never names a path the release zip does not ship. `scripts/package.sh --list`
# prints the archive's entries and builds nothing, which is what makes a docs
# test able to ask that without a cross-compiler; a renamed artifact and a moved
# title id are both entries in that list, so both are red here rather than a
# wrong instruction on someone's SD card.
#
# Reads files and runs one bash script, so none of these ever skips.
set(DOCS_INSTALL_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_docs_install.py")

foreach(scenario
        paths       # every SD path the guide names is one the archive ships
        links       # every relative link and anchor resolves
        safety      # no account secret on the console, no weakened TLS
        pairing     # the state table is exactly what PairingState exposes
        structure)  # #35's ten sections, the hardware note, the README link
  add_test(NAME docs.install_${scenario}
           COMMAND ${Python3_EXECUTABLE} "${DOCS_INSTALL_TEST}" ${scenario}
                   --repo "${CMAKE_SOURCE_DIR}")
  set_tests_properties(docs.install_${scenario} PROPERTIES TIMEOUT 60)
endforeach()

# --- M6-3: turning a tag into a release --------------------------------------
# Three files decide whether a `v*` tag publishes something a user can install:
# VERSION, the release job in .github/workflows/ci.yml, and the notes generator.
# None of these entries needs Docker, a network or a tag, so none of them skips
# -- a release path exercised only by releasing is one nobody has tested.
set(RELEASE_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_release.sh")

# Named version.* rather than release.* on purpose: `ctest -R version` is what
# the acceptance criteria name, and this is the entry that has to be in it.
#
# It reads GITHUB_REF out of the environment, which is why it belongs in ctest
# rather than only in the workflow: on a tag push GitHub sets that variable for
# every step, so the `host-tests` job runs this without knowing it exists, and
# the release job is gated on that job. Locally it is a no-op with one extra
# assertion -- that VERSION is still semver -- because the prerelease flag and
# the asset name both read that string structurally.
add_test(NAME version.tag COMMAND "${BASH_PROGRAM}" "${RELEASE_TEST}" version)
set_tests_properties(version.tag PROPERTIES TIMEOUT 60)

# The workflow itself: one trigger and not two, a job gated on both test jobs,
# and the package/checksum/upload chain inside it. The regression it holds is
# the state this repo shipped in for five milestones -- `on: release:` declared
# with no job consuming it, so a tag ran the build and published nothing.
add_test(NAME release.ci COMMAND "${BASH_PROGRAM}" "${RELEASE_TEST}" ci)
set_tests_properties(release.ci PROPERTIES TIMEOUT 60)

# scripts/release-notes.sh. It reads git history, so it runs here rather than
# only on a tag: the fallback for "no previous tag" is the path the FIRST
# release takes, and this repo has no tags, so that is the path exercised.
add_test(NAME release.notes COMMAND "${BASH_PROGRAM}" "${RELEASE_TEST}" notes)
set_tests_properties(release.notes PROPERTIES TIMEOUT 60)

# Which versions are prereleases. The release job asks release-notes.sh for the
# flag rather than repeating the rule, which is what lets this run it against
# eight made-up versions here instead of finding out on the day a v0.2.0-rc1
# published as a stable release.
add_test(NAME release.prerelease
         COMMAND "${BASH_PROGRAM}" "${RELEASE_TEST}" prerelease)
set_tests_properties(release.prerelease PROPERTIES TIMEOUT 60)

# Which tag the change list is counted from, against a throwaway repo with real
# tags in it. `release.notes` above can only ever exercise the first-release
# fallback, because this repo has no tags -- and the case that actually ships,
# a stable cut after one or more release candidates, is the one that was wrong.
add_test(NAME release.history
         COMMAND "${BASH_PROGRAM}" "${RELEASE_TEST}" history)
set_tests_properties(release.history PROPERTIES TIMEOUT 120)

# --- the TLS terminator the M0-1 probe aims at -------------------------------
# The rig is plain HTTP because the host suite reaches it over loopback; the
# probe needs a real handshake to complete, so server/testing/docker-compose.yml
# has a `tls` profile that nothing else starts. `isolated` and `cert` read the
# compose file and mint a certificate, so they never skip -- and `isolated` is
# the one that matters: it is what keeps this out of every other test's way.
set(TLS_FIXTURE_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_tls_fixture.sh")

add_test(NAME tls.isolated COMMAND "${BASH_PROGRAM}" "${TLS_FIXTURE_TEST}" isolated)
add_test(NAME tls.cert COMMAND "${BASH_PROGRAM}" "${TLS_FIXTURE_TEST}" cert)
add_test(NAME tls.serves COMMAND "${BASH_PROGRAM}" "${TLS_FIXTURE_TEST}" serves)
if(NOT ROMMSYNC_REQUIRE_RIG)
  # `cert` skips on a machine with no openssl, which is the same 77 `serves`
  # uses for a stopped Docker -- without this it would be a hard red there. In
  # CI both are failures, which is the point of the option.
  set_tests_properties(tls.cert tls.serves PROPERTIES SKIP_RETURN_CODE 77)
endif()
# `cert` re-mints the certificate the running terminator is serving (and
# restarts it onto the new one), so it must not run while `serves` is asking
# that terminator for a verified 200.
set_tests_properties(tls.isolated tls.cert tls.serves
                     PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

# --- the M0 exit gate's mechanical clauses -----------------------------------
# "No test in the suite requires a console or a production server"
# (docs/TESTING.md#the-m0-exit-gate) is a claim about every test, so it can only
# stay true if something re-asks it on every run. None of these three need
# Docker, Orca or a network, so none of them ever skips -- except `guards`,
# which needs an interpreter that can import the scripts it is running.
set(POLICY_TEST "${CMAKE_CURRENT_SOURCE_DIR}/test_policy.py")

add_test(NAME policy.loopback_only
         COMMAND ${Python3_EXECUTABLE} "${POLICY_TEST}" loopback
                 --repo "${CMAKE_SOURCE_DIR}"
                 --proxy-url "${PROXY_BASE_URL}")

# Asserted on the refusal itself, not on a non-zero exit: a probe that reaches
# for a production RomM and merely fails to connect also exits non-zero.
add_test(NAME policy.writes_refuse_remote
         COMMAND ${Python3_EXECUTABLE} "${POLICY_TEST}" guards
                 --repo "${CMAKE_SOURCE_DIR}")
if(NOT ROMMSYNC_REQUIRE_RIG)
  # It skips when no interpreter can import the scripts it runs -- a plain clone
  # that has not run setup.sh. CI always has the venv, so there the same
  # condition is a failure: this is the check that keeps a writing script from
  # reaching a real RomM, and it must never quietly not run.
  set_tests_properties(policy.writes_refuse_remote PROPERTIES SKIP_RETURN_CODE 77)
endif()

add_test(NAME policy.tests_are_host_local
         COMMAND ${Python3_EXECUTABLE} "${POLICY_TEST}" host_only
                 --repo "${CMAKE_SOURCE_DIR}"
                 --build "${CMAKE_BINARY_DIR}"
                 --ctest "${CMAKE_CTEST_COMMAND}")

set_tests_properties(policy.loopback_only policy.writes_refuse_remote
                     policy.tests_are_host_local
                     PROPERTIES TIMEOUT 120)

# --- M3-2: the download queue and the worker that drains it ------------------
# The first six need no server and must stay checked with docker stopped: what
# they pin is what a yanked SD card leaves behind -- a queue.json that is
# truncated, oversized, half-written, or written by another release -- and a
# healthy RomM cannot produce one of those. The last five are transfers, so they
# need the real RomM and the fault proxy in front of it.
add_executable(test_download test_download.cpp)
target_link_libraries(test_download PRIVATE rommsync::host)
target_include_directories(test_download PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(test_download PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env"
  # `drain` and `resume` compare what reached the card against the bytes RomM is
  # serving from, which is the only comparison that proves a download rather than
  # a plausible-looking file of the right length.
  ROMMSYNC_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/server/testing/library")

set(DOWNLOAD_UNIT_SCENARIOS
  roundtrip  # every field survives queue.json, states and a null hash included
  queue      # Enqueue/Remove/Clear/Snapshot, and the fixed IPC error set
  corrupt    # truncated, garbage, another release's format: an empty queue and a reason
  store      # the write is atomic, leaves no .tmp/.old, and a failed one costs nothing
  bounds     # a full queue of worst-case entries still fits the byte bound
  endpoint   # the content URL's encoding, and the rom detail shape
)
foreach(scenario IN LISTS DOWNLOAD_UNIT_SCENARIOS)
  add_test(NAME download.${scenario} COMMAND test_download ${scenario})
  set_tests_properties(download.${scenario} PROPERTIES TIMEOUT 60)
endforeach()

set(DOWNLOAD_RIG_SCENARIOS
  drain      # the seeded 240pee.nes, byte-for-byte at the mapped destination
  retries    # a 500 leaves the entry queued with attempts up and backoff spent
  verify     # M3-3: a wrong sha1_hash fails the entry and leaves nothing behind
  fallback   # M3-3: md5_hash when there is no sha1, and unverified when neither
  truncate   # M3-3: a body that ends early, caught by the caller's expected size
  stall      # M3-3: the client's own stall_timeout ends it, and the entry retries
  staging    # M3-3: a leftover .tmp is salvaged by its digest, or discarded
  disabled   # `[downloads] enabled = false` drains nothing and loses nothing
  multifile  # a disc set is refused with a reason and no archive reaches the card
  nested     # ...and a directory holding ONE file is downloaded, not refused
  missing    # a rom the server's library no longer holds fails with a sentence
)
foreach(scenario IN LISTS DOWNLOAD_RIG_SCENARIOS)
  add_test(NAME download.${scenario} COMMAND test_download ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(download.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  # RUN_SERIAL for the reason every rig test has it: the fault proxy holds one
  # armed scenario for every client, so two of these at once damage each other.
  set_tests_properties(download.${scenario} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endforeach()

# `resume` and `progress` both move the 120 MiB fixture twice -- 4 MiB, a dropped
# connection, then the rest over Range -- through the Python fault proxy, so they
# get their own budget rather than the 180 seconds the others need. `progress`
# (M3-5) is the one that watches the second transfer from another thread while it
# is still going, which is the whole of what it adds.
# M6-2 (#33), grouped with the other `toggle.` tests: what survives the hard kill
# ovl-sysmodules performs on a dynamic sysmodule. Cheap enough for the ordinary
# budget -- it cuts a 64 KiB rom, not the 120 MiB one -- and what it adds over
# `download.truncate` is that the second drain is a different process.
add_test(NAME toggle.download COMMAND test_download relaunched)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(toggle.download PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(toggle.download PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

foreach(scenario resume progress)
  add_test(NAME download.${scenario} COMMAND test_download ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(download.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  set_tests_properties(download.${scenario} PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()

# --- the sysmodule's ipc::Engine, on the host --------------------------------
# `sysmodule/source/engine.cpp` names no libnx type, so the only thing that tied
# it to a console was the `sdmc:` prefix on its config directory -- and
# `SdEngine::Load` takes that as a parameter. M3-2 is the first issue to put real
# behaviour behind one of its commands (Enqueue/Dequeue change a file on the
# card), which is what makes "it compiles" stop being a description of what the
# engine does. Needs no server: what it pins is the card.
add_executable(test_engine test_engine.cpp
  "${CMAKE_SOURCE_DIR}/sysmodule/source/engine.cpp")
target_link_libraries(test_engine PRIVATE rommsync::host)
target_include_directories(test_engine PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/sysmodule/source")
target_compile_definitions(test_engine PRIVATE
  ROMMSYNC_PROXY_BASE_URL="${PROXY_BASE_URL}"
  ROMMSYNC_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/scratch"
  ROMMSYNC_FIXTURE_AUTH="${CMAKE_SOURCE_DIR}/server/testing/fixture-auth.env")
# M1-6 put a thread of its own inside `SdEngine`, and the scenarios that drive a
# pairing wait on it from the calling thread.
target_link_libraries(test_engine PRIVATE Threads::Threads)

set(ENGINE_SCENARIOS
  queue     # enqueue, dequeue, and the depth the status screen reads
  persists  # every change reaches queue.json, and a fresh engine reads it back
  rollback  # a write that cannot happen changes neither the file nor memory
  corrupt   # a queue.json a yanked card left behind never blocks the boot
  config    # M5-3: an edit lands on the card and on the running engine
  commit    # M5-3: a commit interrupted between the two renames loses nothing
  unpairs   # M5-3: a server change discards the pairing before it writes
  stale     # M5-3: "did the server change" is a question about the card
  unauthenticated  # M1-4: the verdict the card carries, and the re-pair that lifts it
  commands  # what is still `kUnavailable`, so the list shrinks deliberately
  unreachable  # M1-6: a server that answers nothing refuses the attempt, and writes nothing
)
foreach(scenario IN LISTS ENGINE_SCENARIOS)
  add_test(NAME engine.${scenario} COMMAND test_engine ${scenario})
  set_tests_properties(engine.${scenario} PROPERTIES TIMEOUT 60)
endforeach()
# `unreachable` waits for an attempt to settle rather than for a file, and its own
# budget is longer than the sixty seconds the rest of this group needs. Given its
# own so a broken one fails on the assertion that names the behaviour instead of
# on a CTest timeout that names nothing.
set_tests_properties(engine.unreachable PROPERTIES TIMEOUT 180)

# M6-2 (#33), under `toggle.` with the rest of that issue's acceptance. Both are
# about the two switches rather than about the queue this file otherwise pins:
# `relaunch` is the terminate-then-launch ovl-sysmodules performs -- two engines
# over one card, no lock file, neither refusing to start -- and `reenable` is the
# enable switch turned off and back on inside one process.
add_test(NAME toggle.relaunch COMMAND test_engine relaunch)
add_test(NAME toggle.reenable COMMAND test_engine reenable)
set_tests_properties(toggle.relaunch toggle.reenable PROPERTIES TIMEOUT 60)

# M5-3's one scenario that needs the fixture RomM. Repointing `server.url` has to
# discard the pairing, and a token no server ever accepted would prove nothing
# about that -- so this one uses the live fixture token and checks it works right
# up until the edit, and that the console holds nothing afterwards.
add_test(NAME engine.server COMMAND test_engine server)
if(NOT ROMMSYNC_REQUIRE_RIG)
  set_tests_properties(engine.server PROPERTIES SKIP_RETURN_CODE 77)
endif()
set_tests_properties(engine.server PROPERTIES TIMEOUT 120)

# M1-6's three, which drive a real device-code attempt through the engine: the
# codes are RomM's, the approval goes through RomM's own
# `/api/auth/device/approve`, and the interruptions are the fault proxy's. Serial
# for the same reason `pair.*` is -- the proxy holds one armed scenario for every
# client, and RomM paces the token endpoint -- and generous on time because two
# of them wait out real poll intervals.
set(ENGINE_RIG_SCENARIOS
  pairs        # start to committed token, and the token works against the server
  nonblocking  # `StartPair` returns while the init is stalled: `ipc.hpp`'s contract
  repairs      # `StartPair` then `Unpair`, with neither instant leaving nothing
)
foreach(scenario IN LISTS ENGINE_RIG_SCENARIOS)
  add_test(NAME engine.${scenario} COMMAND test_engine ${scenario})
  if(NOT ROMMSYNC_REQUIRE_RIG)
    set_tests_properties(engine.${scenario} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
  set_tests_properties(engine.${scenario} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()

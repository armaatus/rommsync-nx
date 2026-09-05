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

# MD5, against RFC 1321's own vectors. Pure arithmetic, so it never skips -- and
# it must not: a wrong digest is still 32 plausible hex characters, and the only
# symptom on a console is every unchanged save negotiating as changed forever.
add_executable(test_md5 test_md5.cpp)
target_link_libraries(test_md5 PRIVATE rommsync::core)
target_include_directories(test_md5 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core.md5 COMMAND test_md5)

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
  multifile       # the two-disc fixture, and the two traps around it
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
              watch_bare_url watch_full_draft_untouched cli_broken)
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
# Read the next line before changing it: this is the ONLY test in this file whose
# SKIP_RETURN_CODE is not wrapped in if(NOT ROMMSYNC_REQUIRE_RIG). Every other
# skippable test flips to a failure under that option precisely so CI cannot go
# green on a skip -- and that is the right default, so the exception needs its
# reason recorded rather than inferred. The reason is that this one's enforcement
# lives in a different CI job: `switch-build` in .github/workflows/ci.yml runs
# the same build, in the same image, on every push, and fails there. This entry
# is its local equivalent, not its enforcement.
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
        layout          # the unpacked tree is exactly #32's five paths
        refuses         # a wrong .nsp, an unsigned .ovl, a missing input
        deterministic   # the same inputs, the same bytes
        upgrade)        # unzipping over an install keeps config.ini and the rest
  add_test(NAME package.${scenario}
           COMMAND "${BASH_PROGRAM}" "${PACKAGE_TEST}" ${scenario})
  set_tests_properties(package.${scenario} PROPERTIES TIMEOUT 120)
endforeach()

# The real cross-compile, packaged inside the same container #34's release job
# will use. Same image as switch.builds, and its SKIP_RETURN_CODE is left
# unwrapped for the same recorded reason: the CI host runner has no
# devkitpro/devkita64 and will not pull 2.7GB for one, and the enforcement lives
# in the `switch-build` job rather than here.
add_test(NAME package.builds
         COMMAND "${BASH_PROGRAM}" "${PACKAGE_TEST}" builds)
set_tests_properties(package.builds PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)

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

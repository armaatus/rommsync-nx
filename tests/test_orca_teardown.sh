#!/usr/bin/env bash
# Covers scripts/orca/archive.sh, reap.sh and compose.sh's `down` -- the removal
# half of a worktree's life, the reverse of scripts/orca/setup.sh.
#
# A stack that survives its worktree is not a cosmetic leak: the fixture restarts
# `unless-stopped`, so it comes back on every docker start and holds two ports
# and four volumes, with no directory left on disk to identify it from.
#
#   test_orca_teardown.sh derives   .env is unreadable -> archive.sh must still
#                                   target the project setup.sh created, not the
#                                   compose default. This is the regression:
#                                   reading .env back yields `rmx-local` and
#                                   removes nothing. Needs no docker.
#   test_orca_teardown.sh reap      reap.sh flags a stack with no worktree and
#                                   never flags one still in use. Skips with 77
#                                   when docker is down, like rig.smoke.
#   test_orca_teardown.sh watcher   the agent autostart watcher is signalled on
#                                   the way out, and a pid it merely left behind
#                                   -- one the system has since handed to
#                                   something else -- is not. Needs no docker.
#   test_orca_teardown.sh compose   `compose.sh down` -- the teardown ci.yml and
#                                   provision.py both point at -- activates every
#                                   profile and removes orphans, while `up -d`
#                                   still activates none. Stubbed docker, so it
#                                   needs none.
#   test_orca_teardown.sh compose_live
#                                   the same question put to a real daemon: #122's
#                                   acceptance, that nothing carrying the
#                                   project's label survives `down -v`. Skips
#                                   with 77 when docker is down.
#   test_orca_teardown.sh profiles  every profile in the compose file is named on
#                                   the `down` that removes the stack. `down`
#                                   only touches services whose profile is
#                                   active, so a profiled service is invisible to
#                                   a teardown that does not ask for it -- it
#                                   survives, `restart: unless-stopped` brings it
#                                   back, and it holds its port and blocks the
#                                   network removal behind it. Needs no docker.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SKIP=77

# For orca_project_remnants: the phases below and the scripts they judge then
# ask docker the same three questions, rather than two hand-copied sets of
# filters that can drift apart.
. "$REPO_ROOT/scripts/orca/lib.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }

# Every profile the compose file defines, as bare names. A teardown is only
# complete against the file it is tearing down, so the expectation is read from
# there rather than written out here -- a profile added later then fails these
# assertions instead of quietly escaping them.
compose_profiles() {
  grep -oE '^[[:space:]]*profiles:[[:space:]]*\[[^]]*\]' \
      "$REPO_ROOT/server/testing/docker-compose.yml" \
    | grep -oE '"[^"]+"' | tr -d '"' | sort -u
}

# A loopback port nothing is listening on. The fixture terminator publishes one,
# and a collision would fail `up` for a reason that has nothing to do with
# teardown. Deliberately above the 25000-26999 band lib.sh derives TLS ports
# from, so this can never take a live worktree's.
free_port() {
  local p
  for p in $(seq 27000 27099); do
    (: </dev/tcp/127.0.0.1/"$p") >/dev/null 2>&1 && continue
    printf '%s\n' "$p"
    return 0
  done
  return 1
}

# Whether $2 -- a `down` invocation -- activates every profile the compose file
# defines. $1 names what is being judged, so the failure says which.
#
# Two spellings arrive here and mean the same thing: the literal text of
# archive.sh, reap.sh and compose.sh carries the source's own quoting, while a
# stubbed docker's log has had the shell strip it already. Accepting both in one
# place is what keeps that difference deliberate rather than a coincidence
# repeated at three call sites.
assert_activates_every_profile() {
  local what="$1" invocation="$2" profile
  for profile in $(compose_profiles); do
    case "$invocation" in
      *"--profile $profile"*|*"--profile '*'"*|*"--profile *"*|*'COMPOSE_PROFILES'*) ;;
      *) fail "$what does not activate the '$profile' profile, so $profile services"\
              "survive teardown, restart themselves, and hold the network behind"\
              "them: $invocation" ;;
    esac
  done
}

# Whether $2 sweeps orphans. Beside the profile assertion because it is the
# other half of the same guarantee: a `down` that keeps the profile but loses
# this one removes the terminator and leaves whatever compose has stopped
# recognising as a service, which is the half that leaves nothing to see.
assert_removes_orphans() {
  grep -q -- '--remove-orphans' <<<"$2" \
    || fail "$1 leaves orphaned containers behind: $2"
}

# Remove everything docker holds under one project label. Only ever used to
# clean up after a phase: a test that fabricates a stack and leaves it behind is
# committing the exact leak this file exists to catch.
remove_project() {
  local kind name
  while IFS="$(printf '\t')" read -r kind name; do
    [ -n "${name:-}" ] || continue
    case "$kind" in
      container) docker rm -f "$name" >/dev/null 2>&1 ;;
      volume)    docker volume rm "$name" >/dev/null 2>&1 ;;
      network)   docker network rm "$name" >/dev/null 2>&1 ;;
    esac
  done < <(orca_project_remnants "$1")
  return 0
}

FIXTURE=""
ORPHAN=""
SPACED=""
LIVE=""
cleanup() {
  [ -n "$FIXTURE" ] && rm -rf "$FIXTURE"
  if [ -n "$SPACED" ]; then
    git -C "$REPO_ROOT" worktree remove --force "$SPACED" >/dev/null 2>&1
    git -C "$REPO_ROOT" worktree prune >/dev/null 2>&1
    rm -rf "$(dirname "$SPACED")"
  fi
  # Never leave a fabricated stack behind -- that would be this test committing
  # the exact leak it exists to catch. compose_live's is the one that matters
  # most: the fixture root holding its compose file is deleted just above, so
  # nothing on the machine could find it afterwards.
  [ -n "$ORPHAN" ] && remove_project "$ORPHAN"
  if [ -n "$LIVE" ]; then
    # Unlabelled, so remove_project cannot see it -- see compose_live.
    docker rm -f "$LIVE-upstream" >/dev/null 2>&1
    remove_project "$LIVE"
  fi
  return 0
}
trap cleanup EXIT

case "${1:-}" in
  watcher)
    # archive.sh runs while Orca deletes the worktree underneath it, so a watcher
    # still polling from that directory has to be stopped. It identifies the
    # process before signalling, because a pidfile outlives a `kill -9` and a
    # reboot -- and the number in it is then whatever the system reused it for.
    FIXTURE="$(mktemp -d)"
    mkdir -p "$FIXTURE/scripts/orca" "$FIXTURE/server/testing" "$FIXTURE/.orca"
    cp "$REPO_ROOT"/scripts/orca/{lib.sh,env.sh,archive.sh,compose.sh} "$FIXTURE/scripts/orca/"
    cp "$REPO_ROOT/server/testing/docker-compose.yml" "$FIXTURE/server/testing/"

    # Stand in for the watcher: what archive.sh matches on is the command line,
    # so the name is the whole fixture. No `exec`, or the command line becomes
    # `sleep` and the process stops looking like a watcher.
    printf '#!/usr/bin/env bash\nsleep 20\n' >"$FIXTURE/agent-autostart-stub"
    chmod +x "$FIXTURE/agent-autostart-stub"
    # stdout to /dev/null, or the stand-in (and the `sleep` inside it) holds
    # this test's stdout open and ctest waits on the pipe long after the
    # assertions are done.
    "$FIXTURE/agent-autostart-stub" >/dev/null 2>&1 & watcher=$!
    echo "$watcher" >"$FIXTURE/.orca/agent-autostart.pid"

    # Docker is not required and may not be running; either way archive.sh gets
    # to the watcher first, which is the point of where the block sits.
    out="$(cd "$FIXTURE" && ./scripts/orca/archive.sh 2>&1)"
    for _ in $(seq 1 50); do kill -0 "$watcher" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$watcher" 2>/dev/null; then
      pkill -P "$watcher" 2>/dev/null
      kill -9 "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
      fail "left the watcher running while the worktree was removed; got: $out"
    fi
    # archive.sh signals the script, not the `sleep` it is blocked in. An
    # orphaned child holds this test's stdout open and hangs ctest long after
    # the assertions are done.
    pkill -P "$watcher" 2>/dev/null
    wait "$watcher" 2>/dev/null
    grep -q "stopping the agent autostart watcher" <<<"$out" \
      || fail "stopped it without saying so; got: $out"

    # Now a stale pidfile naming a live process that is NOT a watcher. Signalling
    # it would kill something of the user's that merely inherited the number.
    sleep 20 >/dev/null 2>&1 & bystander=$!
    echo "$bystander" >"$FIXTURE/.orca/agent-autostart.pid"
    out="$(cd "$FIXTURE" && ./scripts/orca/archive.sh 2>&1)"
    alive=1
    kill -0 "$bystander" 2>/dev/null && alive=0
    kill "$bystander" 2>/dev/null; wait "$bystander" 2>/dev/null
    [ "$alive" -eq 0 ] \
      || fail "signalled a recycled pid -- an unrelated process of the user's; got: $out"
    grep -q "stopping the agent autostart watcher" <<<"$out" \
      && fail "claimed to stop a watcher that was not one: $out"
    echo "PASS: the watcher is stopped, and a recycled pid is left alone"
    ;;

  derives)
    FIXTURE="$(mktemp -d)"
    mkdir -p "$FIXTURE/scripts/orca" "$FIXTURE/server/testing"
    cp "$REPO_ROOT"/scripts/orca/{lib.sh,env.sh,archive.sh,compose.sh} "$FIXTURE/scripts/orca/"
    cp "$REPO_ROOT/server/testing/docker-compose.yml" "$FIXTURE/server/testing/"

    # The creation path defines the truth teardown has to match: whatever
    # setup.sh would have named this worktree's stack is what must be removed.
    expected="$("$FIXTURE/scripts/orca/env.sh" | sed -n 's/.*project=\([^ ]*\).*/\1/p')"
    [ -n "$expected" ] || fail "env.sh printed no project name; fixture is broken"

    # Stand in for docker and record every call, so the assertions can be about
    # which stack teardown actually named rather than whether it looked willing
    # to. `info` must succeed: archive.sh treats an unreachable daemon as "leave
    # it for reap" and would otherwise never reach the teardown under test.
    mkdir -p "$FIXTURE/bin"
    cat >"$FIXTURE/bin/docker" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >> "$DOCKER_CALL_LOG"
exit 0
FAKE
    chmod +x "$FIXTURE/bin/docker"

    # Two ways setup.sh can leave a worktree with no usable .env: it died before
    # env.sh finished, or it died inside env.sh's write. Both must still tear
    # down, and neither is hypothetical -- .env is generated, gitignored, and
    # written by a hook that is itself allowed to fail.
    #
    # `truncated` is the regression: it is the state the previous archive.sh
    # died in. `missing` is a weaker assertion by construction -- compose.sh
    # regenerates an absent .env on its own -- but it pins the behaviour so a
    # future change to that fallback cannot silently take teardown with it.
    for state in truncated missing; do
      log="$FIXTURE/docker-$state.log"
      : > "$log"
      case "$state" in
        truncated) printf '# truncated\n' > "$FIXTURE/.env" ;;
        missing)   rm -f "$FIXTURE/.env" ;;
      esac

      DOCKER_CALL_LOG="$log" PATH="$FIXTURE/bin:$PATH" \
        "$FIXTURE/scripts/orca/archive.sh" >/dev/null 2>&1
      rc=$?

      # Assert on what was torn down before what the script returned: the
      # failure that matters is a stack left running, and reporting the exit
      # status first would name the mechanism instead of the leak.
      grep -q 'down -v --remove-orphans' "$log" \
        || fail "[$state] archive.sh tore nothing down; docker calls: $(cat "$log")"
      grep -q -- "-p $expected .*down -v" "$log" \
        || fail "[$state] teardown targeted the wrong project; expected $expected, calls: $(cat "$log")"
      # The failure this guards is teardown running with no project named at
      # all, which compose resolves to `rmx-local` and which removes nothing.
      grep 'down -v' "$log" | grep -qv -- '-p rmx-' \
        && fail "[$state] a teardown ran with no project name: $(cat "$log")"
      [ "$rc" -eq 0 ] \
        || fail "[$state] archive.sh exited $rc against a stubbed docker"
    done

    echo "PASS: archive.sh targets $expected with .env truncated and with it missing"
    ;;

  reap)
    docker info >/dev/null 2>&1 || { echo "SKIP: docker is not running"; exit $SKIP; }

    live="$(sed -n 's/^COMPOSE_PROJECT_NAME=//p' "$REPO_ROOT/.env")"
    [ -n "$live" ] || fail "no COMPOSE_PROJECT_NAME in .env; run ./scripts/orca/env.sh"

    # A stack with no worktree, built by hand out of the pieces compose leaves
    # behind when a worktree is deleted with `rm -rf`.
    ORPHAN="rmx-reap-test-$$"
    docker volume create --label "com.docker.compose.project=$ORPHAN" "${ORPHAN}_db_data" >/dev/null \
      || fail "could not create the fixture volume"
    docker network create --label "com.docker.compose.project=$ORPHAN" "${ORPHAN}_default" >/dev/null \
      || fail "could not create the fixture network"

    out="$("$REPO_ROOT/scripts/orca/reap.sh" 2>&1)" \
      || fail "reap.sh exited non-zero; got: $out"
    grep -q "$ORPHAN" <<<"$out" \
      || fail "reap.sh did not flag a stack with no worktree; got: $out"

    # The dangerous failure is the opposite one. This worktree is live and its
    # database is in use; reap must derive its project name and leave it alone.
    grep -q "^== $live" <<<"$out" \
      && fail "reap.sh listed the live worktree's own stack ($live) as stale"

    # A worktree whose path contains a space is still a live worktree. Parsing
    # that path with `awk '"'"'{print $2}'"'"' truncates it, derives some other name, and
    # drops the real stack out of the protected set -- so `--yes` deletes a
    # database that is in use.
    SPACED="$(mktemp -d)/a worktree with spaces"
    git -C "$REPO_ROOT" worktree add -q --detach "$SPACED" HEAD \
      || fail "could not create the spaced worktree fixture"
    spaced_project="$( . "$REPO_ROOT/scripts/orca/lib.sh"; \
                       orca_derive_env "$SPACED" && echo "$orca_project" )"
    [ -n "$spaced_project" ] || fail "could not derive the spaced worktree's project name"
    out="$("$REPO_ROOT/scripts/orca/reap.sh" 2>&1)" || fail "reap.sh failed: $out"
    grep -q "^== $spaced_project" <<<"$out" \
      && fail "a worktree path with a space was left unprotected ($spaced_project)"

    # With no trustworthy list of live worktrees, everything looks stale. Refusing
    # is the only safe answer; classifying is how a live database gets deleted.
    stub="$(mktemp -d)"
    printf '#!/usr/bin/env bash\nexit 128\n' > "$stub/git"
    chmod +x "$stub/git"
    if out="$(PATH="$stub:$PATH" "$REPO_ROOT/scripts/orca/reap.sh" 2>&1)"; then
      rm -rf "$stub"
      fail "reap.sh succeeded with no usable worktree list; got: $out"
    fi
    grep -q "^== " <<<"$out" \
      && { rm -rf "$stub"; fail "reap.sh classified stacks as stale without a worktree list"; }
    rm -rf "$stub"

    # Only sweep for real when the fixture is the single stale stack, so the
    # test can never destroy an orphan that belongs to someone else's work.
    listed="$("$REPO_ROOT/scripts/orca/reap.sh" 2>&1 | grep -c '^== ')"
    if [ "$listed" -ne 1 ]; then
      echo "SKIP: $listed stale stacks present; --yes would sweep more than the fixture"
      exit $SKIP
    fi
    "$REPO_ROOT/scripts/orca/reap.sh" --yes >/dev/null 2>&1 \
      || fail "reap.sh --yes exited non-zero"

    remaining="$(docker volume ls -q --filter "label=com.docker.compose.project=$ORPHAN"; \
                 docker network ls -q --filter "label=com.docker.compose.project=$ORPHAN")"
    [ -z "$remaining" ] || fail "--yes left the orphan behind: $remaining"
    [ -n "$(docker ps -q --filter "label=com.docker.compose.project=$live")" ] \
      || fail "--yes took down the live worktree's stack ($live)"

    echo "PASS: reap.sh sweeps $ORPHAN, spares $live, and refuses to guess"
    ;;

  profiles)
    profiles="$(compose_profiles)"
    if [ -z "$profiles" ]; then
      echo "PASS: the compose file defines no profiles; nothing to cover"
      exit 0
    fi

    for script in archive.sh reap.sh; do
      # The `down` invocation, which spans two lines in both scripts.
      down="$(grep -A2 -E 'docker compose -p "\$project"' \
                "$REPO_ROOT/scripts/orca/$script" | tr '\n' ' ')"
      [ -n "$down" ] || fail "$script no longer runs docker compose down on a project"
      assert_activates_every_profile "$script's \`down\`" "$down"
      # Both halves here, not just the profile: archive.sh's orphan sweep is
      # pinned by `derives`, but reap.sh's was asserted nowhere, so dropping it
      # left every test green while the sweep stopped removing containers
      # compose had stopped recognising as services.
      assert_removes_orphans "$script's \`down\`" "$down"
    done

    echo "PASS: teardown activates every compose profile ($(echo $profiles | tr '\n' ' '))"
    ;;

  compose)
    # The documented teardown path (#122). archive.sh and reap.sh name the `tls`
    # profile outright, but `./scripts/orca/compose.sh down -v` -- the command
    # ci.yml's teardown step and provision.py both point at -- did not, so the
    # TLS terminator survived it and held the network behind it.
    #
    # Stubbed docker, so this asserts what the wrapper ASKED FOR rather than what
    # a daemon happened to leave; `compose_live` is the same question put to a
    # real one.
    FIXTURE="$(mktemp -d)"
    mkdir -p "$FIXTURE/scripts/orca" "$FIXTURE/server/testing" "$FIXTURE/bin"
    cp "$REPO_ROOT"/scripts/orca/{lib.sh,env.sh,compose.sh} "$FIXTURE/scripts/orca/"
    cp "$REPO_ROOT/server/testing/docker-compose.yml" "$FIXTURE/server/testing/"
    # Hand-written, so env.sh never runs: this phase is about the wrapper's
    # argument handling and nothing else.
    printf 'COMPOSE_PROJECT_NAME=rmx-compose-stub-test\n' >"$FIXTURE/.env"

    cat >"$FIXTURE/bin/docker" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >> "$DOCKER_CALL_LOG"
exit 0
FAKE
    chmod +x "$FIXTURE/bin/docker"

    profiles="$(compose_profiles)"
    [ -n "$profiles" ] || fail "the compose file defines no profiles; fixture is broken"

    log="$FIXTURE/docker.log"

    : > "$log"
    DOCKER_CALL_LOG="$log" PATH="$FIXTURE/bin:$PATH" \
      "$FIXTURE/scripts/orca/compose.sh" down -v >/dev/null 2>&1 \
      || fail "compose.sh down -v exited non-zero against a stubbed docker"
    down="$(cat "$log")"
    grep -q ' down ' <<<" $down " || fail "compose.sh down ran no down: $down"
    assert_activates_every_profile "compose.sh down" "$down"
    # A container compose no longer recognises as a service is still this
    # worktree's, and teardown is the moment to say so -- archive.sh and reap.sh
    # already do.
    assert_removes_orphans "compose.sh down" "$down"

    # The other half, and the reason this is not simply a global flag: an
    # ordinary `up -d` must still start neither the TLS terminator nor anything
    # else profiled. tls-fixture.sh is what asks for that, and
    # tests/test_tls_fixture.sh isolated is what the rig depends on.
    : > "$log"
    DOCKER_CALL_LOG="$log" PATH="$FIXTURE/bin:$PATH" \
      "$FIXTURE/scripts/orca/compose.sh" up -d >/dev/null 2>&1 \
      || fail "compose.sh up -d exited non-zero against a stubbed docker"
    up="$(cat "$log")"
    grep -q -- '--profile' <<<"$up" \
      && fail "compose.sh up -d activated a profile; the rig would gain a TLS front door: $up"

    # And a caller that names a profile itself still gets it through untouched:
    # tls-fixture.sh's `up`, `ps` and `rm` are all of that shape.
    : > "$log"
    DOCKER_CALL_LOG="$log" PATH="$FIXTURE/bin:$PATH" \
      "$FIXTURE/scripts/orca/compose.sh" --profile tls up -d romm-tls >/dev/null 2>&1 \
      || fail "compose.sh --profile tls up -d exited non-zero against a stubbed docker"
    passthrough="$(cat "$log")"
    grep -q -- '--profile tls .*up -d romm-tls' <<<"$passthrough" \
      || fail "compose.sh mangled a caller's own --profile: $passthrough"

    # A `down` behind global flags is still a `down`. `-p` takes a value, so a
    # scan that does not know that reads the project name as the subcommand and
    # activates nothing.
    : > "$log"
    DOCKER_CALL_LOG="$log" PATH="$FIXTURE/bin:$PATH" \
      "$FIXTURE/scripts/orca/compose.sh" -p rmx-other down -v >/dev/null 2>&1 \
      || fail "compose.sh -p ... down -v exited non-zero against a stubbed docker"
    flagged="$(cat "$log")"
    assert_activates_every_profile "compose.sh -p ... down" "$flagged"
    # Both flags come off the same branch today, so this asserts no new code
    # path -- the profile assertion above already fails a `down` the scanner
    # missed, since neither flag would be there. It pins the pair together: a
    # change that ever routed a flagged `down` differently could not then keep
    # the profile and quietly lose the sweep.
    assert_removes_orphans "compose.sh -p ... down" "$flagged"

    echo "PASS: compose.sh down activates every profile ($(echo $profiles | tr '\n' ' ')), up -d none"
    ;;

  compose_live)
    docker info >/dev/null 2>&1 || { echo "SKIP: docker is not running"; exit $SKIP; }

    # The acceptance in #122, put to a real daemon: after `compose.sh down -v`,
    # `docker ps -a`, `docker volume ls` and `docker network ls` show nothing
    # carrying this project's label.
    #
    # A fixture root of its own -- compose.sh derives the repo root from its own
    # path and reads the .env beside it -- so this can start and destroy a whole
    # stack without going anywhere near the worktree's live one.
    FIXTURE="$(mktemp -d)"
    mkdir -p "$FIXTURE/scripts/orca" "$FIXTURE/server/testing/tls/generated" \
             "$FIXTURE/server/testing/library"
    cp "$REPO_ROOT"/scripts/orca/{lib.sh,env.sh,compose.sh} "$FIXTURE/scripts/orca/"
    cp "$REPO_ROOT/server/testing/docker-compose.yml" "$FIXTURE/server/testing/"
    cp "$REPO_ROOT/server/testing/tls/romm-tls.conf.template" "$FIXTURE/server/testing/tls/"
    cp "$REPO_ROOT/server/testing/fault_proxy.py" "$FIXTURE/server/testing/"

    LIVE="rmx-compose-down-test-$$"
    port="$(free_port)" || fail "no free port in 27000-27099 for the fixture terminator"
    # Written by hand rather than by env.sh, and only TLS_PORT is ever bound:
    # nothing but romm-tls starts below.
    printf 'COMPOSE_PROJECT_NAME=%s\nTLS_PORT=%s\n' "$LIVE" "$port" >"$FIXTURE/.env"

    # A certificate, because nginx will not start without one, and it has to be
    # RUNNING for this phase to reproduce what #122 reported: an exited container
    # is still left behind, but only a running one pins the network, which is the
    # other half of the report.
    command -v openssl >/dev/null 2>&1 || { echo "SKIP: no openssl"; exit $SKIP; }
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
      -subj "/CN=romm.fixture.local" \
      -keyout "$FIXTURE/server/testing/tls/generated/server.key" \
      -out "$FIXTURE/server/testing/tls/generated/server.crt" >/dev/null 2>&1 \
      || fail "could not mint the fixture certificate"

    # The terminator alone, without the RomM behind it: a whole rig would cost a
    # database healthcheck for a question that is only about what teardown sees.
    # nginx resolves `proxy_pass ${UPSTREAM}` once, at start-up, so it needs the
    # name `romm` to exist for exactly that long -- hence a stand-in on the
    # network for the duration of the start, removed again immediately. It
    # carries no compose label on purpose: teardown must not be able to claim
    # credit for removing it, and cleanup() takes it by name.
    "$FIXTURE/scripts/orca/compose.sh" --profile tls up -d --no-deps romm-tls >/dev/null 2>&1 \
      || fail "could not create the fixture terminator"
    docker run -d --name "$LIVE-upstream" --network "${LIVE}_default" \
      --network-alias romm nginx:1.27-alpine >/dev/null 2>&1 \
      || fail "could not stand in for the terminator's upstream"
    docker start "$LIVE-romm-tls-1" >/dev/null 2>&1 \
      || fail "could not start the fixture terminator"
    for _ in $(seq 1 50); do
      [ "$(docker inspect -f '{{.State.Running}}' "$LIVE-romm-tls-1" 2>/dev/null)" = true ] && break
      sleep 0.1
    done
    docker rm -f "$LIVE-upstream" >/dev/null 2>&1

    # Assert the fixture is really in the shape the bug needs before tearing it
    # down. Without this the phase passes when nothing was ever started.
    [ "$(docker inspect -f '{{.State.Running}}' "$LIVE-romm-tls-1" 2>/dev/null)" = true ] \
      || fail "the fixture terminator is not running, so it would not hold the network: $(docker logs "$LIVE-romm-tls-1" 2>&1 | tail -1)"
    [ -n "$(docker volume ls -q --filter "label=com.docker.compose.project=$LIVE")" ] \
      || fail "the fixture created no volumes, so the volume half of this would pass vacuously"
    [ -n "$(docker network ls -q --filter "label=com.docker.compose.project=$LIVE")" ] \
      || fail "the fixture created no network, so the network half of this would pass vacuously"

    "$FIXTURE/scripts/orca/compose.sh" down -v >/dev/null 2>&1 \
      || fail "compose.sh down -v exited non-zero"

    remnants="$(orca_project_remnants "$LIVE")"
    [ -z "$remnants" ] || fail "compose.sh down -v left this behind: $(echo $remnants)"

    # By label above, because that is how archive.sh and reap.sh find a stack.
    # By name here, because #122 asks about the project PREFIX, and a remnant
    # that lost its label is exactly what the by-label sweep cannot see either.
    named="$(
      docker ps -a      --format '{{.Names}}'
      docker volume ls  --format '{{.Name}}'
      docker network ls --format '{{.Name}}'
    )"
    leftover="$(grep "^$LIVE" <<<"$named")"
    [ -z "$leftover" ] || fail "something carrying the project prefix survived: $(echo $leftover)"

    echo "PASS: compose.sh down -v leaves no container, volume or network for $LIVE"
    ;;

  *)
    echo "usage: $0 derives|reap|watcher|profiles|compose|compose_live" >&2
    exit 2
    ;;
esac

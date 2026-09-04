#!/usr/bin/env bash
# Follow this worktree's RomM stack -- the `romm` tab in orca.yaml.
#
# The tab cannot just run `compose.sh logs -f`. Orca starts the default tabs when
# the worktree is created, in parallel with setup.sh; `setupAgentStartupPolicy:
# wait-for-setup` holds only the AGENT's tab. setup.sh brings the stack up on its
# last step, after seeding and a full build, so for the first minutes of a
# worktree's life there is nothing to follow -- and `docker compose logs -f`
# against a project with no containers prints nothing and exits 0 immediately.
# The tab died before RomM ever started, which is what made infrastructure
# failure indistinguishable from "the tab is just empty".
#
# So: wait for the stack, follow it, and if it goes away (compose down, a docker
# restart) go back to waiting rather than leaving a dead tab behind.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

COMPOSE="./scripts/orca/compose.sh"
POLL_SECONDS="${ROMM_LOGS_POLL_SECONDS:-2}"

# A pidfile, so setup.sh can answer "is a tab already following?" without having
# to guess at Orca's terminal state -- see ensure-romm-tab.sh. Overridable so a
# test can run a follower without disturbing the real tab's file.
PIDFILE="${ROMM_LOGS_PIDFILE:-$REPO_ROOT/.orca/romm-logs.pid}"
mkdir -p "$(dirname "$PIDFILE")" 2>/dev/null
echo $$ >"$PIDFILE"
# Compared against $$ rather than removed outright: a follower started after this
# one owns the file now, and must not have it deleted out from under it.
trap '[ "$(cat "$PIDFILE" 2>/dev/null)" = "$$" ] && rm -f "$PIDFILE"' EXIT

# One running container is enough to attach. `up -d` creates romm-db first and
# only creates romm once the database is healthy, so waiting for all three would
# show nothing at all when a bring-up stops half way -- an image pull failing
# offline, say -- which is the very failure this tab exists to make visible.
# Attaching early costs nothing: `docker compose logs -f` picks up containers
# created after it attached, verified against this compose version.
stack_is_up() {
  [ -n "$($COMPOSE ps -q 2>/dev/null)" ]
}

while :; do
  if stack_is_up; then
    echo "==> following $(basename "$REPO_ROOT")"
    # --tail keeps a tab opened long after startup from replaying the entire
    # history before it shows anything current.
    $COMPOSE logs -f --tail 200
    echo
    echo "==> log stream ended; waiting for the stack to come back"
    # Unconditional, because `logs -f` can return while the stack is still up --
    # the daemon dropping the stream, for one. Without it that path loops
    # straight back into `logs -f` and spins the tab at tens of iterations a
    # second, three `docker compose` forks each.
    sleep "$POLL_SECONDS"
  else
    echo "==> waiting for RomM (scripts/orca/setup.sh brings it up last)"
    while ! stack_is_up; do sleep "$POLL_SECONDS"; done
  fi
done

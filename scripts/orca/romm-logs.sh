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

# Every service must have a container before following, not just any one of
# them. `up -d` creates romm-db first and only creates romm once the database is
# healthy, so attaching at the first sign of life would follow the database and
# miss RomM's own startup entirely -- the output the tab exists to show.
stack_is_up() {
  local services containers
  services="$($COMPOSE config --services 2>/dev/null | grep -c .)" || return 1
  [ "$services" -gt 0 ] || return 1
  containers="$($COMPOSE ps -q 2>/dev/null | grep -c .)" || return 1
  [ "$containers" -ge "$services" ]
}

while :; do
  if stack_is_up; then
    echo "==> following $(basename "$REPO_ROOT")"
    # --tail keeps a tab opened long after startup from replaying the entire
    # history before it shows anything current.
    $COMPOSE logs -f --tail 200
    echo
    echo "==> stack went away; waiting for it to come back"
  else
    echo "==> waiting for RomM (scripts/orca/setup.sh brings it up last)"
    while ! stack_is_up; do sleep "$POLL_SECONDS"; done
  fi
done

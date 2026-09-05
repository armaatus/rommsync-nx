#!/usr/bin/env bash
# Stop everything, now.
#
# A one-word alias for `fleet.sh stop`, because the moment you need it is not
# the moment to remember a subcommand. See scripts/orca/fleet.sh for what a stop
# actually does -- in short: no new worktrees, and no agent can push, open a PR
# or comment while it is set, whether or not the agent has noticed.
#
#   ./scripts/orca/stop.sh          # drain -- running agents finish what they are on
#   ./scripts/orca/stop.sh --now    # ...and interrupt them too
#   ./scripts/orca/fleet.sh resume  # carry on
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet.sh" stop "$@"

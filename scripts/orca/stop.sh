#!/usr/bin/env bash
# Stop the fleet -- politely by default, immediately with --now.
#
# A one-word alias for `fleet.sh stop`, because the moment you need it is not
# the moment to remember a subcommand. See scripts/orca/fleet.sh, "Stopping",
# for what each of the two does -- in short, a drain writes DRAIN and stops the
# dispatcher launching, while --now also writes STOP, which is what guard.py
# reads before it lets any agent push, open a PR or comment.
#
#   ./scripts/orca/stop.sh          # drain -- the agents in flight finish, push
#                                   #          and open their PRs; nothing new starts
#   ./scripts/orca/stop.sh --now    # ...and interrupt them, and freeze every
#                                   #    outward effect while it is set
#   ./scripts/orca/fleet.sh resume  # carry on
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet.sh" stop "$@"

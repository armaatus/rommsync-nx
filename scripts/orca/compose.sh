#!/usr/bin/env bash
# Run `docker compose` against THIS worktree's stack.
#
# Docker Compose reads `.env` from the project directory -- which is the
# directory holding the compose file, i.e. server/testing/ -- not from the repo
# root and not from the caller's cwd. So a bare
# `docker compose -f server/testing/docker-compose.yml ...` silently resolves
# COMPOSE_PROJECT_NAME to its default and operates on the WRONG stack.
#
# Always go through this wrapper rather than calling docker compose directly.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

[ -f .env ] || ./scripts/orca/env.sh >/dev/null
set -a; . ./.env; set +a

# `down` has to remove everything this worktree started, and a bare
# `docker compose down` does not: a service behind a `profiles:` key is
# invisible to any command that has not activated its profile. The TLS
# terminator (server/testing/docker-compose.yml, profile `tls`) therefore
# survived the documented teardown, `restart: unless-stopped` brought it back on
# every docker start, and it held this worktree's TLS_PORT and the network the
# rest of the teardown was waiting on -- #122. archive.sh and reap.sh already
# name `tls` on their own `down`; this is the same hole in the path a person
# types.
#
# `*` rather than a list, so a profile added to the compose file later is
# covered by this line as it stands. --remove-orphans for the same reason
# teardown wants it there: a container compose no longer recognises as a service
# is still labelled with this project, and reap.sh would find it later.
#
# Only `down`. An ordinary `up -d` must keep starting neither the terminator nor
# anything else profiled -- that is what keeps the host suite talking plain HTTP
# to the fault proxy, and tls-fixture.sh is what asks for the other thing.
subcommand=""
want_value=false
for arg in "$@"; do
  if $want_value; then want_value=false; continue; fi
  case "$arg" in
    --) break ;;
    # `--flag=value` carries its own value; the separated spelling eats the next
    # argument, and a scan that does not know which is which reads
    # `compose.sh -p rmx-other down` as a `-p` subcommand and activates nothing.
    --*=*) ;;
    -f|--file|-p|--project-name|--profile|--project-directory|--env-file|\
    --parallel|--progress|--ansi|--log-level) want_value=true ;;
    -*) ;;
    *) subcommand="$arg"; break ;;
  esac
done

if [ "$subcommand" = down ]; then
  exec docker compose -f server/testing/docker-compose.yml \
    --profile '*' "$@" --remove-orphans
fi

exec docker compose -f server/testing/docker-compose.yml "$@"

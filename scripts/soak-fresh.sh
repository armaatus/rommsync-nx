#!/usr/bin/env bash
# Has the nightly soak passed recently? (M9-15, #204)
#
#   scripts/soak-fresh.sh [MAX_DAYS]      default 3
#
# Exits 1 when the last successful run of .github/workflows/soak.yml on main is
# older than MAX_DAYS. The `soak-fresh` job in ci.yml runs it on every push to
# main.
#
# Why this exists rather than trusting the schedule: GitHub disables a scheduled
# workflow after 60 days without repository activity, and a nightly that fails
# every night is a red X on a page nobody opens. Either way the soak stops
# meaning anything with nothing saying so. This turns "the soak has not passed
# lately" into a red build on main, which is the page people do look at.
#
# A soak that has never passed is measured from the day soak.yml was added, so
# the commit that adds it is not red on arrival and a soak that never once
# passes goes red MAX_DAYS later.
#
# Inputs, each overridable so `ctest -R soak.fresh` can drive it without a
# network or a token:
#   SOAK_LAST_SUCCESS  ISO-8601 time of the last successful run on main; "none"
#                      for never. Default: asked of the GitHub API with `gh`,
#                      which needs GH_TOKEN.
#   SOAK_SINCE         ISO-8601 time soak.yml was added. Default: from git,
#                      which needs history (ci.yml checks out with depth 0).
#   SOAK_NOW           ISO-8601 time to judge at. Default: now.
set -euo pipefail

MAX_DAYS="${1:-3}"
WORKFLOW=".github/workflows/soak.yml"

# epoch ISO -- seconds since the epoch, or nothing for text that is not a time.
# Python rather than `date`, whose flags differ between GNU and BSD.
epoch() {
  python3 -c '
import sys
from datetime import datetime, timezone
text = sys.argv[1].strip().replace("Z", "+00:00")
try:
    moment = datetime.fromisoformat(text)
except ValueError:
    sys.exit(1)
if moment.tzinfo is None:
    moment = moment.replace(tzinfo=timezone.utc)
print(int(moment.timestamp()))
' "$1" 2>/dev/null || true
}

if [ -z "${SOAK_LAST_SUCCESS+set}" ]; then
  SOAK_LAST_SUCCESS="$(gh run list --workflow soak.yml --branch main --status success \
                         --limit 1 --json createdAt --jq '.[0].createdAt // "none"')" || {
    echo "::error::could not ask GitHub for the last successful soak" >&2
    exit 1
  }
fi
if [ -z "${SOAK_SINCE+set}" ]; then
  SOAK_SINCE="$(git log --diff-filter=A --format=%cI -- "$WORKFLOW" | tail -n 1)"
fi
SOAK_NOW="${SOAK_NOW:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"

now="$(epoch "$SOAK_NOW")"
since="$(epoch "$SOAK_SINCE")"
[ -n "$now" ] || { echo "::error::'$SOAK_NOW' is not a time" >&2; exit 1; }
[ -n "$since" ] || {
  echo "::error::cannot tell when $WORKFLOW was added ('$SOAK_SINCE'); is this a shallow checkout?" >&2
  exit 1
}

if [ "$SOAK_LAST_SUCCESS" = "none" ] || [ -z "$SOAK_LAST_SUCCESS" ]; then
  reference="$since"
  what="no soak has passed since $WORKFLOW was added on $SOAK_SINCE"
else
  last="$(epoch "$SOAK_LAST_SUCCESS")"
  [ -n "$last" ] || { echo "::error::'$SOAK_LAST_SUCCESS' is not a time" >&2; exit 1; }
  reference=$(( last > since ? last : since ))
  what="the last soak passed on $SOAK_LAST_SUCCESS"
fi

age=$(( now - reference ))
limit=$(( MAX_DAYS * 86400 ))
if [ "$age" -gt "$limit" ]; then
  echo "::error::$what, $(( age / 86400 )) days ago -- more than $MAX_DAYS. The" \
       "nightly soak has stopped running or stopped passing: open the soak workflow's" \
       "runs, and re-enable it if GitHub disabled it for inactivity (#204)." >&2
  exit 1
fi
echo "ok: $what, $(( age / 3600 )) hours ago (limit $MAX_DAYS days)"

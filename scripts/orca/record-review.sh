#!/usr/bin/env bash
# Record that this exact commit has been reviewed locally.
#
# Writes `.orca/reviewed-<sha>` holding the findings. `.claude/hooks/guard.py`
# refuses `git push` and `gh pr create` from a fleet-owned worktree without it,
# so in the automatic flow a PR arrives already reviewed or it does not arrive.
# A worktree you opened yourself is never gated -- pushing a half-finished
# branch by hand is a normal thing to do.
#
# The marker is per-commit on purpose. Amend or add a commit and the review has
# to be re-run, because the thing that was reviewed is not the thing being
# pushed any more.
#
#   /code-review high                     # then paste or pipe the findings:
#   ./scripts/orca/record-review.sh --stdin  < findings.md
#   ./scripts/orca/record-review.sh findings.md
#   ./scripts/orca/record-review.sh --none   # reviewed, nothing found
#
# It records what you give it. It cannot tell whether a review happened -- that
# is a checklist gate, not proof, and the PR body is where a human checks the
# findings are real.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
mkdir -p .orca

sha="$(git rev-parse HEAD)"
target=".orca/reviewed-$sha"

case "${1:-}" in
  --none)
    printf 'reviewed %s\nno findings\n' "$sha" >"$target" ;;
  --stdin|"")
    { printf 'reviewed %s\n\n' "$sha"; cat; } >"$target" ;;
  *)
    [ -f "$1" ] || { echo "no such file: $1" >&2; exit 2; }
    { printf 'reviewed %s\n\n' "$sha"; cat "$1"; } >"$target" ;;
esac

echo "recorded: $target"
echo "  $(wc -l <"$target" | tr -d ' ') lines. Put these findings in the PR body too --"
echo "  this file is gitignored and the reviewer cannot see it."

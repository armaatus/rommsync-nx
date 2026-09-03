#!/usr/bin/env bash
# Orca issueCommand hook — resolves a linked GitHub issue into a full spec.
#
# Orca runs this to build the agent's initial prompt, so an issue picked from
# the Tasks tab arrives as its complete body rather than a bare URL. Accepts an
# issue number or a github.com issue URL as $1; falls back to this worktree's
# linked issue when called with no argument.
set -euo pipefail

ref="${1:-}"
if [ -z "$ref" ]; then
  ref="$(orca worktree current --json 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["worktree"].get("linkedIssue") or "")' 2>/dev/null || true)"
fi

# Accept a bare number or any .../issues/<n>[...] URL.
num="$(printf '%s' "$ref" | sed -nE 's#.*/issues/([0-9]+).*#\1#p; s#^([0-9]+)$#\1#p' | head -1)"
[ -n "$num" ] || { echo "issue-command: could not resolve an issue from '${ref}'" >&2; exit 1; }

gh issue view "$num" --json number,title,body,labels,milestone,url \
  --template '{{printf "# %v: %v" .number .title}}
{{.url}}
Milestone: {{if .milestone}}{{.milestone.title}}{{else}}none{{end}}
Labels: {{range $i, $l := .labels}}{{if $i}}, {{end}}{{$l.name}}{{end}}

{{.body}}
'

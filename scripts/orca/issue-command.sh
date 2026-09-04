#!/usr/bin/env bash
# Resolves a linked GitHub issue into a full spec.
#
# orca.yaml's `issueCommand` points the agent's opening prompt at this script, so
# an issue picked from the Tasks tab is read as its complete body rather than as
# the bare URL Orca prefills by default. Orca substitutes {{issue}} into that
# prompt, so the agent invokes this with the number; the no-argument form falls
# back to this worktree's linked issue, which is what a human running it by hand
# will want.
set -euo pipefail

ref="${1:-}"
if [ -z "$ref" ]; then
  ref="$(orca worktree current --json 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["worktree"].get("linkedIssue") or "")' 2>/dev/null || true)"
fi

# Accept a bare number or any .../issues/<n>[...] URL.
num="$(printf '%s' "$ref" | sed -nE 's#.*/issues/([0-9]+).*#\1#p; s#^([0-9]+)$#\1#p' | head -1)"
[ -n "$num" ] || { echo "issue-command: could not resolve an issue from '${ref}'" >&2; exit 1; }

# GH_PAGER: Orca runs this hook on a TTY, and `gh` pages TTY output through
# less, which then waits for a keypress no one will press -- the hook never
# exits, Orca never gets the spec, and the agent tab sits on a bare URL forever.
GH_PAGER=cat gh issue view "$num" --json number,title,body,labels,milestone,url \
  --template '{{printf "# %v: %v" .number .title}}
{{.url}}
Milestone: {{if .milestone}}{{.milestone.title}}{{else}}none{{end}}
Labels: {{range $i, $l := .labels}}{{if $i}}, {{end}}{{$l.name}}{{end}}

{{.body}}
'

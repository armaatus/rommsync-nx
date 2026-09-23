#!/usr/bin/env bash
# What a build cost, per issue.
#
#   ./scripts/fleet/fleet.sh cost           every issue the fleet has run
#   ./scripts/fleet/fleet.sh cost 48 52     just those
#   ./scripts/fleet/fleet.sh cost --json    the same figures, for a machine
#
# ONE FILE PER RUN, which is what armaatus/autofleet#151 changed here. This
# report used to reconstruct the answer from the agent CLI's session
# transcripts: slug the worktree path, find `$AUTOFLEET_TRANSCRIPT_DIR/<slug>/`,
# walk every `*.jsonl` under it and its `subagents/` directory, sum the `usage`
# block on each assistant message and de-duplicate by `message.id`. Four hundred
# lines of inference, and every one of its failure modes -- a reaped worktree, a
# path too long for the slug, two worktrees whose slugs collided -- showed up as
# a row of zeros that could not be told from a run that spent nothing.
#
# `--output-format json` makes the build print the answer itself: one object
# carrying `total_cost_usd`, `num_turns` and a `usage` block. The build writes it
# to `$AUTOFLEET_DIR/builds/<issue>/result.json`, and `fleet_build_started` moves
# a finished one into `runs/` rather than overwriting it -- so an issue that was
# resumed has one file per run and this report can say so.
#
# THIS HEADER IS THE AUTHORITY on where the figures come from. `config.sh` and
# `docs/CONFIGURATION.md` describe the same thing for a host project, and when
# they disagree with this file they are the ones that are wrong.
#
# THE TOKEN FIGURES ARE NOT INTERCHANGEABLE AND ARE NEVER ADDED TOGETHER. A
# cache read is roughly a tenth of an input token, so a run that looks expensive
# on `input` may be almost entirely cache, and one total would hide exactly the
# difference this report exists to show. `usd` is the one number that IS
# comparable across runs, and it is the model's own accounting rather than ours.
#
# IT NEVER FAILS A RUN. No build directory, a result a wrapper seam wrote in
# some other shape, a half-written file a live build is still appending to --
# each is normal, costs one line on stderr, and exits 0. `cost` is a reporting
# command; a reporting command that can go non-zero is one more thing a night's
# run can die of.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
. ./scripts/fleet/lib.sh

json=false
issues=()
while [ $# -gt 0 ]; do
  case "$1" in
    --json) json=true; shift ;;
    -h|--help)
      cat >&2 <<'USAGE'
usage: cost.sh [--json] [<issue> ...]

  What each issue's build spent, from the build's own result JSON: dollars,
  turns, and input, output, cache-read and cache-write tokens. The token
  figures are printed separately and never summed.

  With no issues, every issue the fleet has a recorded build for.
USAGE
      exit 2 ;;
    -*) printf 'cost.sh: unknown option %s\n' "$1" >&2; exit 2 ;;
    *)
      # One leading `#` allowed: every line this report prints spells an issue as
      # `#48`, so the form the output teaches was the form the parser refused.
      # Found by the independent review.
      n="${1#\#}"
      case "$n" in
        ''|*[!0-9]*) printf 'cost.sh: not an issue number: %s\n' "$1" >&2; exit 2 ;;
      esac
      issues+=("$n"); shift ;;
  esac
done

shape=table
$json && shape=json

FLEET_BUILDS="$FLEET_BUILDS" python3 -c '
import json, os, sys

shape = sys.argv[1]
wanted = [a for a in sys.argv[2:] if a]
root = os.environ["FLEET_BUILDS"]
as_json = shape == "json"

def note(msg):
    # ONE line on stderr and never an exit: see the header. A reporting command
    # that can go non-zero is one more thing a night run can die of.
    print("cost.sh: " + msg, file=sys.stderr)

# The four token fields, in the order the table prints them, paired with the
# key the CLI`s usage block spells them under. Named here once so the table, the
# totals and the JSON cannot disagree about which is which.
FIELDS = [
    ("input", "input_tokens"),
    ("output", "output_tokens"),
    ("cache-read", "cache_read_input_tokens"),
    ("cache-write", "cache_creation_input_tokens"),
]

def result_files(issue_dir):
    """Every run`s result in this build directory, oldest first.

    `runs/<n>.json` are the finished ones, moved there by the next run`s start;
    `result.json` is the one that is current, which may be a run still writing
    into it. Sorted NUMERICALLY, not lexically -- `runs/10.json` sorts before
    `runs/2.json` as a string, and the report prints them in run order.
    """
    out = []
    runs = os.path.join(issue_dir, "runs")
    if os.path.isdir(runs):
        numbered = []
        for name in os.listdir(runs):
            stem, ext = os.path.splitext(name)
            if ext != ".json":
                continue
            try:
                numbered.append((int(stem), os.path.join(runs, name)))
            except ValueError:
                numbered.append((1 << 30, os.path.join(runs, name)))
        out.extend(path for _, path in sorted(numbered))
    current = os.path.join(issue_dir, "result.json")
    if os.path.exists(current):
        out.append(current)
    return out

def measure(issue_dir):
    """`(runs, sums)` for one issue.

    A file that does not parse is COUNTED AS A RUN and contributes no figures,
    which is the honest reading: the build ran, and a wrapper seam that does not
    honour `--output-format json` is documented as a thing that degrades. A
    row short of figures is better than an issue missing from the report.
    """
    runs = 0
    sums = {key: 0 for _, key in FIELDS}
    sums["usd"] = 0.0
    sums["turns"] = 0
    for path in result_files(issue_dir):
        runs += 1
        try:
            with open(path) as fh:
                doc = json.load(fh)
        except Exception:
            note("could not read %s; that run contributes no figures" % path)
            continue
        # A list is the stream-json shape and its last element is the result
        # object; a bare object is what `--output-format json` emits. Anything
        # else is not ours.
        if isinstance(doc, list):
            doc = doc[-1] if doc else {}
        if not isinstance(doc, dict):
            note("%s is not a result object; that run contributes no figures" % path)
            continue
        usage = doc.get("usage") or {}
        for _, key in FIELDS:
            value = usage.get(key)
            # `not isinstance(value, bool)` -- `isinstance(True, int)` is True in
            # Python, so a bool anywhere in a `usage` block sums as 1 and the
            # row reads as a measurement. The reader this replaced carried the
            # guard and the rewrite dropped it; found by
            # `/mattpocock-skills:code-review`.
            if isinstance(value, int) and not isinstance(value, bool):
                sums[key] += value
        usd = doc.get("total_cost_usd")
        if isinstance(usd, (int, float)) and not isinstance(usd, bool):
            sums["usd"] += float(usd)
        turns = doc.get("num_turns")
        if isinstance(turns, int) and not isinstance(turns, bool):
            sums["turns"] += turns
    return runs, sums

if not os.path.isdir(root):
    note("no builds under %s yet" % root)
    order = []
else:
    # Numerically, and only the issue-numbered directories: `#9` sorts before
    # `#10` in a report whose rows a person compares.
    order = sorted((n for n in os.listdir(root) if n.isdigit()), key=int)

if wanted:
    asked = set(wanted)
    missing = sorted(asked - set(order), key=int)
    if missing:
        note("no build recorded for #%s" % ", #".join(missing))
    order = [n for n in order if n in asked]

rows = []
for issue in order:
    runs, sums = measure(os.path.join(root, issue))
    rows.append({"issue": issue, "runs": runs, "usd": round(sums["usd"], 4),
                 "turns": sums["turns"],
                 **{key: sums[key] for _, key in FIELDS}})

total = {"runs": sum(r["runs"] for r in rows),
         "turns": sum(r["turns"] for r in rows),
         "usd": round(sum(r["usd"] for r in rows), 4)}
for _, key in FIELDS:
    total[key] = sum(r[key] for r in rows)

if as_json:
    json.dump({"builds_dir": root, "issues": rows, "total": total},
              sys.stdout, indent=2)
    sys.stdout.write("\n")
    raise SystemExit(0)

# No rows, no table. The header and a total of zeros is a MEASUREMENT -- of a
# fleet that spent nothing -- and nothing was measured. The --json above still
# emits its structure, because a consumer has to tell "no data" from a crash.
if not rows:
    raise SystemExit(0)

headers = ["issue", "runs", "usd", "turns"] + [label for label, _ in FIELDS]
keys = ["issue", "runs", "usd", "turns"] + [key for _, key in FIELDS]

def cell(value):
    if isinstance(value, float):
        return "%.2f" % value
    # Thousands separators, because the comparison this report exists for is
    # between numbers six and seven digits long and the eye cannot do that
    # unaided. `--json` is what a machine reads.
    return format(value, ",") if isinstance(value, int) else str(value)

body = [[cell(r[k]) for k in keys] for r in rows]
body.append(["total"] + [cell(total[k]) for k in keys[1:]])
widths = [max(len(h), *(len(r[i]) for r in body)) for i, h in enumerate(headers)]
print("  ".join(h.rjust(w) for h, w in zip(headers, widths)))
for i, r in enumerate(body):
    if i == len(body) - 1:
        # A rule above the total, so the row that is not an issue does not read
        # as one.
        print("  ".join("-" * w for w in widths))
    print("  ".join(c.rjust(w) for c, w in zip(r, widths)))
' "$shape" "${issues[@]+"${issues[@]}"}"

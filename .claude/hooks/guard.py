#!/usr/bin/env python3
"""Block the things a host project cannot afford an agent to do.

A PreToolUse hook: reads the tool call as JSON on stdin, exits 2 to block and
writes the reason to stderr, where Claude reads it and corrects itself. Exit 0
allows.

Five rules, and each one is here because asking for it is not good enough:

  1. an agent does not merge a pull request
  2. an agent does not force-push the default branch
  3. an agent does not write a secret, or a path the project pinned
  4. nothing goes outward while the fleet is stopped
  5. an agent does not submit the review of its own pull request

It does NOT fail open. An unreadable payload, a missing key, an unparseable
command: all of those block, because a guard that quietly stops guarding when
something upstream changes shape is worse than no guard at all.

Commands are shell-tokenised with shlex rather than matched as raw strings, so
`git -C /some/path push --force origin main` is caught and
`git commit -m "note about --force pushes"` is not. Compound commands are split
on `;`, `&&`, `||` and `|` and each segment is judged on its own, and `bash -c`
is recursed into.

What this is NOT: a sandbox. It reads a command and decides; it does not confine
one. A session that means to get past it can. The line it holds is the ROUTINE
one -- the shapes an agent reaches for when it is solving the problem in front of
it rather than working around a rule. Past that, the backstops are the diff, the
merge gate, and the person who merges.

Writing a file is judged through the editing tools (Edit, Write, NotebookEdit),
which is how an agent actually applies a diff. The shell half used to enumerate
seven write verbs and still missed `patch -p1` and `git apply`
(armaatus/autofleet#40); a partial model of writing is a guard that reports
success on the spellings it does not know, so it is gone rather than extended.

    ./.claude/hooks/guard.py --selftest
"""

import contextlib
import io
import json
import os
import re
import shlex
import subprocess
import sys

# What THIS project asks the guard to protect, over and above the four rules.
# autofleet ships no knowledge of any one repo, so the project-specific half is
# a file: `.autofleet/guard.json` in the repo root, every key optional.
#
#   {
#     "default_branch": "trunk",
#     "protected_paths": [
#       {"path": "server/contract/captures/",
#        "tail": "captures/",
#        "reason": "the pinned API contract; a test diffs a live probe against it"}
#     ],
#     "secret_suffixes": ["/config.ini"],
#     "secret_contains": ["/token.dat"],
#     "secret_tails":    ["token.dat", "config.ini"]
#   }
#
# A malformed file is FATAL rather than ignored. A guard that silently falls back
# to "protect nothing" on a typo reports success on every write it was installed
# to stop.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GUARD_CONFIG = os.environ.get(
    "AUTOFLEET_GUARD_CONFIG", os.path.join(REPO_ROOT, ".autofleet", "guard.json")
)


def _load_guard_config(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path) as handle:
            loaded = json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"guard.py: cannot read {path}: {exc}", file=sys.stderr)
        sys.exit(2)
    if not isinstance(loaded, dict):
        print(f"guard.py: {path} must hold a JSON object", file=sys.stderr)
        sys.exit(2)
    return loaded


PROJECT = _load_guard_config(GUARD_CONFIG)


def _derive(project):
    """Everything read off the project config, as one tuple.

    One function so the selftest can swap the whole set at once: a fixture that
    replaced four globals and forgot the fifth would assert against a mixture of
    the fixture and whatever the host repo happens to configure.
    """
    paths = tuple(
        (
            entry["path"],
            entry.get("tail", entry["path"].rstrip("/").rsplit("/", 1)[-1] + "/"),
            entry.get("reason", "protected by this project's .autofleet/guard.json"),
        )
        for entry in project.get("protected_paths", [])
    )
    # `.env` is generated -- there is a script for it -- and every secret file is
    # gitignored, so a session with a reason to write one has a bug.
    suffixes = ("/.env", ".env") + tuple(project.get("secret_suffixes", []))
    contains = tuple(project.get("secret_contains", []))
    # The same files matched by a distinctive tail, so a relative path after a
    # `cd` is still recognised.
    tails = tuple(project.get("secret_tails", []))
    return paths, suffixes, contains, tails, tuple(t for _, t, _ in paths) + tails


(PROTECTED_PATHS, SECRET_SUFFIXES, SECRET_CONTAINS,
 SECRET_TAILS, PROTECTED_TAILS) = _derive(PROJECT)

# The branch whose history is the audit trail. Configurable because hard rule 2
# says a project detail arrives through configuration: a host whose default
# branch is `trunk` gets the rule, not an exemption from it.
DEFAULT_BRANCH = os.environ.get(
    "AUTOFLEET_DEFAULT_BRANCH", PROJECT.get("default_branch", "main")
)

# The fleet's state, shared by every worktree because a stop has to reach all of
# them. See scripts/fleet/fleet.sh.
FLEET_DIR = os.environ.get(
    "AUTOFLEET_DIR", os.path.join(os.path.expanduser("~"), ".autofleet")
)
STOP_FILE = os.path.join(FLEET_DIR, "STOP")
# One file per issue the dispatcher is running, holding that worktree's path.
OWNED_DIR = os.path.join(FLEET_DIR, "worktrees")

# What a stopped fleet must not do. Reading, building and testing stay open --
# the point of a stop is that nothing further reaches the outside world, not
# that the machine freezes.
OUTWARD = (
    ("git", "push"),
    ("gh", "pr", "create"),
    ("gh", "pr", "comment"),
    ("gh", "pr", "review"),
    # Arming an auto-merge is an outward effect: it hands GitHub an instruction
    # that outlives the stop. The merge rule below deliberately lets `--auto`
    # past, which is right when the fleet is running and wrong when it is not.
    ("gh", "pr", "merge"),
    ("gh", "pr", "close"),
    ("gh", "pr", "edit"),
    ("gh", "issue", "close"),
    ("gh", "issue", "create"),
    ("gh", "issue", "comment"),
    ("gh", "issue", "edit"),
    ("gh", "workflow", "run"),
    ("gh", "run", "rerun"),
    ("gh", "api"),
)

# `gh api` is both halves of the API: reading a PR and merging one. Only the
# write methods are outward, and blocking the reads would stop an agent finding
# out what it was in the middle of.
API_WRITE_METHODS = ("POST", "PUT", "PATCH", "DELETE")

# ...and `gh api` sends POST implicitly the moment any field is present, so a
# request with no -X can still be a write. A GraphQL mutation is the same thing
# wearing a different hat.
API_FIELD_FLAGS = ("-f", "-F", "--field", "--raw-field", "--input")


def _repo_root():
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, timeout=5,
        )
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


def _fleet_owns_this_worktree():
    """Was this worktree opened by the dispatcher rather than by a person?

    Rule 5 is for the automatic flow only. Somebody reviewing a pull request
    from their own checkout is the ordinary case, and a guard that argues about
    it is a guard people route around.
    """
    root = _repo_root()
    if not root or not os.path.isdir(OWNED_DIR):
        return False
    try:
        for entry in os.listdir(OWNED_DIR):
            with open(os.path.join(OWNED_DIR, entry)) as fh:
                if os.path.realpath(fh.read().strip()) == os.path.realpath(root):
                    return True
    except OSError:
        return False
    return False


def deny(message):
    print(message, file=sys.stderr)
    sys.exit(2)


def _words(command):
    """The command's tokens, best effort.

    shlex on an unbalanced quote raises; falling back to a whitespace split is
    still better than giving up, because giving up here means allowing.
    """
    try:
        return shlex.split(command)
    except ValueError:
        return command.split()


def _is_git(words, verb):
    """`git <verb>`, allowing git's own global options in between."""
    if not words:
        return False
    if words[0].rsplit("/", 1)[-1] != "git":
        return False
    i = 1
    while i < len(words):
        w = words[i]
        if w in ("-C", "-c", "--git-dir", "--work-tree", "--namespace"):
            i += 2
            continue
        if w.startswith("-"):
            i += 1
            continue
        return w == verb
    return False


def _gh_rest(words):
    """`gh`'s subcommand tokens, with its own global options stripped.

    `gh -R owner/repo pr merge 26` is `gh pr merge`, and matching on `words[1:3]`
    said otherwise -- so every rule naming a subcommand was one `-R` away from
    not applying.

    Only for deciding WHICH SUBCOMMAND this is. Rules that inspect flags keep
    reading the unstripped list, because that is where the flags still are.
    """
    if not words or words[0].rsplit("/", 1)[-1] != "gh":
        return []
    rest, i = [], 1
    while i < len(words):
        w = words[i]
        # The only global option that takes a separate value. `--repo=x` is one
        # token and falls through to the generic skip below.
        if w in ("-R", "--repo"):
            i += 2
            continue
        if w.startswith("-"):
            i += 1
            continue
        rest.append(w)
        i += 1
    return rest


def _verb(words):
    return words[0].rsplit("/", 1)[-1] if words else ""


def _git_c_dir(words):
    """The directory `git -C <dir>` would act in, if any."""
    for i, w in enumerate(words):
        if w == "-C" and i + 1 < len(words):
            return words[i + 1]
        if w.startswith("--git-dir="):
            return w.split("=", 1)[1]
    return None


def _current_branch(cwd=None):
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5, cwd=cwd,
        )
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


# `;` `&&` `||` `|` and newlines separate commands; only the first word of each
# is the verb. Judging the whole string as one command is how `x && gh pr merge`
# reads as an invocation of `x`.
_SEGMENT_SPLIT = re.compile(r"(?:\|\||&&|\||;|\n)")

_HEREDOC = re.compile(r"<<-?\s*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1")


def _strip_heredocs(command):
    """Drop heredoc BODIES before splitting.

    A heredoc body is data. Splitting it on newlines and judging every line as a
    command is how writing documentation about this file trips this file: a line
    quoting a blocked command inside a doc is prose, not the command.
    """
    lines = command.split("\n")
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        m = _HEREDOC.search(line)
        i += 1
        if not m:
            continue
        delim = m.group(2)
        while i < len(lines) and lines[i].strip() != delim:
            i += 1
        i += 1  # the delimiter line itself
    return "\n".join(out)


def _segments(command):
    for raw in _SEGMENT_SPLIT.split(_strip_heredocs(command)):
        raw = raw.strip()
        if raw:
            yield raw


def _gh_api_writes(words):
    """Does this `gh api` call change anything?

    "No -X" is not "harmless read": gh sends GET by default and POST the moment
    any field is present, and a GraphQL mutation carries no method at all.
    """
    method = ""
    writes = False
    for i, w in enumerate(words):
        if w in ("-X", "--method") and i + 1 < len(words):
            method = words[i + 1].upper()
        elif w.startswith("--method="):
            method = w.split("=", 1)[1].upper()
        elif w in API_FIELD_FLAGS or any(w.startswith(f + "=") for f in API_FIELD_FLAGS):
            writes = True
        elif "mutation" in w and "graphql" in " ".join(words):
            writes = True
    return method in API_WRITE_METHODS or writes


def _check_stopped(words):
    """Rule 4: nothing outward while ~/.autofleet/STOP exists."""
    if not os.path.exists(STOP_FILE):
        return
    gh_sub = _gh_rest(words)
    for prefix in OUTWARD:
        if prefix[0] == "gh":
            matched = ["gh"] + gh_sub[: len(prefix) - 1] == list(prefix)
        else:
            head = [w.rsplit("/", 1)[-1] for w in words[: len(prefix)]]
            matched = head == list(prefix) or _is_git(words, prefix[1])
        if not matched:
            continue
        if prefix == ("gh", "api") and not _gh_api_writes(words):
            continue
        deny(
            f"Blocked: the fleet is stopped ({STOP_FILE}).\n"
            "Nothing goes out while that file exists -- no push, no PR, no comment.\n"
            "Say where you got to and stop. A person clears it with: "
            "./scripts/fleet/fleet.sh resume"
        )


MERGE_REFUSAL = (
    "Blocked: an agent does not merge a pull request in this repository.\n"
    "Your job ends at an open pull request carrying its closing line. The\n"
    "dispatcher arms auto-merge, the review runs, and GitHub merges on its own\n"
    "rules -- or a person is told why not. See docs/WORKFLOW.md."
)


def _check_merge(words):
    """Rule 1: an agent does not merge, by any of the three spellings."""
    if _verb(words) != "gh":
        return
    rest = words[1:]
    sub = _gh_rest(words)
    if sub[:2] == ["pr", "merge"]:
        # `--auto` does not merge. It asks GitHub to merge later, once the
        # required checks pass -- and the merge gate is one of those, so the
        # conditions in it are what actually decide. `--admin` bypasses them,
        # which is merging.
        if "--auto" in rest and "--admin" not in rest:
            return
        deny(MERGE_REFUSAL)
    if sub[:1] == ["api"] and any(
        re.search(r"/pulls/\d+/merge", w) or "mergePullRequest" in w for w in rest
    ):
        deny(MERGE_REFUSAL + "\n(The REST and GraphQL spellings are the same act.)")


REVIEW_REFUSAL = (
    "Blocked: this worktree was opened by the fleet, and an agent does not "
    "submit the\n"
    "independent review of its own pull request.\n"
    "\n"
    "The verdict is what releases the merge gate, and the reviewer signs in as "
    "the same\n"
    "GitHub account you do -- so the only thing separating its verdict from "
    "yours is a\n"
    "marker `review.sh` writes from a schema-validated field. This is what "
    "keeps that\n"
    "marker out of your reach; a branch that could review itself could certify "
    "itself.\n"
    "\n"
    "Your job ends at an open pull request. To watch the verdict land:\n"
    "  ./scripts/fleet/await-review.sh"
)

# The GraphQL names for the same act. A PENDING review submitted later is a
# review too. `resolveReviewThread` is deliberately NOT here: whether every
# thread is resolved is branch protection, and GitHub decides it.
REVIEW_MUTATIONS = ("addPullRequestReview", "submitPullRequestReview")


def _check_review(words):
    """Rule 5: the author does not write the verdict that clears its own gate.

    Four spellings, because the rule is worth exactly as much as its narrowest
    one: the porcelain, the REST route, the GraphQL mutation, and a field whose
    value this hook cannot read at all.
    """
    if _verb(words) != "gh" or not _fleet_owns_this_worktree():
        return
    rest = words[1:]
    sub = _gh_rest(words)
    if sub[:2] == ["pr", "review"]:
        deny(REVIEW_REFUSAL)
    if sub[:1] != ["api"]:
        return
    # A body this cannot READ is a body it must not allow. `gh api` treats a
    # field value beginning with `@` as a FILENAME, and `--input` takes a plain
    # path with no `@` at all -- either puts the whole mutation out of sight,
    # which is how a verdict gets forged. Inline queries are unaffected.
    if any(w == "--input" or w.startswith("--input=") or w.startswith("@")
           or re.match(r"^[A-Za-z_][A-Za-z0-9_]*=@", w) for w in rest):
        deny(
            "Blocked: `gh api` with a field read from a FILE, from a worktree the "
            "fleet\n"
            "opened. The rules here are about what the call does, and a body this "
            "hook\n"
            "cannot read is one it cannot judge. Put the query on the command "
            "line instead."
        )
    if any(m in w for w in rest for m in REVIEW_MUTATIONS):
        deny(REVIEW_REFUSAL + "\n(That GraphQL mutation is the same act by "
             "another name.)")
    if any(re.search(r"/pulls/\d+/reviews", w) for w in rest) and _gh_api_writes(words):
        deny(REVIEW_REFUSAL + "\n(That is the same act by its REST name; a GET "
             "of the same path is fine.)")


def _check_force_push(words, branch_cache):
    """Rule 2: a force-push to the default branch rewrites the audit trail."""
    if not _is_git(words, "push"):
        return
    forced = any(
        w in ("--force", "-f", "--force-with-lease")
        or w.startswith("--force-with-lease=")
        or (len(w) > 1 and w[0] == "-" and not w.startswith("--") and "f" in w)
        for w in words[1:]
    )
    if not forced:
        return
    # Everything after the `push` verb. "words[1:] minus flags" swept in the verb
    # itself and the ARGUMENT of a global option, so `git -C main push --force
    # origin HEAD` read as targeting main.
    try:
        after = words[words.index("push") + 1:]
    except ValueError:
        after = words[1:]
    refs = [w for w in after if not w.startswith("-")]
    b = DEFAULT_BRANCH
    targets = any(
        r == b or r.startswith(b + ":") or r.startswith("+" + b)
        or r.endswith(":" + b) or r.endswith(":refs/heads/" + b)
        or r == "refs/heads/" + b
        for r in refs
    )
    # `git push -f origin HEAD` is the commoner idiom than naming the branch,
    # and `git push -f` with no refspec at all is the same rewrite.
    if not targets and (any(r == "HEAD" for r in refs) or len(refs) <= 1):
        if branch_cache[0] is None:
            branch_cache[0] = _current_branch(_git_c_dir(words))
        targets = branch_cache[0] == b
    if targets:
        deny(
            f"Blocked: force-pushing {b} rewrites the commit chain that is this\n"
            "project's audit trail. Push to your branch instead, or ask a person to\n"
            "do it deliberately."
        )


def check_bash(command):
    branch_cache = [None]
    for segment in _segments(command):
        words = _words(segment)
        if not words:
            continue
        # `bash -c "..."` is a command in an argument. Judge what it will run.
        if _verb(words) in ("bash", "sh", "zsh", "dash") and "-c" in words:
            idx = words.index("-c")
            if idx + 1 < len(words):
                check_bash(words[idx + 1])
            continue
        _check_stopped(words)
        _check_merge(words)
        _check_review(words)
        _check_force_push(words, branch_cache)
    return 0


def _protected_tail(path):
    """The protected marker this path ends in, if any.

    Suffix rather than prefix: the tool may name a relative path, so the only
    reliable part of it is its tail.
    """
    normalised = path.replace("//", "/").lstrip("./")
    for tail in PROTECTED_TAILS:
        if tail.endswith("/"):
            if ("/" + normalised).find("/" + tail) >= 0 or normalised.startswith(tail):
                return tail
        elif normalised == tail or normalised.endswith("/" + tail):
            return tail
    return None


def check_path(path):
    """Rule 3: secrets, and the paths this project pinned."""
    normalised = path if path.startswith("/") else "/" + path
    tail = _protected_tail(path)

    if (normalised.endswith(SECRET_SUFFIXES)
            or any(m in normalised for m in SECRET_CONTAINS)
            or (tail and tail in SECRET_TAILS)):
        deny(
            f"Blocked: {path} holds per-worktree secrets, and none of them belong in\n"
            "the tree. .env is generated -- regenerate it with "
            "./scripts/fleet/env.sh rather than editing it."
        )

    for where, protected_tail, reason in PROTECTED_PATHS:
        if where in normalised or (protected_tail and tail == protected_tail):
            # The reason is the whole value of the rule: "blocked" with no why
            # sends the agent looking for a way around it.
            deny(
                f"Blocked: {where} is {reason}.\n"
                "Editing it to match a failing run silences whatever it exists to "
                "catch.\n"
                "Say in the PR body what changed and why, or ask for it to be "
                "regenerated the way this project regenerates it."
            )
    return 0


def main(payload):
    tool = payload.get("tool_name") or ""
    tool_input = payload.get("tool_input") or {}

    if tool == "Bash":
        command = tool_input.get("command")
        if not isinstance(command, str) or not command.strip():
            deny(
                "Blocked: this Bash call carries no readable command, so "
                ".claude/hooks/guard.py\n"
                "cannot tell whether it is allowed. Refusing rather than allowing an "
                "unexamined command."
            )
        return check_bash(command)

    # NotebookEdit names its target notebook_path, not file_path. Reading only
    # file_path is how a matcher ends up promising coverage it does not have.
    path = tool_input.get("file_path") or tool_input.get("notebook_path")
    if not isinstance(path, str) or not path:
        # A tool the matcher caught that names no path -- nothing here applies.
        return 0
    return check_path(path)


# Every rule above has a row here, and hard rule 3 is why: a rule with no
# assertion is a rule that can stop holding in silence. The list is the record of
# what has been checked -- it is not a proof that nothing else gets through; see
# the module docstring.
SELFTEST = [
    # (tool, tool_input, expected exit, what it proves)

    # --- 1. merging ---------------------------------------------------------
    ("Bash", {"command": "gh pr merge 42 --squash"}, 2, "an agent cannot merge its own PR"),
    ("Bash", {"command": "gh pr merge 42 --auto --squash"}, 0,
     "...but it may ASK GitHub to merge once the required checks pass"),
    ("Bash", {"command": "gh pr merge 42 --auto --squash --admin"}, 2,
     "...and --admin, which bypasses those checks, is still merging"),
    ("Bash", {"command": "gh  pr  merge 12"}, 2, "...however it is spaced"),
    ("Bash", {"command": "/opt/homebrew/bin/gh pr merge 12"}, 2, "...through an absolute gh"),
    ("Bash", {"command": "gh -R o/r pr merge 12"}, 2, "...past gh's own global options"),
    ("Bash", {"command": "gh api -X PUT repos/o/r/pulls/12/merge"}, 2, "...spelled as the REST call"),
    ("Bash", {"command": "gh api graphql -f query='mutation{ mergePullRequest(x) }'"}, 2,
     "...and by its GraphQL name"),
    ("Bash", {"command": "true && gh pr merge 3"}, 2, "a second segment is judged too"),
    ("Bash", {"command": 'bash -c "gh pr merge 3"'}, 2, "...and so is bash -c"),
    ("Bash", {"command": "gh pr create --title x --body y"}, 0, "opening a PR is allowed"),
    ("Bash", {"command": "gh api repos/o/r/pulls/12"}, 0, "reading a PR over the API is allowed"),
    ("Bash", {"command": 'git commit -m "note: do not gh pr merge yourself"'}, 0,
     "a commit message is not a command"),
    ("Bash", {"command": 'grep -rn "gh pr merge" CLAUDE.md'}, 0,
     "grepping for a blocked command is allowed"),
    ("Bash", {"command": "cat > /tmp/doc.md <<'EOF'\ngh pr merge 1\nEOF"}, 0,
     "a heredoc body is data -- documenting a blocked command is not running it"),

    # --- 2. force-pushing the default branch --------------------------------
    # The fixture's default branch is `trunk`, so these rows fail against a rule
    # that hardcodes a branch name instead of reading `.autofleet/guard.json`.
    ("Bash", {"command": "git push --force origin trunk"}, 2, "force-pushing the default branch is blocked"),
    ("Bash", {"command": "git push origin trunk -f"}, 2, "...with the flag last"),
    ("Bash", {"command": "git push --force origin refs/heads/trunk"}, 2, "...spelled as a full ref"),
    ("Bash", {"command": "git -C /w/demo push --force-with-lease origin trunk"}, 2,
     "...through git's global options"),
    ("Bash", {"command": "git push --force origin main"}, 0,
     "...and `main` is an ordinary branch where the default is not main"),
    ("Bash", {"command": "git -C /w/trunk push --force origin some-branch"}, 0,
     "a -C path containing the branch name is not a refspec"),
    ("Bash", {"command": "git push --force origin armaatus/fix-trunk-loop"}, 0,
     "a branch whose name contains the default's is fine"),
    ("Bash", {"command": "git push -u origin armaatus/thing"}, 0, "an ordinary push is fine"),

    # --- 3. secrets, and the project's own protected paths ------------------
    # Driven by SELFTEST_PROJECT below rather than by anything hardcoded here:
    # these rows prove the .autofleet/guard.json mechanism, using the pinned-
    # contract rule autofleet was extracted from (armaatus/rommsync-nx) as the
    # worked example.
    ("Edit", {"file_path": "/w/demo/.env"}, 2, "secrets are not editable"),
    ("Edit", {"file_path": "/w/demo/server/testing/fixture-auth.env"}, 2,
     "...including a project's fixture credentials"),
    ("Write", {"file_path": "/w/demo/token.dat"}, 2, "...and a stored token"),
    ("NotebookEdit", {"notebook_path": "/w/demo/.env"}, 2,
     "NotebookEdit names its target notebook_path, and is guarded too"),
    ("Edit", {"file_path": "/w/demo/server/contract/captures/login.json"}, 2,
     "a pinned capture is not hand-edited"),
    ("Write", {"file_path": "captures/login.json"}, 2, "...by a relative path, matched on its tail"),
    ("Edit", {"file_path": "/w/demo/.claude/hooks/guard.py"}, 0,
     "the guards themselves are editable -- the diff and the merge gate are their control"),
    ("Edit", {"file_path": "/w/demo/src/app.c"}, 0, "ordinary source files are editable"),
    ("Edit", {"file_path": "/w/demo/.claude/agents/reviewer.md"}, 0, "so are the subagents"),

    # --- 5. reviewing, outside a fleet worktree -----------------------------
    ("Bash", {"command": "gh pr review 7 --approve"}, 0,
     "a person reviewing from their own checkout is the ordinary case"),
    ("Bash", {"command": "gh api graphql --input /tmp/m.json"}, 0,
     "...and so is any other gh api call they make"),

    # --- payloads -----------------------------------------------------------
    ("Bash", {}, 2, "a Bash call with no command is refused, not allowed"),
    ("Edit", {}, 0, "a matched tool that names no path has nothing here to answer to"),
]

# Rule 5, which holds only in a worktree the dispatcher opened. Same
# (command, want, what) shape as STOPPED_CASES; _stateful_checks fabricates the
# ownership record around them.
OWNED_CASES = [
    ("gh pr review 7 --comment --body x", 2,
     "an agent does not submit the review of its own pull request"),
    ("gh -R o/r pr review 7 --approve", 2, "...past gh's own global options"),
    ("gh api -X POST repos/o/r/pulls/7/reviews -f body=x", 2,
     "...nor by the REST route that writes the same record"),
    ("gh api graphql -f query='mutation{ addPullRequestReview(x) }'", 2,
     "...nor as the GraphQL mutation"),
    ("gh api graphql --input /tmp/m.json", 2,
     "...nor with the body in a file this hook cannot read"),
    ("gh api graphql -F query=@/tmp/m.gql", 2, "...however that file is named"),
    ("gh api repos/o/r/pulls/7/reviews", 0,
     "reading the reviews is how await-review.sh works, and stays open"),
    ("gh api graphql -f query='mutation{ resolveReviewThread(x) }'", 0,
     "resolving a thread is branch protection's business, not this rule's"),
    ("gh pr create --title x --body y", 0, "...and opening the pull request is the job"),
]

# The rules that depend on a FILE rather than on the command alone. Same
# (input, want, what) shape as SELFTEST; the state is built and torn down around
# them by _stateful_checks.
STOPPED_CASES = [
    ("git push origin HEAD", 2, "a stopped fleet pushes nothing"),
    ("gh pr create --title x --body y", 2, "...and opens no pull request"),
    ("gh -R o/r pr comment 3 --body hi", 2, "...and comments on none, past gh's globals"),
    ("gh api -X POST repos/o/r/issues/3/comments -f body=hi", 2, "...nor by the REST spelling"),
    ("gh api graphql -f query='mutation{ addComment(x) }'", 2, "...nor as a GraphQL mutation"),
    ("gh api repos/o/r/pulls/3", 0, "reading stays open: a stop is not a freeze"),
    ("./tests/run.sh", 0, "...and so does building and testing"),
]

# The project config the selftest runs against. Not the host repo's own
# `.autofleet/guard.json`: the rows above assert exact refusals, and a suite
# whose expectations come from whatever file happens to be on disk asserts
# nothing. It doubles as the worked example of what a project puts in that file.
SELFTEST_PROJECT = {
    # NOT "main", which is the built-in default: a fixture that agrees with the
    # fallback cannot tell a rule that READS this key from one that ignores it,
    # and every force-push row below would have passed against a hardcoded
    # branch name. Hard rule 2 says the branch arrives through configuration;
    # this is what asserts that it does. Verified by hardcoding it and watching
    # four rows go red.
    "default_branch": "trunk",
    "protected_paths": [
        {
            "path": "server/contract/captures/",
            "tail": "captures/",
            "reason": "the pinned API contract a test diffs a live probe against",
        }
    ],
    "secret_suffixes": ["/config.ini"],
    "secret_contains": ["/token.dat", "/device.dat"],
    "secret_tails": ["token.dat", "device.dat", "config.ini"],
}


def _run(tool, tool_input):
    """One case, with the refusal text swallowed.

    Every blocking row prints its reason to stderr, and forty of them buries the
    one line that matters -- which assertion failed. The failures below print
    their own message, so nothing is lost.
    """
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            main({"tool_name": tool, "tool_input": tool_input})
        return 0
    except SystemExit as exc:
        return exc.code


def _bash_cases(cases):
    failures = 0
    for command, want, what in cases:
        got = _run("Bash", {"command": command})
        if got != want:
            print(f"FAIL: {what} (expected exit {want}, got {got})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: {what}")
    return failures


# The refusals that are not a tool call at all: a payload this cannot read, and a
# project config it cannot parse. Both are the not-failing-open rule, and both
# were the only rules in this file with nothing asserting them.
def _plumbing_checks():
    import tempfile
    failures = 0
    cases = [
        ('{"tool_name": "Bash", "tool_input": {"command": "ls"}}', None,
         "a well-formed payload is read and judged"),
        ("not json at all", 2, "a payload this cannot parse is refused, not allowed"),
        ('["a", "list"]', 2, "...and so is one that is not an object"),
        ("", 2, "...and an empty one"),
    ]
    for raw, want, what in cases:
        try:
            with contextlib.redirect_stderr(io.StringIO()):
                parse_payload(raw)
            got = None
        except SystemExit as exc:
            got = exc.code
        if got != want:
            print(f"FAIL: {what} (expected exit {want}, got {got})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: {what}")

    # A malformed `.autofleet/guard.json` is FATAL rather than ignored: falling
    # back to "protect nothing" on a typo reports success on every write the
    # rule was installed to stop.
    with tempfile.TemporaryDirectory() as tmp:
        for body, what in (
            ("{ not json", "a guard.json that does not parse is fatal, not ignored"),
            ('["a", "list"]', "...and so is one that is not an object"),
        ):
            path = os.path.join(tmp, "guard.json")
            with open(path, "w") as fh:
                fh.write(body)
            try:
                with contextlib.redirect_stderr(io.StringIO()):
                    _load_guard_config(path)
                got = None
            except SystemExit as exc:
                got = exc.code
            if got != 2:
                print(f"FAIL: {what} (expected exit 2, got {got})", file=sys.stderr)
                failures += 1
            else:
                print(f"  ok: {what}")
        # ...while an ABSENT one is the ordinary case: a host that configures
        # nothing still gets the four universal rules.
        if _load_guard_config(os.path.join(tmp, "nothing-here.json")) != {}:
            print("FAIL: an absent guard.json is not the same as a broken one",
                  file=sys.stderr)
            failures += 1
        else:
            print("  ok: an absent guard.json leaves the universal rules in place")
    return failures


PLUMBING_ASSERTIONS = 7


def _stateful_checks():
    """STOPPED_CASES and OWNED_CASES, against state this function controls.

    The rows above assert what the guard does for an ordinary session -- "an
    ordinary push is fine". Run against a real `~/.autofleet/STOP` those are not
    merely untrue, they are untrue BY DESIGN, so the selftest would go red on
    every machine whose fleet happened to be drained. It controls the state
    instead.
    """
    import tempfile
    global STOP_FILE, OWNED_DIR
    saved = (STOP_FILE, OWNED_DIR)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        # Nothing is owned while the stop rows run, and nothing is stopped while
        # the ownership rows do: a fixture that set both would let either rule
        # pass for the other one's reason.
        OWNED_DIR = os.path.join(tmp, "unowned")
        STOP_FILE = os.path.join(tmp, "STOP")
        open(STOP_FILE, "w").close()
        try:
            failures += _bash_cases(STOPPED_CASES)
        finally:
            STOP_FILE = saved[0]

        OWNED_DIR = os.path.join(tmp, "worktrees")
        os.mkdir(OWNED_DIR)
        with open(os.path.join(OWNED_DIR, "7"), "w") as fh:
            fh.write(_repo_root() or os.getcwd())
        try:
            failures += _bash_cases(OWNED_CASES)
        finally:
            OWNED_DIR = saved[1]
    return failures


def selftest():
    global PROJECT, PROTECTED_PATHS, SECRET_SUFFIXES, SECRET_CONTAINS
    global SECRET_TAILS, PROTECTED_TAILS, STOP_FILE, OWNED_DIR, DEFAULT_BRANCH
    import tempfile

    PROJECT = SELFTEST_PROJECT
    (PROTECTED_PATHS, SECRET_SUFFIXES, SECRET_CONTAINS,
     SECRET_TAILS, PROTECTED_TAILS) = _derive(PROJECT)
    DEFAULT_BRANCH = PROJECT["default_branch"]

    failures = 0
    saved = (STOP_FILE, OWNED_DIR)
    with tempfile.TemporaryDirectory() as empty:
        # Nothing is stopped and nothing is owned for the stateless rows,
        # whatever this machine's own fleet is doing. Left alone, those rows
        # would assert the opposite of themselves inside a fleet worktree --
        # which is where they matter most.
        STOP_FILE = os.path.join(empty, "STOP")
        OWNED_DIR = os.path.join(empty, "worktrees")
        try:
            for tool, tool_input, want, what in SELFTEST:
                got = _run(tool, tool_input)
                if got != want:
                    print(f"FAIL: {what} (expected exit {want}, got {got})", file=sys.stderr)
                    failures += 1
                else:
                    print(f"  ok: {what}")
        finally:
            STOP_FILE, OWNED_DIR = saved
    failures += _stateful_checks()
    failures += _plumbing_checks()
    if failures:
        print(f"{failures} guard assertion(s) failed", file=sys.stderr)
        return 1
    print(f"{len(SELFTEST) + len(STOPPED_CASES) + len(OWNED_CASES) + PLUMBING_ASSERTIONS} "
          "guard assertions hold")
    return 0


def parse_payload(raw):
    """The tool call, or a refusal. A FUNCTION so it can carry a selftest row.

    This is the not-failing-open rule itself -- the one the module docstring
    opens with -- and while it lived inline under `__main__` it was the only
    refusal in the file with nothing asserting it.
    """
    try:
        parsed = json.loads(raw)
    except Exception:
        deny(
            "Blocked: .claude/hooks/guard.py could not read this tool call, so it "
            "cannot tell\n"
            "whether it is allowed. Refusing rather than allowing an unexamined action."
        )
    if not isinstance(parsed, dict):
        deny("Blocked: .claude/hooks/guard.py got a tool call that is not an object.")
    return parsed


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    sys.exit(main(parse_payload(sys.stdin.read())))

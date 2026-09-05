#!/usr/bin/env python3
"""Block the things this repo cannot afford an agent to do.

A PreToolUse hook: reads the tool call as JSON on stdin, exits 2 to block and
writes the reason to stderr, where Claude reads it and corrects itself. Exit 0
allows.

CLAUDE.md and the skills beside this file are advisory -- a session can read them
and still do the thing. Everything here is a rule where "rare" is not good
enough, so it is enforced instead of asked for.

It does NOT fail open. An unreadable payload, a missing key, an unparseable
command: all of those block, because a guard that quietly stops guarding when
something upstream changes shape is worse than no guard at all -- nothing on
screen says the enforcement went away.

Commands are shell-tokenised with shlex rather than matched as raw strings, so
`git -C /some/path push --force origin main` is caught and
`git commit -m "note about --force pushes"` is not. Compound commands are split
on `;`, `&&`, `||` and `|` and each segment is judged on its own, and `bash -c`
is recursed into.

What this is NOT: a sandbox. It reads a command and decides; it does not confine
one. A session that means to get past it can -- an interpreter one-liner that
opens a file, a path assembled from a variable, an `exec` through something this
does not model. The line it holds is the ROUTINE one: the heredoc, the redirect,
the `sed -i`, the `gh pr merge`, the shapes an agent reaches for when it is
solving the problem in front of it rather than working around a rule. Past that,
the backstops are the diff and the human who merges. Do not write documentation
that claims more than this.

    ./.claude/hooks/guard.py --selftest
"""

import json
import os
import re
import shlex
import subprocess
import sys

CAPTURES = "server/contract/captures/"

# Files that hold a real bearer token or key. Every one is gitignored, and a
# session with a reason to write one has a bug. `.env` is generated -- there is
# a script for it.
SECRET_SUFFIXES = ("/config.ini", "/.env", ".env")
SECRET_CONTAINS = ("/token.dat", "/device.dat")

# The enforcement layer itself. An agent that can rewrite its own guards has no
# guards; this is the line between "advisory" and "enforced" that CLAUDE.md
# documents, and it has to be enforced by something the agent cannot reach.
# Skills and subagents are deliberately NOT in here: they are advisory by
# design, they are reviewed in the diff like anything else, and an agent
# improving one is the loop working.
SELF_PROTECTED = (
    "/.claude/hooks/",
    "/.claude/settings.json",
    # Gitignored, so a permission rule written here appears in no diff. That is
    # exactly why it needs the guard the committed file has.
    "/.claude/settings.local.json",
)

# The same targets, matched loosely enough to survive a relative path. A shell
# command can `cd` first, so `captures/login.json` and
# `server/contract/captures/login.json` are the same file and only one of them
# looks like it. Suffix-matching a distinctive tail is imperfect -- see the
# honesty note in the module docstring -- but it is what makes the routine forms
# reachable at all.
PROTECTED_TAILS = (
    "captures/",
    ".claude/hooks/",
    ".claude/settings.json",
    ".claude/settings.local.json",
    "workflows/unblock.yml",
    "token.dat",
    "device.dat",
    "config.ini",
)


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


def _verb(words):
    return words[0].rsplit("/", 1)[-1] if words else ""


def _touches_captures(word):
    return CAPTURES in word or _protected_tail(word) == "captures/"


def _protected_tail(path):
    """The protected marker this path ends in, if any.

    Suffix rather than prefix: the command may have `cd`-ed first, so the only
    reliable part of a relative path is its tail.
    """
    normalised = path.replace("//", "/").lstrip("./")
    for tail in PROTECTED_TAILS:
        if tail.endswith("/"):
            if ("/" + normalised).find("/" + tail) >= 0 or normalised.startswith(tail):
                return tail
        elif normalised == tail or normalised.endswith("/" + tail):
            return tail
    return None


def _current_branch():
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5,
        )
        return out.stdout.strip() if out.returncode == 0 else ""
    except Exception:
        return ""


# `;` `&&` `||` `|` and newlines separate commands; only the first word of each
# is the verb. Judging the whole string as one command is how `x && rm <secret>`
# reads as an invocation of `x`.
_SEGMENT_SPLIT = re.compile(r"(?:\|\||&&|\||;|\n)")


def _segments(command):
    for raw in _SEGMENT_SPLIT.split(command):
        raw = raw.strip()
        if raw:
            yield raw


REDIRECT = re.compile(r"^(?:\d*|&)?>{1,2}$")


def _written_paths(words):
    """Every path this command creates, replaces, moves or deletes.

    A write is a write whichever verb performs it. The point of collecting them
    is that check_path -- the rules about secrets, the pinned contract, the
    workflow and the hooks themselves -- then applies to a shell command exactly
    as it applies to an Edit. Without this, `cat > .claude/hooks/guard.py`
    rewrites the guard and the guard says nothing.
    """
    written = []
    positional = [w for w in words[1:] if not w.startswith("-")]
    verb = _verb(words)

    # Redirections, attached (`>out`) or detached (`> out`).
    for i, w in enumerate(words):
        if REDIRECT.match(w):
            if i + 1 < len(words):
                written.append(words[i + 1])
        else:
            m = re.match(r"^(?:\d*|&)?>{1,2}(.+)$", w)
            if m:
                written.append(m.group(1))

    if verb in ("rm", "shred", "truncate", "unlink"):
        written += positional
    elif verb == "tee":
        written += positional
    elif verb in ("cp", "install", "ln") and positional:
        written.append(positional[-1])
    elif verb == "mv" and positional:
        # Both ends: a move REMOVES the source. `cp` out is reading; `mv` out is
        # deleting, and the comment that justifies ignoring cp's source does not
        # carry to mv.
        written += positional
    elif verb == "dd":
        written += [w.split("=", 1)[1] for w in words[1:] if w.startswith("of=")]
    elif verb == "sed" and any(
        w == "-i" or (w.startswith("-i") and not w.startswith("--")) for w in words[1:]
    ):
        written += positional
    elif _is_git(words, "rm") or _is_git(words, "restore"):
        written += positional[1:]
    elif _is_git(words, "checkout") and "--" in words:
        written += words[words.index("--") + 1:]

    return [w for w in written if w and not w.startswith("-")]


def check_bash(command):
    branch = None
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

        # "A human merges. Do not merge your own PR." -- CLAUDE.md, "Finishing a
        # task". Separation of duties is the one review control this project
        # has, and an agent that can merge is not separated from anything.
        if _verb(words) == "gh":
            rest = words[1:]
            if rest[:2] == ["pr", "merge"]:
                deny(
                    "Blocked: agents do not merge PRs on this repo (CLAUDE.md, "
                    '"Finishing a task").\n'
                    "A human merges. Open the PR, put the /code-review findings in "
                    "its body, and stop there."
                )
            # The REST spelling of the same thing.
            if rest[:1] == ["api"] and any(
                re.search(r"/pulls/\d+/merge", w) for w in rest
            ):
                deny(
                    "Blocked: `gh api .../pulls/N/merge` is merging a PR, which agents "
                    "do not do\n"
                    'on this repo (CLAUDE.md, "Finishing a task"). A human merges.'
                )

        # A force-push to main rewrites the commit chain, which is this
        # project's audit trail: who asked for what, what the agent produced,
        # who approved it.
        if _is_git(words, "push"):
            forced = any(
                w in ("--force", "-f", "--force-with-lease")
                or w.startswith("--force-with-lease=")
                or (len(w) > 1 and w[0] == "-" and not w.startswith("--") and "f" in w)
                for w in words[1:]
            )
            if forced:
                refs = [w for w in words[1:] if not w.startswith("-")]
                targets_main = any(
                    r == "main"
                    or r.startswith("main:")
                    or r.startswith("+main")
                    or r.endswith(":main")
                    or r.endswith(":refs/heads/main")
                    or r == "refs/heads/main"
                    for r in refs
                )
                # `git push -f origin HEAD` is the commoner idiom than naming the
                # branch, and on main it is the same rewrite.
                if not targets_main and any(r == "HEAD" for r in refs):
                    if branch is None:
                        branch = _current_branch()
                    targets_main = branch == "main"
                # ...and so is `git push -f` with no refspec at all.
                if not targets_main and len(refs) <= 1:
                    if branch is None:
                        branch = _current_branch()
                    targets_main = branch == "main"
                if targets_main:
                    deny(
                        "Blocked: force-pushing main rewrites the commit chain that is "
                        "this project's audit trail.\n"
                        "Push to your branch instead, or ask a human to do it "
                        "deliberately."
                    )

        # Every path this segment writes goes through the same rules an Edit
        # would. A write is a write whichever verb performs it.
        for path in _written_paths(words):
            if _touches_captures(path):
                deny(
                    f"Blocked: {CAPTURES} is the pinned RomM 5.2.0 contract, and "
                    "contract.captures\n"
                    "diffs a live probe against it. Rewriting a capture to match a "
                    "failing run silences\n"
                    "the only test that notices RomM changing.\n"
                    "Re-capture with server/contract/probe_contract.py and say in the "
                    "PR body what changed and why.\n"
                    "Reading a capture is fine -- this blocks writing one."
                )
            check_path(path)

    return 0


def check_path(path):
    normalised = path if path.startswith("/") else "/" + path
    tail = _protected_tail(path)

    for marker in SELF_PROTECTED:
        if marker in normalised or (tail and marker.endswith(tail)):
            deny(
                f"Blocked: {path} is part of the enforcement layer -- the hooks that "
                "decide what an\n"
                "agent may do, and the settings that register them. An agent that can "
                "rewrite its own\n"
                "guards has no guards, which is the line CLAUDE.md draws between "
                "advisory and enforced.\n"
                "A human edits these. Skills and subagents under .claude/ are advisory "
                "and freely editable."
            )

    if (normalised.endswith(SECRET_SUFFIXES)
            or any(m in normalised for m in SECRET_CONTAINS)
            or tail in ("token.dat", "device.dat", "config.ini")):
        deny(
            f"Blocked: {path} holds per-worktree secrets (CLAUDE.md hard rule 5).\n"
            ".env is generated -- regenerate it with ./scripts/orca/env.sh rather than "
            "editing it."
        )

    if CAPTURES in normalised or tail == "captures/":
        deny(
            f"Blocked: {CAPTURES} is the pinned RomM 5.2.0 contract, and "
            "contract.captures\n"
            "diffs a live probe against it. Editing a capture to match a failing run "
            "silences the\n"
            "only test that notices RomM changing.\n"
            "Re-capture with server/contract/probe_contract.py and explain the diff in "
            "the PR body."
        )

    if normalised.endswith("/.github/workflows/unblock.yml") or tail == "workflows/unblock.yml":
        deny(
            "Blocked: unblock.yml derives the blocked/ready labels that decide what "
            "other agents\n"
            'may start (CLAUDE.md, "Working in parallel"). Changing it changes what '
            "three worktrees\n"
            "are allowed to do. A human edits this one."
        )

    return 0


def main(payload):
    tool = payload.get("tool_name") or ""
    tool_input = payload.get("tool_input") or {}

    if tool == "Bash":
        command = tool_input.get("command")
        if not isinstance(command, str) or not command.strip():
            deny(
                "Blocked: this Bash call carries no readable command, so the guards in "
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


SELFTEST = [
    # (tool, tool_input, expected exit, what it proves)
    #
    # Every row here is either a rule this repo depends on or an escape someone
    # actually found. The list is the record of what has been checked -- it is
    # not a proof that nothing else gets through; see the module docstring.

    # --- merging ------------------------------------------------------------
    ("Bash", {"command": "gh pr merge 42 --squash"}, 2, "an agent cannot merge its own PR"),
    ("Bash", {"command": "gh  pr  merge 12"}, 2, "...however it is spaced"),
    ("Bash", {"command": "/opt/homebrew/bin/gh pr merge 12"}, 2, "...through an absolute gh"),
    ("Bash", {"command": "gh api -X PUT repos/o/r/pulls/12/merge"}, 2, "...or spelled as the REST call"),
    ("Bash", {"command": "gh pr create --title x --body y"}, 0, "opening a PR is allowed"),
    ("Bash", {"command": "gh api repos/o/r/pulls/12"}, 0, "reading a PR over the API is allowed"),
    ("Bash", {"command": "ctest --test-dir build --output-on-failure"}, 0, "running the tests is allowed"),
    ("Bash", {"command": 'git commit -m "note: do not gh pr merge yourself"'}, 0, "a commit message is not a command"),
    ("Bash", {"command": 'grep -rn "gh pr merge" CLAUDE.md'}, 0, "grepping for a blocked command is allowed"),

    # --- force-pushing main -------------------------------------------------
    ("Bash", {"command": "git push --force origin main"}, 2, "force-pushing main is blocked"),
    ("Bash", {"command": "git push origin main -f"}, 2, "...with the flag last"),
    ("Bash", {"command": "git push --force origin refs/heads/main"}, 2, "...spelled as a full ref"),
    ("Bash", {"command": "git -C /w/rommsync push --force-with-lease origin main"}, 2, "...through git's global options"),
    ("Bash", {"command": "git push --force origin armaatus/fix-main-loop"}, 0, "a branch whose name contains 'main' is fine"),
    ("Bash", {"command": "git push -u origin armaatus/thing"}, 0, "an ordinary push is fine"),

    # --- the pinned contract ------------------------------------------------
    ("Bash", {"command": "probe.py > server/contract/captures/login.json"}, 2, "redirecting into a capture is blocked"),
    ("Bash", {"command": "probe.py >server/contract/captures/login.json"}, 2, "...with no space after the >"),
    ("Bash", {"command": "probe.py 1> server/contract/captures/login.json"}, 2, "...through an explicit fd"),
    ("Bash", {"command": "cd server/contract && cp /tmp/x captures/login.json"}, 2, "...after a cd, where the path is relative"),
    ("Bash", {"command": "rm server/contract/captures/login.json"}, 2, "deleting a capture is blocked"),
    ("Bash", {"command": "mv server/contract/captures/login.json /tmp/gone.json"}, 2, "moving one AWAY is deleting it, and blocked"),
    ("Bash", {"command": "sed -i '' s/a/b/ server/contract/captures/login.json"}, 2, "editing a capture in place is blocked"),
    ("Bash", {"command": "cp /tmp/new.json server/contract/captures/login.json"}, 2, "copying over a capture is blocked"),
    ("Bash", {"command": "cp server/contract/captures/login.json /tmp/look.json"}, 0, "copying one OUT is reading, and allowed"),
    ("Bash", {"command": "cat server/contract/captures/login.json"}, 0, "reading a capture is allowed"),
    ("Bash", {"command": "diff server/contract/captures/login.json /tmp/new.json > /tmp/d"}, 0, "a diff whose output goes elsewhere is allowed"),
    ("Bash", {"command": "grep -r foo server/contract/captures/"}, 0, "grepping the captures is allowed"),

    # --- the enforcement layer, from the shell ------------------------------
    # The whole class the first version missed: check_path only ran for Edit, so
    # every one of these rewrote a guarded file and said nothing.
    ("Bash", {"command": "cat > .claude/hooks/guard.py"}, 2, "a heredoc cannot rewrite the guard"),
    ("Bash", {"command": "sed -i '' s/deny/allow/ .claude/hooks/guard.py"}, 2, "...nor sed -i"),
    ("Bash", {"command": "sed -i '' s/deny/allow/ .github/workflows/unblock.yml"}, 2, "...nor the blocked/ready workflow"),
    ("Bash", {"command": "echo TOKEN > token.dat"}, 2, "...nor a stored token"),
    ("Bash", {"command": "printf x > .env"}, 2, "...nor .env"),
    ("Bash", {"command": "echo '{}' > .claude/settings.local.json"}, 2, "...nor the gitignored settings override"),
    ("Bash", {"command": "true && rm .claude/hooks/guard.py"}, 2, "a second segment is judged too"),
    ("Bash", {"command": 'bash -c "rm .claude/hooks/guard.py"'}, 2, "...and so is bash -c"),
    ("Bash", {"command": "cat .claude/hooks/guard.py"}, 0, "reading the guard is allowed"),
    ("Bash", {"command": "echo hi > /tmp/note.txt"}, 0, "an ordinary redirect is allowed"),

    # --- payloads -----------------------------------------------------------
    ("Bash", {}, 2, "a Bash call with no command is refused, not allowed"),

    # --- the editing tools --------------------------------------------------
    ("Edit", {"file_path": "/w/rommsync-nx/.env"}, 2, "secrets are not editable"),
    ("Edit", {"file_path": "/w/rommsync-nx/server/testing/fixture-auth.env"}, 2, "...including the fixture credentials"),
    ("Write", {"file_path": "/w/rommsync-nx/token.dat"}, 2, "...and a stored token"),
    ("Edit", {"file_path": "/w/rommsync-nx/server/contract/captures/login.json"}, 2, "a capture is not hand-edited"),
    ("Edit", {"file_path": "/w/rommsync-nx/.github/workflows/unblock.yml"}, 2, "the blocked/ready workflow is not agent-editable"),
    ("Edit", {"file_path": "/w/rommsync-nx/.claude/hooks/guard.py"}, 2, "an agent cannot rewrite its own guards"),
    ("Write", {"file_path": "/w/rommsync-nx/.claude/settings.json"}, 2, "...or the settings that register them"),
    ("Edit", {"file_path": "/w/rommsync-nx/.claude/skills/save-safety/SKILL.md"}, 0, "skills are advisory and stay editable"),
    ("Edit", {"file_path": "/w/rommsync-nx/.claude/agents/verifier.md"}, 0, "so do subagents"),
    ("Edit", {"file_path": "/w/rommsync-nx/core/src/sync.cpp"}, 0, "ordinary source files are editable"),
    ("NotebookEdit", {"notebook_path": "/w/rommsync-nx/.claude/settings.json"}, 2, "NotebookEdit names its target notebook_path, and is guarded too"),
]


def selftest():
    failures = 0
    for tool, tool_input, want, what in SELFTEST:
        try:
            main({"tool_name": tool, "tool_input": tool_input})
            got = 0
        except SystemExit as exc:
            got = exc.code
        if got != want:
            print(f"FAIL: {what} (expected exit {want}, got {got})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: {what}")
    if failures:
        print(f"{failures} guard assertion(s) failed", file=sys.stderr)
        return 1
    print(f"{len(SELFTEST)} guard assertions hold")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv[1:]:
        sys.exit(selftest())
    try:
        raw = sys.stdin.read()
        parsed = json.loads(raw)
    except Exception:
        deny(
            "Blocked: .claude/hooks/guard.py could not read this tool call, so it "
            "cannot tell\n"
            "whether it is allowed. Refusing rather than allowing an unexamined action."
        )
    if not isinstance(parsed, dict):
        deny("Blocked: .claude/hooks/guard.py got a tool call that is not an object.")
    sys.exit(main(parsed))

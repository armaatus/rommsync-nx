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
`git commit -m "note about --force pushes"` is not.

    ./.claude/hooks/guard.py --selftest
"""

import json
import shlex
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
    if not words or words[0] != "git" and not words[0].endswith("/git"):
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


def _touches_captures(word):
    return CAPTURES in word


def check_bash(command):
    words = _words(command)

    # "A human merges. Do not merge your own PR." -- CLAUDE.md, "Finishing a
    # task". Separation of duties is the one review control this project has,
    # and an agent that can merge is not separated from anything.
    if words[:3] == ["gh", "pr", "merge"]:
        deny(
            "Blocked: agents do not merge PRs on this repo (CLAUDE.md, "
            '"Finishing a task").\n'
            "A human merges. Open the PR, put the /code-review findings in its body, "
            "and stop there."
        )

    # A force-push to main rewrites the commit chain, which is this project's
    # audit trail: who asked for what, what the agent produced, who approved it.
    if _is_git(words, "push"):
        forced = any(
            w in ("--force", "-f", "--force-with-lease")
            or w.startswith("--force-with-lease=")
            or (len(w) > 1 and w[0] == "-" and not w.startswith("--") and "f" in w)
            for w in words[1:]
        )
        targets_main = any(
            w == "main" or w.startswith("main:") or w.startswith("+main")
            or w.endswith(":main") or w.endswith(":refs/heads/main")
            for w in words[1:]
        )
        if forced and targets_main:
            deny(
                "Blocked: force-pushing main rewrites the commit chain that is this "
                "project's audit trail.\n"
                "Push to your branch instead, or ask a human to do it deliberately."
            )

    # The pinned RomM snapshot. contract.captures diffs a live probe against
    # these files, so rewriting one to match a failing run turns the only test
    # that notices a server change into a test that agrees with whatever
    # happened.
    #
    # Writes only. Reading a capture is how you answer "what does this endpoint
    # actually return", and a guard that blocks `cat`, `grep` or a diff is read
    # as an obstacle rather than as a rule.
    def deny_capture_write():
        deny(
            f"Blocked: {CAPTURES} is the pinned RomM 5.2.0 contract, and "
            "contract.captures\n"
            "diffs a live probe against it. Rewriting a capture to match a failing run "
            "silences\n"
            "the only test that notices RomM changing.\n"
            "Re-capture with server/contract/probe_contract.py and say in the PR body "
            "what changed and why.\n"
            "Reading a capture is fine -- this blocks writing one."
        )

    # A redirect into a capture, anywhere in the pipeline.
    for i, w in enumerate(words):
        if w in (">", ">>") and i + 1 < len(words) and _touches_captures(words[i + 1]):
            deny_capture_write()
        if (w.startswith(">") or w.startswith(">>")) and _touches_captures(w):
            deny_capture_write()

    positional = [w for w in words[1:] if not w.startswith("-")]
    verb = words[0].rsplit("/", 1)[-1] if words else ""
    if verb in ("rm", "tee", "truncate") and any(map(_touches_captures, positional)):
        deny_capture_write()
    # cp and mv: only the DESTINATION matters. Copying a capture out to look at
    # it is reading.
    if verb in ("cp", "mv", "install") and positional and _touches_captures(positional[-1]):
        deny_capture_write()
    if verb == "sed" and any(w == "-i" or (w.startswith("-i") and not w.startswith("--")) for w in words[1:]):
        if any(map(_touches_captures, positional)):
            deny_capture_write()

    return 0


def check_path(path):
    normalised = path if path.startswith("/") else "/" + path

    for marker in SELF_PROTECTED:
        if marker in normalised:
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

    if normalised.endswith(SECRET_SUFFIXES) or any(m in normalised for m in SECRET_CONTAINS):
        deny(
            f"Blocked: {path} holds per-worktree secrets (CLAUDE.md hard rule 5).\n"
            ".env is generated -- regenerate it with ./scripts/orca/env.sh rather than "
            "editing it."
        )

    if CAPTURES in normalised:
        deny(
            f"Blocked: {CAPTURES} is the pinned RomM 5.2.0 contract, and "
            "contract.captures\n"
            "diffs a live probe against it. Editing a capture to match a failing run "
            "silences the\n"
            "only test that notices RomM changing.\n"
            "Re-capture with server/contract/probe_contract.py and explain the diff in "
            "the PR body."
        )

    if normalised.endswith("/.github/workflows/unblock.yml"):
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
    ("Bash", {"command": "gh pr merge 42 --squash"}, 2, "an agent cannot merge its own PR"),
    ("Bash", {"command": "gh pr create --title x --body y"}, 0, "opening a PR is allowed"),
    ("Bash", {"command": "ctest --test-dir build --output-on-failure"}, 0, "running the tests is allowed"),
    ("Bash", {"command": "git push --force origin main"}, 2, "force-pushing main is blocked"),
    ("Bash", {"command": "git push origin main -f"}, 2, "...with the flag last, too"),
    ("Bash", {"command": "git -C /w/rommsync push --force-with-lease origin main"}, 2, "...through git's global options"),
    ("Bash", {"command": "git push --force origin feature/domain-fix"}, 0, "a feature branch whose name contains 'main' is fine"),
    ("Bash", {"command": "git push -u origin armaatus/thing"}, 0, "an ordinary push is fine"),
    ("Bash", {"command": 'git commit -m "note about --force pushes to main"'}, 0, "a commit message is not a command"),
    ("Bash", {"command": "probe.py > server/contract/captures/login.json"}, 2, "redirecting into a capture is blocked"),
    ("Bash", {"command": "rm server/contract/captures/login.json"}, 2, "deleting a capture is blocked"),
    ("Bash", {"command": "sed -i '' s/a/b/ server/contract/captures/login.json"}, 2, "editing a capture in place is blocked"),
    ("Bash", {"command": "cp /tmp/new.json server/contract/captures/login.json"}, 2, "copying over a capture is blocked"),
    ("Bash", {"command": "cp server/contract/captures/login.json /tmp/look.json"}, 0, "copying a capture OUT is reading, and allowed"),
    ("Bash", {"command": "cat server/contract/captures/login.json"}, 0, "reading a capture is allowed"),
    ("Bash", {"command": "diff server/contract/captures/login.json /tmp/new.json > /tmp/d"}, 0, "a diff whose output goes elsewhere is allowed"),
    ("Bash", {"command": "grep -r foo server/contract/captures/"}, 0, "grepping the captures is allowed"),
    ("Bash", {}, 2, "a Bash call with no command is refused, not allowed"),
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

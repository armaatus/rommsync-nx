#!/usr/bin/env python3
"""Block the things this project cannot afford an agent to do.

A PreToolUse hook: reads the tool call as JSON on stdin, exits 2 to block and
writes the reason to stderr, where Claude reads it and corrects itself. Exit 0
allows.

Four rules, and each one is here because asking for it is not good enough:

  1. an agent does not merge a pull request -- the maintainer does
  2. an agent does not force-push the default branch
  3. an agent does not write a secret, or a path the project pinned
  4. an agent in a worktree does not submit the review of a pull request

It does NOT fail open. An unreadable payload, a missing key, an unparseable
command: all of those block, because a guard that quietly stops guarding when
something upstream changes shape is worse than no guard at all.

Commands are shell-tokenised with shlex rather than matched as raw strings, so
`git -C /some/path push --force origin main` is caught and
`git commit -m "note about --force pushes"` is not. Compound commands are split
on `;`, `&&`, `||`, `|` and `&` -- outside quotes only -- and each segment is
judged on its own, and `bash -c` is recursed into.

What this is NOT: a sandbox. It reads a command and decides; it does not confine
one. A session that means to get past it can. The line it holds is the ROUTINE
one -- the shapes an agent reaches for when it is solving the problem in front of
it rather than working around a rule. Past that, the backstops are the diff and
the person who merges.

Rule 3 judges a write whichever way it arrives: through the editing tools
(Edit, Write, NotebookEdit), and through the shell spellings an agent routinely
reaches for -- a redirect, `cp`/`mv`/`install`/`ln`/`rsync`/`tee`/`rm`, `sed -i`
and `perl -i`, and the git verbs that rewrite a tracked file. That is a list,
and it misses what it does not name (`patch`, `git apply`, `dd`, an interpreter
opening a file); the selftest is the record of which spellings it knows. It
came back because leaving it out was worse: #267's capture reached the pinned
contract by a plain `cp`, while the docs said the shell was covered.

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

# What THIS project asks the guard to protect, over and above the four rules,
# is a file beside this one: `.claude/hooks/guard.json`, every key optional.
# The rules here stay generic; the paths and names are data.
#
#   {
#     "default_branch": "trunk",
#     "protected_paths": [
#       {"path": "server/contract/captures/",
#        "tail": "captures/",
#        "reason": "the pinned API contract; a test diffs a live probe against it",
#        "regenerate": "Regenerate it with the probe's --capture."}
#     ],
#     "secret_suffixes": ["/config.ini"],
#     "secret_contains": ["/token.dat"],
#     "secret_tails":    ["token.dat", "config.ini"]
#   }
#
# A malformed file is FATAL rather than ignored. A guard that silently falls back
# to "protect nothing" on a typo reports success on every write it was installed
# to stop.
GUARD_CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "guard.json")


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
            entry.get("reason", "protected by .claude/hooks/guard.json"),
            # How the project DOES rewrite it, when it does: the refusal names
            # the sanctioned writer instead of leaving the agent to find one.
            entry.get("regenerate", ""),
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
    return paths, suffixes, contains, tails, tuple(p[1] for p in paths) + tails


(PROTECTED_PATHS, SECRET_SUFFIXES, SECRET_CONTAINS,
 SECRET_TAILS, PROTECTED_TAILS) = _derive(PROJECT)

# The branch whose history is the audit trail. Read from guard.json rather than
# spelled here, so the selftest can prove the rule reads it (see its fixture).
DEFAULT_BRANCH = PROJECT.get("default_branch", "main")

# Whether this is an agent's worktree, for the selftest to decide. None asks git.
AGENT_WORKTREE = None

# `gh api` is both halves of the API: reading a PR and reviewing one. Only the
# write methods change anything, and blocking the reads would stop an agent
# finding out what it was in the middle of.
API_WRITE_METHODS = ("POST", "PUT", "PATCH", "DELETE")

# ...and `gh api` sends POST implicitly the moment any field is present, so a
# request with no -X can still be a write. A GraphQL mutation is the same thing
# wearing a different hat.
API_FIELD_FLAGS = ("-f", "-F", "--field", "--raw-field", "--input")


def _in_agent_worktree(cwd=None):
    """Is this a linked git worktree rather than the main checkout?

    Agents work in linked worktrees -- one per ticket, whether a Claude Code
    session's Agent subagent or Orca made it -- and the maintainer works in the
    main checkout. Rule 4 is for the first: somebody reviewing a pull request
    from their own checkout is the ordinary case, and a guard that argues about
    it is a guard people route around. A linked worktree's git dir is
    `<common>/worktrees/<name>`, so the two paths differ there and only there.

    A question git cannot answer counts as an agent's worktree: the rule must
    not fail open on the day the probe breaks.
    """
    if AGENT_WORKTREE is not None:
        return AGENT_WORKTREE
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--absolute-git-dir", "--git-common-dir"],
            capture_output=True, text=True, timeout=5, cwd=cwd,
        )
    except Exception:
        return True
    lines = out.stdout.split("\n")
    if out.returncode != 0 or len(lines) < 2 or not lines[1]:
        return True
    git_dir = lines[0]
    common = os.path.join(cwd or os.getcwd(), lines[1])
    return os.path.realpath(git_dir) != os.path.realpath(common)


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
    return _git_args(words)[0] == verb


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
    """The command's simple commands, split on `;` `&&` `||` `|` `&` and newlines.

    Only the first word of each is the verb: judging the whole string as one
    command is how `x && gh pr merge` reads as an invocation of `x`. The split is
    made OUTSIDE quotes only. A commit message that says "; tee .env was wrong"
    is prose, and splitting inside it made the prose a command (#267's write rule
    reads far more prose than the merge rule ever did). `>|` and `2>&1` are
    redirections, not separators.
    """
    # A backslash-newline is a continuation, not a separator, and left in place
    # it would glue a newline onto the next word.
    text = _strip_heredocs(command).replace("\\\n", " ")
    out, cur, quote, i = [], [], None, 0
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == "\\" and quote == '"' and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c == "\\" and i + 1 < len(text):
            cur += [c, text[i + 1]]
            i += 2
            continue
        if c in "'\"":
            quote = c
        elif text.startswith(("&&", "||"), i):
            out.append("".join(cur))
            cur, i = [], i + 2
            continue
        elif (c in ";\n" or (c == "|" and text[i - 1:i] != ">")
              or (c == "&" and text[i - 1:i] != ">" and text[i + 1:i + 2] != ">")):
            out.append("".join(cur))
            cur, i = [], i + 1
            continue
        cur.append(c)
        i += 1
    out.append("".join(cur))
    for raw in out:
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


MERGE_REFUSAL = (
    "Blocked: an agent does not merge a pull request in this repository.\n"
    "Work lands on the milestone's integration branch, which goes up as one\n"
    "pull request, and the maintainer reviews and merges it. See docs/WORKFLOW.md."
)


def _check_merge(words):
    """Rule 1: an agent does not merge, by any of the three spellings."""
    if _verb(words) != "gh":
        return
    rest = words[1:]
    sub = _gh_rest(words)
    if sub[:2] == ["pr", "merge"]:
        # `--auto` included. It asks GitHub to merge once the required checks
        # pass, and with merge-gate deleted (#277) none of them stands for the
        # maintainer's review: arming it is merging on green.
        deny(MERGE_REFUSAL)
    if sub[:1] == ["api"] and any(
        re.search(r"/pulls/\d+/merge", w) or "mergePullRequest" in w for w in rest
    ):
        deny(MERGE_REFUSAL + "\n(The REST and GraphQL spellings are the same act.)")


REVIEW_REFUSAL = (
    "Blocked: an agent working in a worktree does not submit a review of a pull\n"
    "request. Every pull request here goes up from the maintainer's account, so\n"
    "it is your own work's, and a branch that could review itself could certify\n"
    "itself. Report findings in your output instead; the maintainer reviews and\n"
    "merges."
)

# The GraphQL names for the same act. A PENDING review submitted later is a
# review too. `resolveReviewThread` is deliberately NOT here: whether every
# thread is resolved is branch protection, and GitHub decides it.
REVIEW_MUTATIONS = ("addPullRequestReview", "submitPullRequestReview")


def _check_review(words):
    """Rule 4: the author does not write the verdict on its own work.

    Four spellings, because the rule is worth exactly as much as its narrowest
    one: the porcelain, the REST route, the GraphQL mutation, and a field whose
    value this hook cannot read at all.
    """
    if _verb(words) != "gh" or not _in_agent_worktree():
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
            "Blocked: `gh api` with a field read from a FILE, from an agent's "
            "worktree.\n"
            "The rules here are about what the call does, and a body this "
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


def _shell_tokens(segment):
    """The segment's tokens with every redirection operator its own token.

    `_words` reads `echo x>a` as two words, the second one `x>a`, so the
    redirect is invisible to it. `punctuation_chars` splits `>`, `>>`, `&>` and
    `2>&1` out while still keeping a quoted `"a > b"` as one word -- which is
    what keeps a commit message that mentions a redirect from being one.
    """
    lexer = shlex.shlex(segment, posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:
        return segment.split()


_OPERATOR_CHARS = set("<>&|();")

# Words that run the next word as the command -- `sudo cp ...` is still `cp` --
# with the flags of theirs that take a value, so `sudo -u x cp` is too.
_WRAPPERS = {
    "sudo": ("-u", "-g", "-C", "-D", "-h", "-p", "-r", "-t", "-U", "-T"),
    "env": ("-u", "-C", "-S"),
    "command": (),
    "nohup": (),
    "time": ("-f", "-o"),
    "exec": ("-a",),
}

_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")

# The switches after which the rest of a clustered word is that switch's value,
# so an `i` there is not `-i`: perl's `-Mstrict`, sed's `-fscript`.
_VALUE_SWITCHES = {"sed": "efl", "perl": "MmIxFdDCl0e"}


def _in_place(words, verb):
    """`sed -i`/`perl -i`, however the flag is clustered or suffixed."""
    for w in words[1:]:
        if w == "--in-place" or w.startswith("--in-place="):
            return verb == "sed"
        if len(w) < 2 or w[0] != "-" or w[1] == "-":
            continue
        for c in w[1:]:
            if c == "i":
                return True
            if c in _VALUE_SWITCHES[verb]:
                break
    return False


def _unwrapped(words):
    """`words` with leading assignments and wrapper commands peeled off."""
    while words:
        if _ASSIGNMENT.match(words[0]):
            words = words[1:]
            continue
        takes_value = _WRAPPERS.get(_verb(words))
        if takes_value is None:
            return words
        i = 1
        while i < len(words) and (words[i].startswith("-") or _ASSIGNMENT.match(words[i])):
            i += 2 if words[i] in takes_value else 1
        words = words[i:]
    return words


def _git_args(words):
    """(`git`'s verb, the words after it, the `-C` dir), or (None, [], None)."""
    if _verb(words) != "git":
        return None, [], None
    i, c_dir = 1, None
    while i < len(words):
        w = words[i]
        if w in ("-C", "-c", "--git-dir", "--work-tree", "--namespace"):
            if w == "-C" and i + 1 < len(words):
                c_dir = words[i + 1]
            i += 2
            continue
        if w.startswith("-"):
            i += 1
            continue
        return w, words[i + 1:], c_dir
    return None, [], None


def _restore_source(args):
    """The revision `git restore` copies from, or None for the index."""
    for k, w in enumerate(args):
        if w in ("-s", "--source"):
            return args[k + 1] if k + 1 < len(args) else ""
        if w.startswith("--source="):
            return w.split("=", 1)[1]
        if w.startswith("-s") and not w.startswith("--"):
            return w[2:]
    return None


def _git_written(words):
    """The paths a `git` command rewrites, in the working tree or the index.

    The index counts: `git rm --cached` unpins a capture in the next commit as
    surely as deleting the file does.
    """
    verb, args, c_dir = _git_args(words)
    written = []
    if verb == "rm":
        written = [w for w in args if not w.startswith("-")]
    elif verb == "checkout":
        # Only checking a path out of ANOTHER revision rewrites it. `git
        # checkout -- <path>` puts back the bytes the index holds -- and `HEAD`
        # is the pin itself -- which is how a probe run gone wrong is undone;
        # refusing that would refuse the repair.
        if "--" in args:
            cut = args.index("--")
            rev = [w for w in args[:cut] if not w.startswith("-")]
            if rev and rev != ["HEAD"]:
                written = args[cut + 1:]
        elif not any(w in ("-b", "-B", "--orphan") for w in args):
            # `git checkout <rev> <path>`. A lone word is a branch, not a path,
            # and so are the two words of `-b <new> <start>`.
            positional = [w for w in args if not w.startswith("-")]
            if len(positional) >= 2 and positional[0] != "HEAD":
                written = positional[1:]
    elif verb == "restore":
        # The same line: `git restore <path>` restores the pin, `--source`
        # brings in another revision's bytes.
        source = _restore_source(args)
        if source not in (None, "HEAD"):
            written = [w for w in args if not w.startswith("-") and w != source]
    # A tree-wide write from another revision reaches everything under it.
    return [(w, (), True) for w in written], c_dir


def _target_dirs(words):
    """The destination `cp`/`mv`/`install`/`ln` was given by `-t`, if any.

    Clustered too: `cp -rt DIR` and `cp -tDIR`. Not for rsync, whose `-t` is
    "preserve times".
    """
    dirs = []
    for k, w in enumerate(words[1:], start=1):
        if w == "--target-directory" and k + 1 < len(words):
            dirs.append(words[k + 1])
        elif w.startswith("--target-directory="):
            dirs.append(w.split("=", 1)[1])
        elif len(w) > 1 and w[0] == "-" and w[1] != "-" and "t" in w:
            rest = w[w.index("t") + 1:]
            if rest:
                dirs.append(rest)
            elif k + 1 < len(words):
                dirs.append(words[k + 1])
    return dirs


def _recursive(words):
    return any(w in ("--recursive", "-a", "--archive")
               or (len(w) > 1 and w[0] == "-" and w[1] != "-" and ("r" in w or "R" in w))
               for w in words[1:])


def _written_paths(segment):
    """Every path this one command creates, replaces, moves or deletes.

    The routine spellings, not all of them: the redirect, the copy, the move,
    `tee`, `rm`, the in-place edit, and the git verbs that rewrite a tracked
    file. `patch`, `git apply`, `dd` and an interpreter opening a file are not
    here, and nothing claims they are -- the module docstring says what this
    is not.

    Returns (verb, writes, c_dir): each write is (path, sources, reaches_all),
    where `sources` are what a copy puts INTO `path` and `reaches_all` says the
    write lands on everything beneath it -- which is what lets a write to a
    PARENT of a protected directory be judged (see `_reaches_protected`).
    `c_dir` is the `git -C` dir the paths are relative to, if any.
    """
    tokens = _shell_tokens(segment)
    redirected, words, i = [], [], 0
    while i < len(tokens):
        t = tokens[i]
        if t and set(t) <= _OPERATOR_CHARS:
            if "<" in t or ">" in t:
                target = tokens[i + 1] if i + 1 < len(tokens) else None
                # `2>err`: the 2 is the descriptor, not an argument of the verb.
                if words and words[-1].isdigit():
                    words.pop()
                # `2>&1` duplicates a descriptor; `&> f` and `>& f` write f.
                duplicates = t.endswith("&") and target is not None and (
                    target.isdigit() or target == "-")
                if ">" in t and target is not None and not duplicates:
                    redirected.append(target)
                i += 2
            else:
                i += 1
            continue
        words.append(t)
        i += 1

    writes = [(w, (), False) for w in redirected]
    words = _unwrapped(words)
    verb = _verb(words)
    c_dir = None
    if verb in ("cp", "mv", "install", "ln"):
        targets = _target_dirs(words)
    else:
        targets = []
    positional = [w for w in words[1:] if not w.startswith("-") and w not in targets]

    if verb in ("cp", "install", "ln", "rsync", "mv"):
        dest = targets or positional[-1:]
        sources = tuple(positional if targets else positional[:-1])
        # rsync's `src/` and cp's `src/.` pour a directory's CONTENTS into the
        # destination, so whatever it holds lands beneath it.
        pours = any(s.endswith("/") if verb == "rsync" else s.endswith("/.")
                    for s in sources)
        writes += [(d, sources, pours) for d in dest]
        if verb == "mv":
            # Both ends: a move REMOVES its source, so moving a capture -- or a
            # directory holding the captures -- out of the tree unpins it.
            writes += [(s, (), True) for s in sources]
    elif verb == "tee":
        writes += [(w, (), False) for w in positional]
    elif verb == "rm":
        writes += [(w, (), _recursive(words)) for w in positional]
    elif verb in _VALUE_SWITCHES and _in_place(words, verb):
        writes += [(w, (), False) for w in positional]
    elif verb == "git":
        paths, c_dir = _git_written(words)
        writes += paths
    return verb, [w for w in writes if w[0]], c_dir


def _joined(prefix, path):
    """`path` under `prefix`, uncollapsed. A falsy prefix leaves it alone."""
    if path.startswith("/") or not prefix:
        return path
    return prefix.rstrip("/") + "/" + path


def _join(prefix, path):
    """`path` as seen from `prefix`, collapsed; the raw path when unknowable.

    A `prefix` of None is "somewhere this cannot work out" and "" is "a
    relative root it does not know": either way the path is judged on its own
    tail, which is what the path rules match.
    """
    collapsed = _collapse(_joined(prefix, path))
    return collapsed if collapsed else path


def _after_cd(prefix, words):
    """Where `cd <dir>` leaves the shell, or None when this cannot know.

    Unknown has to stay unknown -- a variable, a bare `cd`, a `cd -`, a walk
    above a relative root. A guessed prefix would resolve a later path to a file
    the command never touches.
    """
    targets = [w for w in words[1:] if not w.startswith("-")]
    if len(targets) != 1:
        return None
    target = targets[0]
    if "$" in target or "`" in target or target.startswith("~"):
        return None
    if not target.startswith("/") and prefix is None:
        return None
    return _collapse(_joined(prefix, target))


def _collapse(path):
    """`a/./b`, `a//b` and `a/c/../b` as the one path they name.

    None for a walk that climbs past the top. The protected markers are matched
    literally, so an unnormalised `./` or `..` in a path is enough to stop one
    matching.
    """
    rooted = path.startswith("/")
    out = []
    for part in path.split("/"):
        if not part or part == ".":
            continue
        if part == "..":
            if not out:
                return None
            out.pop()
        else:
            out.append(part)
    joined = "/".join(out)
    return ("/" + joined) if rooted else joined


def _reaches_protected(dest, sources, reaches_all, resolved):
    """The refusal for a write to a PARENT of a protected directory that lands in it.

    `rsync -a out/ server/contract/` and `git checkout main -- .` never name
    captures/, and both rewrite it. A resolved parent is recognised by the
    protected directory actually existing beneath it; a relative one, whose root
    this cannot know, by the protected path's own leading parts
    (`server/contract`) -- or, for the root itself, by the directory existing
    beneath it. It lands there when the write reaches everything beneath it, or
    when a copy's source carries the very name of the next directory down.
    """
    d = dest.rstrip("/") or "/"
    names = {s.rstrip("/").rsplit("/", 1)[-1] for s in sources}
    for where, _, _, _ in PROTECTED_PATHS:
        parts = [p for p in where.split("/") if p]
        for k in range(len(parts)):
            head = "/".join(parts[:k])
            if resolved or not head:
                if not os.path.isdir(os.path.join(d, *parts[k:])):
                    continue
            elif d != head and not d.endswith("/" + head):
                continue
            if reaches_all or parts[k] in names:
                return _refusal(d.rstrip("/") + "/" + "/".join(parts[k:]) + "/", resolved)
    return None


def _check_writes(segment, prefix):
    """Rule 3 from the shell: the paths a command writes, judged as an Edit is."""
    verb, writes, c_dir = _written_paths(segment)
    if c_dir is not None:
        prefix = _join(prefix, c_dir) if prefix is not None or c_dir.startswith("/") else None
    for path, sources, reaches_all in writes:
        full = _join(prefix, path)
        # Absolute -- because the shell's cwd is known, or the path was -- means
        # the protected paths are matched where they actually are, not on a tail
        # that any scratch `/tmp/.../captures/` also ends in.
        resolved = full.startswith("/")
        # A destination directory without its slash -- `cp x .../captures` --
        # writes INTO it, so it is judged as a directory too.
        message = (_refusal(full, resolved) or _refusal(full.rstrip("/") + "/", resolved)
                   or _reaches_protected(full, sources, reaches_all, resolved))
        if message:
            deny(f"{message}\n(`{verb}` writes {path} -- the shell spelling of the "
                 "same write.)")


def check_bash(command, cwd=None):
    """Every rule that reads a command line.

    `cwd` is where the shell stands -- the hook payload's own `cwd` -- so that a
    relative path after a `cd` still reaches the path rules as the file it is.
    """
    branch_cache = [None]
    prefix = cwd if isinstance(cwd, str) else ""
    for segment in _segments(command):
        words = _words(segment)
        if not words:
            continue
        # `(cd x && ...)`: the subshell's parenthesis is not part of the verb.
        words[0] = words[0].lstrip("(")
        if _verb(words) in ("cd", "pushd"):
            # Before the prefix moves: a redirect on the `cd` line is written
            # where the shell already stands.
            _check_writes(segment, prefix)
            prefix = _after_cd(prefix, words)
            continue
        if _verb(words) == "popd":
            prefix = None
            continue
        # `bash -c "..."` is a command in an argument. Judge what it will run --
        # and, first, where its own output is redirected.
        if _verb(words) in ("bash", "sh", "zsh", "dash") and "-c" in words:
            _check_writes(segment, prefix)
            idx = words.index("-c")
            if idx + 1 < len(words):
                check_bash(words[idx + 1], prefix)
            continue
        # `ssh host "..."` runs its argument on the far side, as bash -c does
        # here. Quote-blind splitting caught that by accident; the quote-aware
        # split needs it said. Only the merge, review and force-push rules
        # cross: the far side's paths are not this tree's.
        if _verb(words) == "ssh":
            remote = _ssh_remote(words)
            if remote:
                for far in _segments(remote):
                    far_words = _words(far)
                    if far_words:
                        _check_merge(far_words)
                        _check_review(far_words)
                        _check_force_push(far_words, branch_cache)
            _check_writes(segment, prefix)
            continue
        _check_merge(words)
        _check_review(words)
        _check_force_push(words, branch_cache)
        _check_writes(segment, prefix)
    return 0


# ssh(1)'s options that take a value: the word after one is not the host.
_SSH_VALUE_OPTS = set("BbcDEeFIiJLlmOoPpQRSWw")


def _ssh_remote(words):
    """The command line `ssh [options] host command...` runs remotely, if any."""
    i = 1
    while i < len(words) and words[i].startswith("-") and words[i] != "--":
        opt = words[i]
        i += 1
        if len(opt) == 2 and opt[1] in _SSH_VALUE_OPTS:
            i += 1
    if i < len(words) and words[i] == "--":
        i += 1
    return " ".join(words[i + 1:])


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


def _refusal(path, resolved=False):
    """Why rule 3 refuses a write to `path`, or None. Edit and shell alike.

    `resolved` says the path is known to be where it says -- an absolute path
    the shell resolved -- so a protected path must appear in it whole. The tail
    is for a path whose root this cannot see. Secrets keep their tails either
    way: a token is a token in whatever directory it sits.
    """
    normalised = path if path.startswith("/") else "/" + path
    tail = _protected_tail(path)

    if (normalised.endswith(SECRET_SUFFIXES)
            or any(m in normalised for m in SECRET_CONTAINS)
            or (tail and tail in SECRET_TAILS)):
        return (
            f"Blocked: {path} holds per-worktree secrets, and none of them belong in\n"
            "the tree. .env is generated -- regenerate it with "
            "./scripts/orca/env.sh rather than editing it."
        )

    for where, protected_tail, reason, regenerate in PROTECTED_PATHS:
        if where in normalised or (
                not resolved and protected_tail and tail == protected_tail):
            # The reason is the whole value of the rule: "blocked" with no why
            # sends the agent looking for a way around it -- and so is a rule
            # that refuses the write without naming the writer that may.
            return (
                f"Blocked: {where} is {reason}.\n"
                "Editing it to match a failing run silences whatever it exists to "
                "catch.\n"
                + (regenerate if regenerate else
                   "Say in the PR body what changed and why, or ask for it to be "
                   "regenerated the way this project regenerates it.")
            )
    return None


def check_path(path):
    """Rule 3: secrets, and the paths this project pinned."""
    message = _refusal(path)
    if message:
        deny(message)
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
        return check_bash(command, payload.get("cwd"))

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
    ("Bash", {"command": "gh pr merge 42 --auto --squash"}, 2,
     "...nor by arming auto-merge: with no merge gate, that is merging on green"),
    ("Bash", {"command": "gh pr merge 42 --auto --squash --admin"}, 2,
     "...and --admin, which bypasses the checks too, is still merging"),
    ("Bash", {"command": "gh  pr  merge 12"}, 2, "...however it is spaced"),
    ("Bash", {"command": "/opt/homebrew/bin/gh pr merge 12"}, 2, "...through an absolute gh"),
    ("Bash", {"command": "gh -R o/r pr merge 12"}, 2, "...past gh's own global options"),
    ("Bash", {"command": "gh api -X PUT repos/o/r/pulls/12/merge"}, 2, "...spelled as the REST call"),
    ("Bash", {"command": "gh api graphql -f query='mutation{ mergePullRequest(x) }'"}, 2,
     "...and by its GraphQL name"),
    ("Bash", {"command": "true && gh pr merge 3"}, 2, "a second segment is judged too"),
    ("Bash", {"command": 'bash -c "gh pr merge 3"'}, 2, "...and so is bash -c"),
    ("Bash", {"command": 'ssh host "x; gh pr merge 3"'}, 2,
     "...and so is what ssh runs on the far side"),
    ("Bash", {"command": "ssh -p 2222 -i key host gh pr merge 3"}, 2,
     "...past ssh's own options and the host"),
    ("Bash", {"command": 'ssh host "ls; uptime"'}, 0, "an ssh that merges nothing is allowed"),
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
    # that hardcodes a branch name instead of reading `.claude/hooks/guard.json`.
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
    # these rows prove the guard.json mechanism, with the pinned-contract rule
    # as the worked example.
    ("Edit", {"file_path": "/w/demo/.env"}, 2, "secrets are not editable"),
    ("Edit", {"file_path": "/w/demo/server/testing/fixture-auth.env"}, 2,
     "...including a project's fixture credentials"),
    ("Write", {"file_path": "/w/demo/token.dat"}, 2, "...and a stored token"),
    ("NotebookEdit", {"notebook_path": "/w/demo/.env"}, 2,
     "NotebookEdit names its target notebook_path, and is guarded too"),
    ("Edit", {"file_path": "/w/demo/server/contract/captures/login.json"}, 2,
     "a pinned capture is not hand-edited"),
    ("Write", {"file_path": "captures/login.json"}, 2, "...by a relative path, matched on its tail"),
    # The same rule for a shell command (#267): the 5.4.0 capture reached
    # captures/ by a `cp` from the probe's scratch output, and the guard that
    # refused it as a Write never saw it.
    ("Bash", {"command": "cp x server/contract/captures/5.4.0/y.json"}, 2,
     "a capture is not copied into place from the shell"),
    ("Bash", {"command": "echo x>server/contract/captures/a.json"}, 2,
     "...nor redirected into, with the operator against the word before it"),
    ("Bash", {"command": "probe &> captures/a.json"}, 2, "...nor by `&>`"),
    ("Bash", {"command": "cat > server/contract/captures/a.json <<'EOF'\n{}\nEOF"}, 2,
     "...and a heredoc's opening line is a real write even though its body is data"),
    ("Bash", {"command": "install -m 0644 /tmp/x server/contract/captures/a.json"}, 2,
     "...nor installed"),
    ("Bash", {"command": "rsync -a /tmp/out/ server/contract/captures/5.4.0/"}, 2,
     "...nor rsynced"),
    ("Bash", {"command": "perl -pi -e 's/a/b/' server/contract/captures/a.json"}, 2,
     "...nor edited in place by perl"),
    ("Bash", {"command": "cp -t server/contract/captures/5.4.0 /tmp/a.json /tmp/b.json"}, 2,
     "...nor copied with the destination given by -t"),
    ("Bash", {"command": "cp /tmp/a.json server/contract/captures"}, 2,
     "...nor into the directory named without its slash"),
    ("Bash", {"command": "cd server/contract/captures && cp /tmp/a.json 5.4.0/a.json"}, 2,
     "...nor after a cd that consumed the marker"),
    ("Bash", {"command": "cd server/contract/captures/5.4.0 && cp /tmp/a.json ."}, 2,
     "...nor to `.` once the cd has arrived"),
    ("Bash", {"command": "FOO=1 cp /tmp/x .env"}, 2, "...nor behind an assignment"),
    ("Bash", {"command": "bash -c 'cp /tmp/x token.dat'"}, 2, "...nor inside bash -c"),
    ("Bash", {"command": "rm server/contract/captures/a.json"}, 2,
     "a capture is not deleted -- a removed pin is as unpinned as an edited one"),
    ("Bash", {"command": "git rm -q server/contract/captures/a.json"}, 2, "...nor by git rm"),
    ("Bash", {"command": "mv server/contract/captures/a.json /tmp/"}, 2,
     "...nor moved out, which removes it"),
    ("Bash", {"command": "git checkout main -- server/contract/captures/"}, 2,
     "a capture is not checked out of another revision"),
    ("Bash", {"command": "git restore --source=HEAD~3 server/contract/captures/a.json"}, 2,
     "...nor restored from one"),
    ("Bash", {"command": "git -C server/contract/captures checkout main -- a.json"}, 2,
     "...however git is pointed at it"),
    ("Bash", {"command": "git checkout -- server/contract/captures/"}, 0,
     "putting back the pinned bytes is the repair, not a write around the pin"),
    ("Bash", {"command": "git restore server/contract/captures/a.json"}, 0, "...by restore too"),
    ("Bash", {"command": "git checkout fix/captures/sweep"}, 0, "a branch is not a path"),
    ("Bash", {"command": "cp server/contract/captures/a.json /tmp/a.json"}, 0,
     "copying a capture OUT is reading it"),
    ("Bash", {"command": "cd /tmp && cp server/contract/captures/a.json ."}, 0,
     "...wherever the cd left the copy"),
    ("Bash", {"command": "./.venv/bin/python server/probe_contract.py --auth --negotiate "
                         "--sync-scenarios --capture server/contract/captures/5.4.0"}, 0,
     "the probe writing its captures in place is the sanctioned writer"),
    ("Bash", {"command": "ctest --test-dir build 2>&1 | tee build/logs/ctest.log"}, 0,
     "`2>&1` is a descriptor, and tee into a log is ordinary"),
    ("Bash", {"command": "probe 2>build/logs/err.txt"}, 0, "...and so is a stderr redirect"),
    ("Bash", {"command": "git commit -m 'never cp x > server/contract/captures/a.json'"}, 0,
     "a quoted redirect in a commit message is not one"),
    ("Bash", {"command": "sed -i '' s/a/b/ src/app.c"}, 0, "an in-place edit of source is fine"),
    ("Bash", {"command": "echo hi > /tmp/note.txt"}, 0, "an ordinary redirect is fine"),
    # What the review of the first cut found, both directions.
    ("Bash", {"command": "bash -c 'probe' > server/contract/captures/a.json"}, 2,
     "a redirect of bash -c's own output is judged, not only what it runs"),
    ("Bash", {"command": "cp -rt server/contract/captures /tmp/a.json"}, 2,
     "-t clustered with another flag still names the destination"),
    ("Bash", {"command": "env -i cp /tmp/x .env"}, 2, "a wrapper's own flags are not the verb"),
    ("Bash", {"command": "sudo -u x cp /tmp/x .env"}, 2, "...nor the value one of them takes"),
    ("Bash", {"command": "echo x >| server/contract/captures/a.json"}, 2,
     "`>|` is a redirect, not a pipe"),
    ("Bash", {"command": "(cd server/contract/captures && cp /tmp/a.json 5.4.0/a.json)"}, 2,
     "a cd inside a subshell is followed"),
    ("Bash", {"command": "cp x \\\n  server/contract/captures/a.json"}, 2,
     "a line continuation joins the line rather than hiding its second half"),
    ("Bash", {"command": "rsync -a /tmp/out/ server/contract/"}, 2,
     "pouring a directory's contents into a PARENT of captures/ reaches it"),
    ("Bash", {"command": "cp -r /tmp/run/captures server/contract/"}, 2,
     "...and so does copying a directory named captures into its parent"),
    ("Bash", {"command": "git checkout main -- server/contract"}, 2,
     "...and checking a parent out of another revision"),
    ("Bash", {"command": "rm -rf server/contract"}, 2, "...and deleting a parent"),
    ("Bash", {"command": "git rm --cached server/contract/captures/a.json"}, 2,
     "unstaging a capture from the index unpins it in the next commit"),
    ("Bash", {"command": "cp -r /tmp/out server/contract/"}, 0,
     "a copy into the parent that lands beside captures/ is not into it"),
    ("Bash", {"command": 'git commit -m "drop the stale .env; tee .env was wrong"'}, 0,
     "a `;` inside a quoted message does not start a command"),
    ("Bash", {"command": 'git commit -m "first line\n\nrm .env stays blocked"'}, 0,
     "...nor does a newline inside one"),
    ("Bash", {"command": "cp x /tmp/scratch/captures/a.json"}, 0,
     "an absolute scratch dir named captures/ is not the pinned contract"),
    ("Bash", {"command": "rm -rf /tmp/probe/captures"}, 0, "...nor is deleting one"),
    ("Bash", {"command": "git checkout -b fix captures"}, 0,
     "`-b <new> <start>` names two branches, not a path"),
    ("Bash", {"command": "pushd server/contract/captures && popd && cp /tmp/x a.json"}, 0,
     "a popd is followed -- to somewhere unknown, not to where pushd went"),
    ("Bash", {"command": "perl -Mstrict -ne 'print' server/contract/captures/a.json"}, 0,
     "perl's -Mstrict is not -i"),
    ("Bash", {"command": "git checkout HEAD -- server/contract/captures/"}, 0,
     "HEAD is the pin itself, so checking a capture out of it is the repair"),
    ("Bash", {"command": "git restore --source=HEAD server/contract/captures/a.json"}, 0,
     "...and so is restoring one from it"),
    ("Edit", {"file_path": "/w/demo/.claude/hooks/guard.py"}, 0,
     "the guards themselves are editable -- the diff and the maintainer's merge are their control"),
    ("Edit", {"file_path": "/w/demo/src/app.c"}, 0, "ordinary source files are editable"),
    ("Edit", {"file_path": "/w/demo/.claude/agents/verifier.md"}, 0, "so are the subagents"),

    # --- 4. reviewing, outside an agent's worktree --------------------------
    ("Bash", {"command": "gh pr review 7 --approve"}, 0,
     "a person reviewing from the main checkout is the ordinary case"),
    ("Bash", {"command": "gh api graphql --input /tmp/m.json"}, 0,
     "...and so is any other gh api call they make"),

    # --- payloads -----------------------------------------------------------
    ("Bash", {}, 2, "a Bash call with no command is refused, not allowed"),
    ("Edit", {}, 0, "a matched tool that names no path has nothing here to answer to"),
]

# #267's acceptance, as the matrix it is: each routine write shape into the
# pinned contract and into each secret is refused, and reading the same path
# by the routine read shapes is not.
_WRITE_TARGETS = ("server/contract/captures/5.4.0/y.json", ".env",
                  "token.dat", "device.dat", "config.ini",
                  "server/testing/fixture-auth.env")
_WRITE_SHAPES = (("cp /tmp/x {}", "cp"), ("mv /tmp/x {}", "mv"),
                 ("echo x | tee {}", "tee"), ("probe > {}", "a > redirect"),
                 ("probe >> {}", "a >> redirect"), ("sed -i '' s/a/b/ {}", "sed -i"))
_READ_SHAPES = (("cat {}", "cat"), ("diff {} /tmp/x", "diff"),
                ("git show HEAD:{}", "git show"))
SELFTEST += [("Bash", {"command": shape.format(t)}, 2, f"{name} into {t} is refused")
             for t in _WRITE_TARGETS for shape, name in _WRITE_SHAPES]
SELFTEST += [("Bash", {"command": shape.format(t)}, 0, f"{name} of {t} is allowed")
             for t in _WRITE_TARGETS for shape, name in _READ_SHAPES]

# A relative path means wherever the shell already stands, which the hook
# payload's `cwd` says. (cwd, command, want, what)
CWD_CASES = [
    ("/w/demo/server/contract/captures/5.4.0", "cp /tmp/a.json b.json", 2,
     "a relative write is judged where the shell stands"),
    ("/w/demo/server/contract/captures", "cd .. && cp /tmp/a.json b.json", 0,
     "...and a cd out of there is followed"),
    ("/w/demo", "cp /tmp/a.json b.json", 0, "...which is an ordinary directory usually"),
    ("/w/demo", "mkdir -p build/captures && cp out.json build/captures/", 0,
     "...and with the cwd known, a build/captures/ is not the pinned one"),
]

# A write to a PARENT of the protected directory, with the cwd known: that is
# decided by the protected directory existing beneath it, so these run in a real
# temporary tree holding one. (command, want, what)
TREE_CASES = [
    ("git checkout main -- .", 2, "checking the whole tree out of another revision reaches captures/"),
    ("rsync -a /tmp/out/ server/contract/", 2, "...and so does pouring into its parent"),
    ("cp -r /tmp/out .", 0, "...while a copy that lands beside it does not"),
]

# Rule 4, which holds only in a linked worktree -- where agents work. Same
# (command, want, what) shape as SELFTEST's Bash rows; _stateful_checks says
# "this is an agent's worktree" around them.
AGENT_CASES = [
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
    ("gh api repos/o/r/pulls/7/reviews", 0, "reading the reviews stays open"),
    ("gh api graphql -f query='mutation{ resolveReviewThread(x) }'", 0,
     "resolving a thread is branch protection's business, not this rule's"),
    ("gh pr create --title x --body y", 0, "...and opening a pull request is allowed"),
    ("git push -u origin fleet/m13", 0, "...and so is pushing a branch"),
]

# The project config the selftest runs against. Not this repo's own
# `.claude/hooks/guard.json`: the rows above assert exact refusals, and a suite
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
            "regenerate": "Regenerate it with the probe, which may write there.",
        }
    ],
    "secret_suffixes": ["/config.ini"],
    "secret_contains": ["/token.dat", "/device.dat"],
    "secret_tails": ["token.dat", "device.dat", "config.ini"],
}


def _run(tool, tool_input, cwd=None, said=None):
    """One case, with the refusal text swallowed -- or kept in `said`.

    Every blocking row prints its reason to stderr, and forty of them buries the
    one line that matters -- which assertion failed. The failures below print
    their own message, so nothing is lost.
    """
    payload = {"tool_name": tool, "tool_input": tool_input}
    if cwd is not None:
        payload["cwd"] = cwd
    try:
        with contextlib.redirect_stderr(said if said is not None else io.StringIO()):
            main(payload)
        return 0
    except SystemExit as exc:
        return exc.code


# A refusal with no way forward sends the agent looking for a way round, and
# #267's own `cp` was exactly that: the probe's output, copied in by hand. So
# both halves of rule 3 must name the writer that MAY write there.
MESSAGE_CASES = [
    ("Bash", {"command": "cp /tmp/x server/contract/captures/a.json"},
     "the shell refusal names the sanctioned writer"),
    ("Write", {"file_path": "/w/demo/server/contract/captures/a.json"},
     "...and so does the Edit/Write one"),
]


def _cwd_and_message_checks():
    """CWD_CASES, TREE_CASES in a real tree, and MESSAGE_CASES."""
    import tempfile
    failures = 0
    with tempfile.TemporaryDirectory() as tree:
        os.makedirs(os.path.join(tree, "server", "contract", "captures"))
        cases = CWD_CASES + [(tree, c, want, what) for c, want, what in TREE_CASES]
        for cwd, command, want, what in cases:
            got = _run("Bash", {"command": command}, cwd=cwd)
            if got != want:
                print(f"FAIL: {what} (expected exit {want}, got {got})", file=sys.stderr)
                failures += 1
            else:
                print(f"  ok: {what}")
    hint = SELFTEST_PROJECT["protected_paths"][0]["regenerate"]
    for tool, tool_input, what in MESSAGE_CASES:
        said = io.StringIO()
        got = _run(tool, tool_input, said=said)
        if got != 2 or hint not in said.getvalue():
            print(f"FAIL: {what} (exit {got}, said {said.getvalue()!r})", file=sys.stderr)
            failures += 1
        else:
            print(f"  ok: {what}")
    return failures


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

    # A malformed `guard.json` is FATAL rather than ignored: falling
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
        # ...while an ABSENT one still leaves the four rules in place.
        if _load_guard_config(os.path.join(tmp, "nothing-here.json")) != {}:
            print("FAIL: an absent guard.json is not the same as a broken one",
                  file=sys.stderr)
            failures += 1
        else:
            print("  ok: an absent guard.json leaves the universal rules in place")

    # Which checkout this is, asked of a real repository and its real linked
    # worktree: the main checkout is a person's, a linked worktree an agent's.
    with tempfile.TemporaryDirectory() as tmp:
        main_dir, linked = os.path.join(tmp, "main"), os.path.join(tmp, "linked")
        plain = os.path.join(tmp, "plain")
        os.mkdir(plain)
        quiet = {"capture_output": True, "check": True}
        # This machine's git config must not decide the fixture: a global
        # commit.gpgsign or hooks path would fail or stall the empty commit.
        isolated = ["-c", "user.name=t", "-c", "user.email=t@t",
                    "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null"]
        subprocess.run(["git", "init", "-q", main_dir], **quiet)
        subprocess.run(["git", "-C", main_dir, *isolated,
                        "commit", "-q", "--allow-empty", "-m", "x"], **quiet)
        subprocess.run(["git", "-C", main_dir, *isolated,
                        "worktree", "add", "-q", linked], **quiet)
        for where, want, what in (
            (main_dir, False, "the main checkout is not an agent's worktree"),
            (linked, True, "...and a linked worktree is"),
            (plain, True, "...and neither is a directory git cannot answer for waved through"),
        ):
            got = _in_agent_worktree(where)
            if got != want:
                print(f"FAIL: {what} (expected {want}, got {got})", file=sys.stderr)
                failures += 1
            else:
                print(f"  ok: {what}")
    return failures


PLUMBING_ASSERTIONS = 10


def _stateful_checks():
    """AGENT_CASES, as if this were an agent's worktree.

    The SELFTEST rows assert what the guard does in the main checkout -- "a
    person reviewing is the ordinary case". Run inside a linked worktree those
    would be untrue BY DESIGN, so the selftest decides which one it is in
    rather than asking git.
    """
    global AGENT_WORKTREE
    saved = AGENT_WORKTREE
    AGENT_WORKTREE = True
    try:
        return _bash_cases(AGENT_CASES)
    finally:
        AGENT_WORKTREE = saved


def selftest():
    global PROJECT, PROTECTED_PATHS, SECRET_SUFFIXES, SECRET_CONTAINS
    global SECRET_TAILS, PROTECTED_TAILS, DEFAULT_BRANCH, AGENT_WORKTREE

    PROJECT = SELFTEST_PROJECT
    (PROTECTED_PATHS, SECRET_SUFFIXES, SECRET_CONTAINS,
     SECRET_TAILS, PROTECTED_TAILS) = _derive(PROJECT)
    DEFAULT_BRANCH = PROJECT["default_branch"]

    failures = 0
    saved = AGENT_WORKTREE
    # The main checkout for the stateless rows, whichever checkout this runs
    # in. Left to git, those rows would assert the opposite of themselves inside
    # an agent's worktree -- which is where they matter most.
    AGENT_WORKTREE = False
    try:
        for tool, tool_input, want, what in SELFTEST:
            got = _run(tool, tool_input)
            if got != want:
                print(f"FAIL: {what} (expected exit {want}, got {got})", file=sys.stderr)
                failures += 1
            else:
                print(f"  ok: {what}")
    finally:
        AGENT_WORKTREE = saved
    failures += _stateful_checks()
    failures += _cwd_and_message_checks()
    failures += _plumbing_checks()
    if failures:
        print(f"{failures} guard assertion(s) failed", file=sys.stderr)
        return 1
    held = (len(SELFTEST) + len(AGENT_CASES) + len(CWD_CASES) + len(TREE_CASES)
            + len(MESSAGE_CASES) + PLUMBING_ASSERTIONS)
    print(f"{held} guard assertions hold")
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

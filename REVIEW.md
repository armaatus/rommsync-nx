# Review policy

What the independent review looks for, which findings **block a merge** and
which do not, and what it should not report at all.

**It runs once.** One review per pull request, from a context that has not seen
the conversation which produced the diff, because an author reviewing its own
work shares its blind spots. If it asks for changes, the author gets exactly one
fix session and that fix is re-reviewed once. That second verdict is final: a
second request for changes parks the pull request for a person. **Two reviews
maximum, ever.**

Nothing else reads the branch. That bound is the finding of #86, which burned
four reviews without one ever judging the commit that merged: every answer to a
finding is a commit, every commit moves the head, and a head move invalidates
the review that asked for it. Reviewing a branch four times is not four times
the assurance; it is the same review of four different commits.

Read this file before reviewing. If you are the author, read it before you
finish — a finding you can predict is one you can avoid.

## The dimensions

Evaluate all seven. Report only what you found; a dimension with nothing in it
gets one line or none.

1. **Architecture and design** — separation of concerns, modularity, coupling,
   whether abstractions are at a consistent level, whether the design holds at
   the size this will actually reach.
2. **Code quality and maintainability** — readability by someone with no AI
   assistance and no context, naming, unnecessary complexity, duplication that
   should be extracted, error handling, and whether comments explain *why*.
3. **Impact and breaking changes** — every usage of a changed interface found
   and updated, backward compatibility, migrations that are safe and reversible,
   consumers of a changed API accounted for, downstream effects considered.
4. **Testing** — critical paths and edge cases covered, tests meaningful and
   independent, failure cases tested. **Is there a test that would have failed
   before this change?** Name it. Its absence is Important regardless of how
   green the suite is.
5. **Performance** — algorithmic complexity at the expected data size, query
   patterns and N+1s, resource usage, whether long-running work blocks something
   it should not.
6. **Security** — input validation, authentication and authorisation, exposure
   of sensitive data, injection, and dependencies.
7. **Project standards and spec** — the hard rules in `CLAUDE.md`; the issue's
   **Scope** and **Acceptance**, naming anything in the diff outside Scope and
   anything in Acceptance the diff does not cover; the `## Plan` section, where
   an undocumented departure from it is Important and a documented one is fine;
   and whether the work invalidated an issue — any issue — that has not been
   edited, which is Important because those bodies are the only channel between
   parallel worktrees.

**A fleet pull request carries its `fleet.sh cost` figure.** It is the only
number saying whether a run got cheaper. Missing, it is a Suggestion.

### ...and this project's own

A host repository adds its correctness rules in **`.autofleet/review.md`**, and
they are part of this policy wherever that file exists. Read it after this one.

That seam is the point. The rules that belong there are the ones this file
cannot know — the save file that must be written atomically, the header that may
not appear in `core/`, the address a test may not reach. They were written into
this file once, and this file is **vendored into every host repository**, so
every project that installed autofleet was reviewed against another project's
save format. Nothing in the payload may know about one project; that is hard rule
2, and this file was breaking it.

## Critical, Important, Suggestion

**Critical** — security, data loss or corruption, a breaking change with no
migration, or a production failure. **A Critical finding is fixed, never argued
away.** There is no second reviewer behind this one to take the argument to.

**Important** — a real defect or a breach of a hard rule: wrong behaviour on a
path that matters, a missing test that would have caught the bug, a broken build
on either target, or the tracker left saying something untrue. Fixed, or
disputed in the commit message with a reason the re-review accepts.

**Suggestion** — naming, comment wording, ordering, a clearer formulation of
something already correct. **A Suggestion is posted, and that is all.** It goes
in an ordinary pull request comment that blocks nothing and that nobody has to
answer; `review.sh` separates them from the blocking half by severity, so
labelling one thing as another is the single mistake here that moves a merge.
The asymmetry is deliberate: a nit that held a branch cost a whole loop to
change a comment.

**A Suggestion never becomes its own issue.** One that does costs a whole loop —
brief, implementation, review, fix — to change a comment, and a review that
mints work every round is the most expensive thing in this system. Anything
worth keeping goes on the repository's standing nit issue, if it keeps one.

Report at most **five Suggestions**, and summarise the rest as a count. A review
whose signal is buried in twenty preferences costs more attention than it saves.

## What not to report

- **The comment density**, where a project's own rules ask for it. A diff that
  matches the code around it is correct.
- **A preference restated as a defect.** If the existing code is correct and you
  would have written it differently, that is a Suggestion at most, and it counts
  against the cap of five.
- **Anything you cannot cite.** A behaviour claim needs a `file:line` in the
  actual source, not an inference from a name. If you are unsure a finding is
  real, drop it or say you are unsure — there is one fix session behind this
  review, and a wrong Important finding spends it with no later round to take it
  back in.

## The verdict is a field, not a sentence

The reviewer answers with a JSON object — `verdict`, and `findings` each
carrying a severity, a file, a line and its text. `scripts/fleet/review.sh`
posts it: the Critical and Important findings as the review body, the
Suggestions as an ordinary comment, and a marker naming the head it judged.

**`verdict` is `request-changes` if and only if something is Critical or
Important.** Nothing reads prose to decide. That marker is the only thing
`.github/scripts/merge_gate.py` looks for, and it is written by the script from
the `verdict` field — never spelled by the model, and never reachable from the
worktree under review (`.claude/hooks/guard.py` refuses `gh pr review` there).

It has to be a marker and not GitHub's own APPROVED state because GitHub
refuses `--approve` and `--request-changes` on your own pull request, and the
reviewer signs in as whoever `gh` is — normally the account that opened the PR.
`review.sh` tries the real state first and falls back, so a repository with a
separate reviewer identity gets the badge for free.

**A count of findings is not a trailer.** Three HTML comments used to end every
review body, because the gate could not otherwise tell five Suggestions from
nothing at all — both being a COMMENTED verdict. The severity field tells it
now, in the one place the reviewer states it.

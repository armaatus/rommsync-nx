# plans/

One file per issue, written before the code and committed with it:
`plans/<issue-number>-<slug>.md`, for example `plans/12-md5-hashing.md`.

## Why a file and not just a conversation

An agent that starts editing has already made every decision that matters --
which files change, in what order, and what proves it worked. Those decisions are
the cheapest thing in the whole cycle to correct and the most expensive to
discover in a finished diff. Writing them down first is what makes correcting
them possible; committing them is what lets a reviewer check the diff against
what was agreed rather than reconstructing the intent from the code.

It is also the channel between worktrees. Three agents run in parallel and cannot
see each other. A committed plan says which files a branch is going to touch,
which is the earliest point at which a collision is visible.

## The shape

```markdown
# M2-3: MD5 hashing + state.db baseline  (#12)

## Files that change
- `core/src/hash.cpp` (new) — streaming MD5 over a file handle
- `core/include/rommsync/hash.hpp` (new)
- `core/src/state.cpp` — store the digest alongside the rom row
- `tests/test_hash.cpp` (new)

## Order of work
1. `hash.hpp` + `hash.cpp` with the digest test against a known vector
2. state.db column and migration
3. wire the scan to record it

## Risks
- state.db already exists in provisioned worktrees; the migration has to be
  idempotent or every existing worktree breaks on the next run.
- MD5 over a 4 GB rom must stream; reading it into memory is an OOM on Horizon.

## Proof
- `tests/test_hash.cpp` — known-vector digest, and a digest over a file larger
  than the read buffer
- `ctest -R state` stays green, and a new case asserts the column survives a
  reopen
```

Four headings, nothing else: **Files that change**, **Order of work**, **Risks**,
**Proof**. The bar is that an engineer who has never seen the conversation could
implement the change from the plan alone.

## The rule that makes it worth keeping

**When the implementation departs from the plan, update the plan in the same
commit.** A plan that describes a diff that does not exist is worse than no plan:
it reads as agreed when it is not, and review pass 3 in [REVIEW.md](../REVIEW.md)
checks the diff against it.

Departing is normal. Silently departing is the problem.

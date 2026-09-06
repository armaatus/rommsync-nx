# Local review — M1-6 (#123)

Two passes, on `main...HEAD`, per CLAUDE.md and REVIEW.md.

## `/code-review high` — defects

Ran at high effort over the whole branch. It built the tree and ran the suite
(270/270) before reporting. It spent most of its effort on the new concurrency
and cleared it: the `driven_`/`attempt_` shared-pointer identity comparison is
ABA-safe (both the driving thread and `driven_` hold a strong reference, so no
replacement can reuse the address); every access to `auth_`, `gate_`, the
attempt and the `token.dat`/`auth.json` writes is under `mutex_`; there is no
lock-order inversion, because the pairing thread never holds the session's lock
while taking `mutex_`; and no wake-up can be lost, because both `wait`
predicates are re-evaluated under the lock rather than trusting `notify_all`.

Three findings, **all three fixed** in `9c5aaa4`.

1. **`std::thread`'s constructor throws, and the console builds with
   `-fno-exceptions`** (`engine.cpp`, thread creation). `switch.mk`'s own comment
   says a throwing stdlib path "calls std::terminate here instead -- the
   sysmodule dies". Creating the thread inside `StartPairing` put that throw
   under a user's finger: once #126 gives a console a transport, a **Pair** press
   on a console whose inner heap is already carrying a download buffer and a TLS
   context would kill `sys-rommsync` rather than answer a refusal the settings
   screen can draw. Secondary: the attempt was published *before* the thread
   existed, so even a build that could recover would be left with an attempt
   nobody drives and a screen stuck at `kStarting`.
   **Fixed:** the thread starts in `UsePairingBackend`, beside where `main.cpp`
   already aborts deliberately on `sm` and `fs`. A backend with no transport
   starts nothing, which is every console today.

2. **The pairing thread woke five times a second for the life of a code**
   (`kPairingTick{200}`) — some 3000 wake-and-relock cycles per ten-minute
   pairing, on a process resident for the life of the console, each taking two
   mutexes to be told the poll was not due. `PairingSession::next_poll_at()`
   exists for exactly this and had no callers anywhere in the tree.
   **Fixed:** `wait_until` on that deadline. One wake-up per poll actually made,
   and the condition variable still cuts the wait short for a new attempt or for
   shutdown, which is what promptness there actually depends on.

3. **One stale comment survived the sweep** (`settings_screen.cpp`, beside
   `PressRepair`): it still said the `StartPair`-before-`Unpair` ordering existed
   because `StartPairing` was not real, contradicting the four places that *were*
   updated — and it is the copy sitting next to the code that implements the
   order. **Fixed.**

Two things it checked and deliberately did not report, which I agree with:
`Unpair()` not abandoning a live attempt (deliberate, and `engine.repairs` pins
it), and `IdentityError::kUnreadable` surfacing as `ipc::Error::kWriteFailed`
(semantically loose, reasoned about in the code, and it reaches the user as a
generic refusal either way).

## `/mattpocock-skills:code-review` — standards and spec

Two axes, run as separate agents so neither masks the other.

### Standards

**No hard violations.** It checked each rule this diff could plausibly breach:
hard rule 4 (the `core/` hunks are comment-only; the transport and
`setsysGetSerialNumber` are injected through `PairingBackend`, which is the right
side of the seam), REVIEW.md on network calls (timeouts come from
`PairingConfig::request_timeout`, and `engine.nonblocking` asserts the command
does not wait), hard rule 1 (`engine.unreachable` uses `127.0.0.1:9`;
`engine.commands` uses `romm.example.com`, this file's existing never-contacted
convention), and CLAUDE.md's finishing rules (docs corrected in the same commit,
tests that would have failed before).

One judgement call against a documented rule, **fixed**: sysmodule/AGENTS.md's
heap discipline, against a `std::thread` whose stack nothing sizes. It now says
so in `engine.hpp` and is an acceptance item on #126, which is the issue that
gives a console a transport and so the first attempt that ever runs.

Four baseline smells, **all four fixed** in `b29b357`:

- **Data Clumps** — `attempt_`, `attempt_server_url_`, `attempt_generation_` and
  `driven_generation_` were one object spread over four members, three of which
  travelled together again as `CommitGrant`'s parameters. Now one
  `PairingAttempt{session, server_url}` behind a `shared_ptr`, and the two
  counters are gone: pointer identity answers "was I superseded" exactly.
- **Repeated Switches** — `test_engine.cpp`'s `main()` switched on the scenario
  name twice. Now one `{name, fn}` table.
- **Duplicated Code** — three scenarios opened with the same
  config/backend/boot preamble. Now `BootPairable`.
- **Middle Man** — `test_pairing.cpp`'s `Deny` became pure delegation once the
  helper moved to `rig.hpp`. Callers use `rig::DenyDeviceCode` directly;
  `Approve` stays, because it binds `kScopes`.

Two nits, both fixed: `<vector>` in its own include block in `rig.hpp`, and
`http://` on an example host in a file that otherwise uses `https://`.

### Spec

**Acceptance 2, 3, 4, 5 and 7 are met** (`engine.pairs`, `engine.nonblocking`,
`engine.repairs`, `engine.unreachable` + `engine.commands`, `switch.builds`).
Acceptance 6 is tracker work, done and listed below.

**Acceptance 1 is met in part, deliberately, and the tracker now says so.**
`StartPairing` no longer returns `kUnavailable` *unconditionally*, and
`ipc.hpp`'s list is updated — but it still returns it on a build with no HTTP
transport, which is every console. The issue's note said "whichever of the three
lands first builds the Horizon client"; M1-6 landed first and did not. The `ssl`
backend is a whole piece of Horizon glue that cannot be proven off-console before
the M8-1 gate, and a partial `HttpClient` is worse than none, since `Download`'s
`Range` resume and streamed multipart are what M2 and M3 already depend on. It is
**#126**, and #43 and #123 are edited to say so.

**Two claims in the issue body were false of the tree**, both recorded on #123
and on #31: there is no `SdEngine::UseServer`, and #31 has not landed, so the
three list commands still answer `kUnavailable`.

Three pieces of behaviour it flagged as not asked for, all kept, with reasons now
in the code: abandoning a live attempt on a `server.url` edit (the alternative is
committing a token issued by one RomM under a config naming another), reporting a
commit failure through `PairingStatus::message` (the session cannot know the card
refused, and would go on reporting `kApproved` — a console that says it paired and
holds nothing), and comment-only edits under `overlay/` (they asserted things this
PR made false).

One implementation weakness, **not fixed, now documented**: "Start over" is not
prompt. `http::HttpClient` cannot be cancelled from outside a call and
`PairingSession` owns its requests, so a second `StartPairing` cannot `Begin()`
until the previous request returns — the screen can read "starting" for up to one
`request_timeout` after a press that follows a hung init. Fixing it needs a
`CancelToken` on `PairingConfig`, which is `core/`'s to add and outside this
issue's scope.

## Not from either pass

`pair.retry` failed once inside a combined run and passed alone seconds later,
unchanged, while a second worktree was running its suite. Recorded on #118 as a
data point rather than a diagnosis; it is not caused by this branch, and the
final full run is green.

## After the branch was rebased onto M5-4 (#31 / PR #127)

#127 merged while this was in review and touched five of the same files. The
branch is rebased onto it, and three things came out of that:

- **The review bot's one Important finding was the same collision.** It flagged
  `ipc.hpp`'s `kUnavailable` list for still naming the three list commands, which
  #127 had just made real. Already fixed as part of the rebase: that list now
  names only `SyncNow` (M7-2) and `StartPair`-with-no-transport, and says out
  loud that a *list* answers `kOffline` on the same console while a pairing
  attempt answers `kUnavailable` -- a list has a server it cannot reach, and an
  attempt has no way to reach one. Its other two passes found nothing, and it
  hand-traced the lock order, the `attempt_`/`driven_` supersession and
  `AbandonPairingLocked` against a live `Begin()` and called them clean.
- **`SdEngine` now has two setters that each take an `http::HttpClient*`** --
  #127's `UseServer` for the lists and this issue's `UsePairingBackend` for
  pairing -- because the two landed in parallel and each needed one. Both are
  kept and the seam is written down in `engine.hpp`: a console has one client,
  and reconciling them belongs to #126, the issue that gives it one. Recorded on
  #31 as well, so whoever touches that file next does not add a third.
- **A merge commit was the wrong resolution, and `release.notes` said so.**
  Resolving with `git merge` was tried first because four commits touch the same
  regions; it failed `release.notes`, which builds notes with `--no-merges`
  because this repo squash-merges, so a merge commit at HEAD never appears in
  them. Redone as a rebase, and the final tree was pinned to the merge tree that
  had already been run, so what is being pushed is byte-identical to what passed.

## One thing added after the reviews, at the fleet monitor's request

`docs/WORKFLOW.md` Stage 3 now carries the rebase-not-merge rule. It is not a
finding from either pass -- it is the hour this branch lost, written where the
next agent meets it. The rule existed only as a comment beside
`git log --no-merges` in `scripts/release-notes.sh`, and the failure it produces
names the symptom rather than the cause. CLAUDE.md asks for exactly this ("when
you get the same correction twice, it belongs in this file or in a skill"), and
the monitor declined to make the edit itself for a good reason: a doc change
landing on `main` under three in-flight branches is the conflict it exists to
watch for, so it belongs in the branch that learned it.

## Rebased a second time, onto M6-2 (#33 / PR #129)

Beaten to `main` again while waiting on review, by a PR touching the same two
test files. Rebased, resolved three conflicts in `tests/test_engine.cpp` (all
"keep both sides": the scenario list, the `Console` helpers, `main`'s dispatch),
and re-ran everything: **281/281**, `switch.builds` and `release.notes` included.

Two process notes, both mine and both worth having on the record:

- **The first resolution ran `SyncNow`'s body into the next method**, because the
  two sides shared a closing brace. It failed to compile, which is the good case;
  it was fixed and squashed back into the commit whose resolution broke it, so
  the red-test commit still compiles on its own. Verified by checking that commit
  out and building it.
- **The `engine.*` aborts I saw first were my own artifact, not a defect.** To
  check that red-test commit I built it in the shared build directory, which left
  stale objects; the next run linked a `test_engine.o` and an `engine.cpp.o` from
  two different layouts and every engine scenario died with `mutex lock failed:
  Invalid argument`. A clean `rm -rf build` reproduced none of it. Recorded
  because the symptom looks exactly like a threading bug in the code this PR
  adds, and the next person to build an old commit in a shared build directory
  will see the same thing.

The structural half of this -- being overtaken twice while the review is the
slowest check, with each rebase invalidating it -- is written up on #116.

## Round two of the independent review

Two threads, neither a bug, both acted on in `74a07e9`.

- **An empty anonymous namespace** left behind when the previous review round
  replaced the fixed 200 ms tick with `next_poll_at()` and the constant went with
  it. Deleted.
- **`kNoSeed` answering `kInternal` while the comment above justified
  `kWriteFailed` "for both".** Fair catch on the comment, which described a
  two-way split over a three-way branch. Kept `kInternal` and wrote the reason
  in: `kUnreadable` and `kPersistFailed` are the card refusing, and
  `kWriteFailed` is true of them; `kNoSeed` is the platform layer handing over a
  seed it was required to refuse to substitute for, which is a defect in this
  build rather than a state a user is in, and `kWriteFailed` would send them to
  look at an SD card that is fine.

## Round three of the independent review: one Important, and it was real

`CommitGrant` held `mutex_` across `SaveToken` and `ClearBlock` -- two card
writes -- and that same mutex gates `Snapshot()` and `pairing_status()`, which
`GetStatus` and `GetPairState` serve and their screens poll every frame. A slow
card could stall the status and pairing screens for the length of the write.
`main.cpp` already promises `GetStatus` never goes near the card; this broke that
promise from the other side, and I had not seen it.

Fixed in `73d4de2`, but not by the suggested shape alone. Writing outside the
lock and nothing else would undo what the lock was for -- `Unpair` discards
`token.dat` while the pairing thread may be committing one, and those two must
not interleave. So there are two locks and an order:

- `card_mutex_` serialises writes to `token.dat` and `auth.json`, taken by the
  three methods that write them: `CommitGrant`, `Unpair`, and `ApplyConfigEdit`.
- `mutex_` guards the shared in-memory state and is never held across I/O.
- Order is `card_mutex_` then `mutex_`, never the reverse. The commands polled
  every frame take `mutex_` only, so they cannot queue behind an SD write.

`CommitGrant`'s supersession check moved *inside* `card_mutex_` and before the
write, which is what makes it a decision rather than a guess: a `server.url`
change clears the attempt and discards the token under that same lock, so it
cannot slip between the check and the write.

281/281 after the change, `switch.builds` included.

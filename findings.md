# Review findings — #118

Two passes, both on this branch, plus an independent `verifier` run.

## `/code-review high`

Ran against the working tree, and stress-ran the two-concurrent-`ctest` case the
change exists to fix: 40 concurrent `rig.smoke` runs, 0 failures (on `main` that
is the case that flaked).

1. **Important — `harness.fault_owner`'s untagged block still races a second
   `ctest`; 13 failures in 30 concurrent runs.** Path-scoping stops another run
   from *claiming* the untagged fault, but there is one untagged slot for the
   whole proxy, so a concurrent run's untagged *arm* replaces it.
   **Fixed:** those semantics moved out of the shared proxy entirely, into
   `proxy.ownership` (`tests/test_fault_proxy.py`), which drives a `Registry` in
   process. `harness.fault_owner` keeps only the owner-scoped assertions, which
   are race-free by construction.
2. **Important — `peek()`'s untagged fallback makes one leaked untagged fault
   fail `ExpectDisarmed` for every later rig test, and `_prune` never expired the
   untagged entry.** **Fixed:** the prune expires it too, and `peek` prunes
   before it answers. `peek` still reports the untagged scenario, deliberately:
   `ExpectDisarmed` asks "is anything about to damage me?", and an untagged fault
   is something this caller really can claim.
3. **Medium — `disarm()` popped the untagged entry, so any test deleted a
   stranger's fault.** **Fixed:** a `DELETE` clears the caller's own scenario and
   nothing else. Clearing the untagged one put every suite back to reaching into
   a fault it did not arm, which is #118 in the other direction.
4. **Low — the untagged arm was the one fault in the suite not RAII-scoped.**
   Moot: that block is gone (finding 1).
5. **Low — the new CMake header's "Every rig test below carries RUN_SERIAL" is
   not true** (`rig.smoke`, `rig.provisioned`, `engine.server`). **Fixed:** the
   comment now names the three exceptions.

Confirmed by the same pass and left alone: `_resolve`'s own-first-then-untagged
order, per-owner `after`/`count`, the header stripped before forwarding upstream,
and `OwnedHttpClient::Signed`'s prefix guard correctly leaving
`tests/loopback_server.hpp` traffic unsigned (`test_http_native`'s header
assertions still pass).

## `/mattpocock-skills:code-review`

### Standards

- **Important — `harness.fault_owner` used fixed tags** (`"...-mine"`,
  `"...-stranger"`), which two `ctest` runs would share. **Fixed:** both derive
  from `rig::FaultOwner()`.
- **Important — `tests/rig.hpp`'s rationale comment was factually wrong.** It
  claimed `test_conflicts` and `test_token_store` fork *and make requests from
  the child*; neither child makes a request. **Fixed:** the comment now says what
  is true — nothing forks a client today, and the export is what keeps that from
  silently mattering.
- Smell: the #118 narrative was restated in six places. **Fixed** by trimming
  `rig.hpp`'s copy to a pointer at docs/TESTING.md, which is now the one home.
- Smell: `OwnedHttpClient::owner()` had no caller. **Fixed:** deleted.
- Smell: `Registry._drop` rescanned the dict by identity. **Fixed:** `_resolve`
  returns the key, and `claim` deletes by it.
- Nit: an unwrapped line in docs/TESTING.md. **Fixed.**
- Noted, not changed: `"X-Fault-Owner"` is spelled in three places (Python,
  `rig.hpp`, and raw libcurl in `test_rig_smoke.cpp`). Two languages and one
  deliberately independent copy; sharing it is not available.

### Spec

- **The failure should "name the real cause".** Ownership removes the common
  case but cannot separate the untagged scenario, which anybody may claim.
  **Fixed as far as it goes:** the proxy now logs `<owner> claimed the untagged
  scenario on <path>` when a tagged client takes it.
- **Scope creep — `disarm()` also popping the untagged entry.** Agreed and
  reversed (see finding 3 above).
- **Scope creep — `OWNER_TTL_SECONDS` / `_prune`.** Kept, and now load-bearing:
  since no client may clear another's scenario, age is the only thing that can.
- **Scope creep — `download.*`'s rewritten RUN_SERIAL rationale invented a
  bandwidth argument.** **Fixed:** it now names the real reason, one scratch
  directory under fixed names (#151).
- **`test_rig_smoke.cpp` tagged itself with a bare pid**, the hazard its sibling
  file documents. **Fixed:** random suffix added.
- **`claim()` asymmetry: an owner whose own fault does not match the path does
  not fall through to the untagged one.** Deliberate, now stated in `_resolve`'s
  docstring and asserted by `proxy.ownership`.
- Acceptance items all met; #109 untouched, as the issue requires.

## `verifier` (independent, fresh context, deleted `build/`)

Verdict **READY**. From-scratch build with no warnings under `-Werror`, and
`376/376` green with RomM up — nothing skipped, `rig.smoke` and `rig.provisioned`
included. It reproduced the pre-fix failure by running the freshly built HEAD
binary against `main`'s `fault_proxy.py` on a spare port:

```
FAIL: a stranger's heartbeat is untouched by a fault it did not arm -- expected 200, got 418
FAIL: and my third is the one that fails -- expected 418, got 200
```

Its one judgement call, left as it is and recorded here: an untagged fault now
survives every test's cleanup, so a forgotten manual `curl` can damage a run for
up to `OWNER_TTL_SECONDS`. It degrades to a loud red — `ExpectDisarmed` prints
the scenario the proxy holds — rather than a silent one, and the alternative is
letting any suite delete a fault a human armed.

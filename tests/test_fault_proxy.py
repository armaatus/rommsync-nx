#!/usr/bin/env python3
"""The fault proxy's ownership rules, driven in process (#118).

`harness.fault_owner` proves the same rules end to end against the running
proxy, and that is the test that matters -- it damages a real response over a
real socket. What it cannot prove is anything about the UNTAGGED scenario: there
is one of those for the whole proxy by design, so a second `ctest` arming its own
replaces the one an assertion here was about. Asserting on it against the shared
proxy is a flake in the test written to rule flakes out.

So the untagged half lives here, over a `Registry` this process owns. No socket,
no container, nothing shared: it never skips, and it is the only place the
fallback, the prune and "a disarm clears your own and nothing else" are stated as
assertions rather than as a docstring.
"""
import pathlib
import sys
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "server" / "testing"))

import fault_proxy  # noqa: E402  -- the path above is what makes it importable

ANONYMOUS = fault_proxy.ANONYMOUS


def status(**overrides) -> dict:
    spec = {"mode": "status", "status": 418}
    spec.update(overrides)
    return spec


class Ownership(unittest.TestCase):
    def setUp(self) -> None:
        self.registry = fault_proxy.Registry()

    def claim(self, owner: str, path: str = "/api/heartbeat"):
        spec, _ = self.registry.claim(path, owner)
        return spec

    # -- the defect itself -----------------------------------------------------
    def test_a_stranger_does_not_spend_a_positional_budget(self):
        self.registry.arm("mine", status(after=2, count=1))
        for _ in range(5):
            self.assertIsNone(self.claim("stranger"))
        self.assertIsNone(self.claim("mine"))
        self.assertIsNone(self.claim("mine"))
        self.assertIsNotNone(self.claim("mine"))

    def test_arming_does_not_overwrite_a_stranger(self):
        self.registry.arm("mine", status(status=418))
        self.registry.arm("stranger", status(status=429))
        self.assertEqual(self.claim("mine")["status"], 418)
        self.assertEqual(self.claim("stranger")["status"], 429)

    def test_count_disarms_only_the_owner_that_spent_it(self):
        self.registry.arm("mine", status(count=1))
        self.registry.arm("stranger", status(count=1))
        self.assertIsNotNone(self.claim("mine"))
        self.assertIsNone(self.registry.peek("mine"))
        self.assertIsNotNone(self.registry.peek("stranger"))

    # -- the untagged scenario, which is global on purpose ---------------------
    def test_an_untagged_scenario_applies_to_everybody(self):
        # The documented one-line `curl` in CLAUDE.md arms without a tag and
        # expects the next request through the proxy to be the one damaged,
        # whoever makes it.
        self.registry.arm(ANONYMOUS, status(count=2))
        self.assertIsNotNone(self.claim("mine"))
        self.assertIsNotNone(self.claim(ANONYMOUS))
        self.assertIsNone(self.claim("stranger"))

    def test_your_own_scenario_wins_over_the_untagged_one(self):
        self.registry.arm(ANONYMOUS, status(status=503))
        self.registry.arm("mine", status(status=418))
        self.assertEqual(self.claim("mine")["status"], 418)
        self.assertEqual(self.claim("stranger")["status"], 503)

    def test_your_own_scenario_is_not_a_fallthrough(self):
        # An owner holding a scenario for another path is not falling through to
        # somebody else's fault. Yours is yours, matching or not.
        self.registry.arm(ANONYMOUS, status(status=503))
        self.registry.arm("mine", status(status=418, path="/api/saves"))
        self.assertIsNone(self.claim("mine", "/api/heartbeat"))

    def test_a_claim_names_who_it_came_from(self):
        # What lets the proxy log the one case ownership cannot separate.
        self.registry.arm(ANONYMOUS, status())
        _, claimed_from = self.registry.claim("/api/heartbeat", "mine")
        self.assertEqual(claimed_from, ANONYMOUS)

    # -- disarming -------------------------------------------------------------
    def test_a_disarm_clears_your_own_and_nothing_else(self):
        self.registry.arm(ANONYMOUS, status(status=503))
        self.registry.arm("mine", status(status=418))
        self.registry.disarm("mine")
        # The untagged one survives: clearing it would put every suite back to
        # reaching into a scenario it did not arm.
        self.assertEqual(self.registry.peek("mine")["status"], 503)
        self.registry.disarm(ANONYMOUS)
        self.assertIsNone(self.registry.peek("mine"))

    def test_peek_reports_what_can_damage_you(self):
        self.registry.arm(ANONYMOUS, status(status=503))
        self.assertIsNotNone(self.registry.peek("mine"))

    # -- the registry does not grow forever -----------------------------------
    def test_an_expired_scenario_does_not_fire_on_the_request_path(self):
        # The path every proxied request takes, and the one nothing sweeps: a
        # `curl` that armed a fault and went to lunch damages traffic whether or
        # not anybody arms or peeks in between, so `claim` has to find it dead.
        self.registry.arm(ANONYMOUS, status())
        for fault in self.registry._faults.values():
            fault.armed_at = time.monotonic() - fault_proxy.OWNER_TTL_SECONDS - 1
        self.assertIsNone(self.claim("mine"))

    def test_an_abandoned_scenario_expires(self):
        # A client killed between arming and firing, and the `curl` whose author
        # moved on. Nobody else may clear either, so age has to.
        self.registry.arm("gone", status())
        self.registry.arm(ANONYMOUS, status())
        for fault in self.registry._faults.values():
            fault.armed_at = time.monotonic() - fault_proxy.OWNER_TTL_SECONDS - 1
        self.assertIsNone(self.registry.peek("gone"))
        self.assertIsNone(self.registry.peek("mine"))


if __name__ == "__main__":
    unittest.main(verbosity=2)

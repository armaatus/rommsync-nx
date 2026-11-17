# Selftest reports

The reports [the on-console selftest](../../selftest/README.md) (M12-1, #306)
wrote on real consoles, copied off the card as they were written:
`report-<YYYYMMDDTHHMMSSZ>.json`, one per run, never edited. Each is named from
its run's PRE-02 row in [HARDWARE_RESULTS.md](../HARDWARE_RESULTS.md).

The launch gate's `selftest` row (`scripts/launch-gate.sh`) is settled by:

```sh
scripts/selftest-judge.py --pairs 2 docs/selftest/report-*.json
```

> **No report is committed yet.** None may be before `./scripts/v1-gate.sh`
> exits 0 (CLAUDE.md, hard rule 1).

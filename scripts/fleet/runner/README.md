# Runner drivers

`lib.sh` sources `$AUTOFLEET_RUNNER.sh` from this directory. The contract every
driver meets is in [../../../docs/RUNNERS.md](../../../docs/RUNNERS.md).

Anything in this directory may know about one runner. Nothing outside it may —
and that is checkable rather than aspirational:

```sh
grep -rni 'orca' scripts/fleet --include='*.sh' \
  | grep -v '^scripts/fleet/runner/' \
  | awk 'BEGIN { sq = sprintf("%c", 39) }
    {
      code = $0
      sub(/^[^:]*:[0-9]+:/, "", code)   # drop path:lineno:
      # Truncate at the first # that is NOT inside quotes, so a trailing comment
      # is prose and a `"#$num"` in the middle of a line does not hide the rest.
      out = ""; q = ""
      for (i = 1; i <= length(code); i++) {
        c = substr(code, i, 1)
        if (q == "") {
          if (c == "\"" || c == sq) q = c
          else if (c == "#") break
        } else if (c == q) q = ""
        out = out c
      }
      if (tolower(out) ~ /orca/) print
    }' \
  | grep -v '^scripts/fleet/config.sh:[0-9]*:: "${AUTOFLEET_RUNNER:=orca}"$'
```

The comment is STRIPPED rather than the line skipped, so `FOO=1  # not orca` is
prose and `ORCA_DEADLINE=20  # a knob` is a leak. `config.sh`'s
`AUTOFLEET_RUNNER` default is allowed by name: prose may say `orca`, and the file
that CHOOSES the driver may name one. Code may not.

[docs/RUNNERS.md](../../../docs/RUNNERS.md) carries the same command and is the
definition; this copy is a convenience. Running it against a diff is a line in
`.autofleet/review.md` rather than a build step (armaatus/autofleet#153).

`tests/test_fleet.sh runner_stub` is the other half: it drives the whole fleet
on a driver that is not Orca and fails if anything reaches for the CLI.

# rommsync-selftest

M12-1 (#306): the on-console selftest. A homebrew `.nro` someone launches from
hbmenu against a **disposable** RomM, which runs the sysmodule's real Horizon
backends and core/'s real engine calls, and writes a report a host script judges.

It is the first thing run on a console, before any sysmodule is installed
([HARDWARE_TEST_PLAN.md, PRE-02](../docs/HARDWARE_TEST_PLAN.md#pre-02-the-selftest-nro-first-no-sysmodule-yet)).
**Building it is fine now; running it waits for `./scripts/v1-gate.sh` to exit
0** (CLAUDE.md, hard rule 1). Nothing below has run on a console yet: every row
is a question the first run answers, not a claim.

## What it links

- core/, compiled in whole, as the sysmodule compiles it.
- `sysmodule/source/http/` -- the `ssl` transport, `MakeHorizonHttpClient`.
- `sysmodule/source/card.cpp`, through `source/sysmodule_card.cpp`.

Never copies of them: a row that passes here passes for the code that ships.
`ctest -R switch.selftest` builds it in the pinned devkitPro image and checks
the image links those symbols; the `switch-build` CI job builds and uploads it.

## Setting it up

1. Copy `rommsync-selftest.nro` (switch-build's artifact) to `/switch/` on the
   card.
2. Write `/config/rommsync-selftest/selftest.ini`. With none, the NRO prints
   this template and sends nothing anywhere -- it has no default target, for
   tlsprobe's reason:

   ```ini
   url = https://romm.example.lan       # a DISPOSABLE RomM, never your own
   ca_file = sdmc:/config/rommsync-selftest/ca.pem   # only for a private CA
   rom_id = 12                          # a single-file homebrew rom on it, 2 B to 8 MiB
   bad_tls_url = https://192.168.1.20/  # same server by IP: the name check fails
   # blackhole_host = 10.255.255.1      # an address that routes nowhere
   # stall_url = https://192.168.1.20:9443/   # accepts, never answers
   # tls13_url = https://romm-tls13.example.lan/
   ```

   The docker fixture provides all of it but a routable address: put RomM
   behind `./scripts/orca/tls-fixture.sh` on a LAN interface. The fault proxy
   armed with `stall` is a `stall_url`.
3. Set the console's time zone **far from UTC** (UTC+9): the mtime row cannot
   tell UTC from local time in a zone within an hour of it, and fails rather
   than guess (CLK-04).
4. On an exFAT card, leave more than 4 GiB free: the format is learned by asking
   the card for a file FAT32 cannot hold.

## The run

Launch it, approve its device code in RomM when it shows one (B gives up), and
press + when it says so. It pairs as a device of its own -- an identity from
entropy alone, never the console's serial -- so it never shares a token or a
device with the sysmodule.

**Everything it writes is under `sdmc:/config/rommsync-selftest/`**: the report,
its log, its own `token.dat` and `device.dat`, and a `scratch-<ts>/` folder
holding the card rows' files, the fixture save, its backups and state, and the
downloaded rom. Never `/config/rommsync/`, never an emulator's folder
(`ctest -R selftest.roots`). Delete the folder when done; it holds a token for
the disposable server.

## The report

`sdmc:/config/rommsync-selftest/report-<YYYYMMDDTHHMMSSZ>.json`, rewritten after
every row, so a run that dies leaves the rows it got through:

```json
{
  "schema": 1,
  "tool": "rommsync-selftest",
  "version": "<VERSION>",
  "started": "2026-12-01T10:00:00Z",
  "console": {"hos": "21.0.0", "ams": "1.10.1", "emummc": true, "sd_format": "fat32"},
  "server": "https://romm.example.lan",
  "rows": [
    {"id": "net.tls", "result": "pass", "detail": "HTTP 200 from ... in 412 ms"}
  ]
}
```

`result` is `pass`, `fail`, `skip` or `na`; `value`, when present, is the
measurement. The rows, in the order they run:

| Row | Passes when |
|---|---|
| `platform.hos` | `setsysGetFirmwareVersion` answers |
| `platform.ams` | Atmosphere's spl config item 65000 answers |
| `platform.emummc` | item 65007 answers (`value` says which) |
| `platform.sd_format` | FAT32 or exFAT is learned (a 4 GiB + 1 file refused or made) |
| `platform.free_space` | the card backend reports 64 MiB or more free |
| `platform.mtime_utc` | a file written now has a UTC `st_mtime`, in a zone far from UTC -- [SYNC_PROTOCOL.md](../docs/SYNC_PROTOCOL.md#the-consoles-clock-295)'s open question |
| `card.atomic_write` | `io::WriteAtomically` writes, renames over, reads back, leaves no `.tmp` |
| `card.commit_device` | every `fsdevCommitDevice("sdmc:")` the writes asked for succeeded |
| `card.backup_then_commit` | `sync::BackUpFirst` then an overwrite: the backup holds the old bytes |
| `card.fat32_ceiling` | on FAT32, the card backend caps a file at 4 GiB; `na` on exFAT |
| `card.long_unicode_path` | a ~250-byte Japanese/accented path is made, listed and read back |
| `net.nifm` | nifm says online |
| `net.dns` | the server's host resolves |
| `net.tls` | a verified request to `/api/heartbeat` answers 2xx |
| `platform.clock` | `sync::CheckClock` trusts the clock against the server's `Date` |
| `net.tls13_only` | *optional*: `tls13_url` answers; `skip` without one |
| `net.handshake_leak` | 64 failed handshakes, then a good request still works -- the `ENOENT` descriptor question in [TESTING.md](../docs/TESTING.md#what-the-first-console-decides) |
| `net.timeout` | a connect into `blackhole_host` gives up within 3 s + 2 s; with `stall_url`, so does a stalled read |
| `net.connect_50` | 50 requests answer, and the heap moves under 8 KiB after the fifth |
| `proto.pair` | device-code pairing ends with a stored token |
| `proto.negotiate` | `sync::Negotiate` on the fixture save answers with a plan |
| `proto.tick` | `sync::RunTick` on the fixture save completes, nothing failed |
| `proto.download_resume` | the rom, half staged as a `.part`, resumes with a 206 to the same bytes |
| `footprint.heap` | peak C++ heap + bsd transfer memory fits the sysmodule's 1.5 MiB heap |

A row that could not run because an earlier one failed (no token, no fixture
save) is written as `fail` with `not run:` and the reason.

## Judging it

```sh
scripts/selftest-judge.py report-20261201T100000Z.json
scripts/selftest-judge.py --pairs 2 docs/selftest/report-*.json   # the launch gate's row
scripts/selftest-judge.py --rows                                  # what is judged
```

One line per row -- `PASS`, `FAIL`, or `MISSING` -- then `VERDICT PASS|FAIL`.
A report that is not one this NRO could have written is `MALFORMED` and exit 2.
`--pairs N` needs N distinct HOS/Atmosphere pairs among the passing reports.
The judge's `ROWS` table is the contract: `ctest -R selftest.rows` holds this
directory to exactly its ids. Commit the reports a run judged under
[`docs/selftest/`](../docs/selftest/README.md).

## Where it departs from #306

- **The download is interrupted by staging half of it, not by airplane mode.**
  The row then means the same on every run; a real drop mid-transfer is the
  plan's NET scenarios.
- **The heap is measured as `alloc.*` measures it** -- every C++ allocation,
  counted (`heap_count.cpp`) -- plus the bsd transfer memory, against the
  sysmodule's heap. An NRO's own heap is the loader's and far larger, so the
  question asked is whether the same work would have fitted.
- **No NPDM or stack check.** An NRO runs under hbloader's NPDM, on hbloader's
  main thread; `npdm.svcs` and `stack.measured` stay the sysmodule's.

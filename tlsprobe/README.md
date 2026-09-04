# rommsync-tlsprobe (the M0-1 spike)

One question, one program: **can a process with a sysmodule-sized heap do TLS by
borrowing Horizon's `ssl` service instead of carrying its own stack?**

This is a `.nro` a human launches from the homebrew menu and closes again. It is
**not** a sysmodule, **not** on the boot path, and **not** installed anywhere. An
`.nro` that will not run unless someone starts it is the property that makes this
experiment safe to run on a console at all — and even that is M8 work, behind the
**M8-1** gate (`CLAUDE.md` hard rule 1).

The answer, the measurements and the go/no-go live in
[docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision).
This page is how to build it and how to run it.

```bash
make -C tlsprobe                     # -> tlsprobe/rommsync-tlsprobe.nro
make -C tlsprobe clean
ctest --test-dir build -R switch.tlsprobe   # the same build, in the CI image
./tlsprobe/measure-footprint.sh      # ssl vs mbedTLS, in code bytes
```

## What it does

One HTTPS GET, instrumented at every stage:

```
socketInitialize (trimmed)   ->  bsd transfer memory lands on our heap
sslInitialize + CreateContext ->  optional ImportServerPki for a fixture CA
socket + connect (with a deadline)
CreateConnection + socketSslConnectionSetSocketDescriptor
SetHostName / SetVerifyOption / SetIoMode
DoHandshake  ->  Write(GET)  ->  Read until EOF
```

and prints, to the screen and to `sdmc:/switch/rommsync-tlsprobe.log`: the HTTP
status, header and body byte counts, connect/handshake/total timings, the
negotiated cipher, and a heap table — `mallinfo().uordblks` and the kernel's
`UsedMemorySize` at baseline, after each stage, at the peak while reading, and
after everything is closed again. That last row is the one to read twice: a
teardown that has not returned to baseline is a leak a resident sysmodule would
pay on every sync tick.

## Running it

It has **no compiled-in target**. With no ini it prints the ini it wants and
stops — a probe with a default host is a probe that can reach something nobody
chose, and the thing it must never reach is a production RomM.

1. Stand up a TLS endpoint in front of this worktree's RomM and get the ini:

   ```bash
   ./scripts/orca/tls-fixture.sh up
   ./scripts/orca/tls-fixture.sh ini
   ```

2. Put that on the SD card (or the emulator's virtual one) as
   `sdmc:/switch/rommsync-tlsprobe.ini`, next to
   `sdmc:/switch/rommsync-fixture-ca.pem` — a copy of
   `server/testing/tls/generated/server.crt`.
3. Copy `rommsync-tlsprobe.nro` to `sdmc:/switch/`, launch it, read the report,
   press `+`.

The fixture binds `127.0.0.1` like every other port this rig publishes. That is
enough for an emulator running on this machine and deliberately not enough for a
console on the LAN: pointing hardware at anything is M8 work.

### The ini

| key | meaning |
|---|---|
| `host`, `port`, `path` | what to GET. No defaults for `host`. |
| `sni` | the name asked for and verified against; defaults to `host`. Set it to the name on the fixture certificate and hostname verification can stay **on** against an IP. |
| `ca_pem` | a PEM handed to `sslContextImportServerPki`. Omit for a server the console's own CertStore already trusts. |
| `verify` | `0` clears `PeerCa\|HostName` — which needs `SkipDefaultVerify` first, and is the one thing in here that is a real security decision (docs/SECURITY.md). |
| `read_buf` | the in-flight buffer size, i.e. the download buffer under another name. |
| `connect_timeout_ms`, `stall_timeout_ms` | a probe that hangs proves nothing. |
| `tcp_tx`, `tcp_rx`, `tcp_tx_max`, `tcp_rx_max`, `udp_tx`, `udp_rx`, `sb_efficiency`, `bsd_sessions` | the bsd transfer-memory budget. These *are* the heap answer, so an alternative budget is measurable by editing the ini rather than the source. |

## Why it is built the way it is

- **`core/` is not compiled in.** The sysmodule already proves `core/` links for
  aarch64 (`switch.builds`). Here every byte of `.text` and `.bss` has to be
  attributable to the TLS path being measured, so the engine stays out —
  `switch.tlsprobe` asserts it did.
- **The measuring apparatus is not on the heap it measures.** The config and the
  report are fixed buffers; the two deliberate allocations are the CA PEM (freed
  after the import) and the read buffer, both of which a real sysmodule would
  also pay.
- **One run per launch.** No retry loop, no persistence, nothing installed.

## Its future

This is a spike, not a shipped component. The real TLS backend is an
`HttpClient` implementation behind
[`core/include/rommsync/http.hpp`](../core/include/rommsync/http.hpp), written at
M8 when there is hardware to prove it on — and `tlsprobe/` is the thing to run
first when that day comes. It may be retired once that backend exists and is
proven; its assertions are deliberately in a `switch.tlsprobe` phase of their
own so retiring it does not disturb the two targets that ship.

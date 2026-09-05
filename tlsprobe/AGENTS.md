# tlsprobe/ — the M0-1 spike

A manually-launched `.nro` that does one TLS GET through the Horizon `ssl`
service and reports what it cost. Read [README.md](README.md) first, and
[docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision)
for the answer it produced.

- **It is not a sysmodule and must never become one.** No `CONFIG_JSON`, no
  install step, nothing on the boot path. That is what makes it safe to run on a
  console at M8, and it is the property to preserve if this file is edited.
- **It has no default target.** With no ini it prints the ini it wants and stops.
  Do not add a fallback host; the one it must never reach is a production RomM.
- **`core/` stays out of this build.** The footprint is the deliverable, and it
  is only attributable if the image holds the TLS path and nothing else.
  `switch.tlsprobe` fails if a core/ symbol appears in the link.
- The real backend is not here. It is an `HttpClient` implementation behind
  `core/include/rommsync/http.hpp`, written at M8.

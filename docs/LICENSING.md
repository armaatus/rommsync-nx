# Licensing

What the release zip is licensed under, and why. The short version:

| Binary | Licence | Why |
|---|---|---|
| `ovl-rommsync.ovl` (the overlay) | **GPL-2.0** | it statically links libultrahand, which is GPL-2.0 |
| `exefs.nsp` (sys-rommsync, the sysmodule) | **MIT** | it links nothing that reaches it under the GPL |
| this repository's own source | **MIT** | [`LICENSE`](../LICENSE) |

`rommsync-selftest.nro`, the on-console selftest (M12-1, #306), is not in the
zip: testers get it as `switch-build`'s CI artifact
([HARDWARE_TEST_PLAN](HARDWARE_TEST_PLAN.md) PRE-02). It is **MIT** for the
sysmodule's reason. It links `core/`, the sysmodule's HTTP backend, libnx,
newlib and the GCC runtime, and no libultrahand or portlibs
([`selftest/Makefile`](../selftest/Makefile)). The notices it owes are the
sysmodule's rows of `THIRD_PARTY_NOTICES.txt`; the artifact carries the `.nro`
alone, so a tester who wants them reads them from a release zip.

The zip ships all of it under `config/rommsync/`: `LICENSE` (MIT),
`COPYING.GPL-2.0`, and `THIRD_PARTY_NOTICES.txt`, which
[`scripts/notices.sh`](../scripts/notices.sh) generates from
[`packaging/third_party.manifest`](../packaging/third_party.manifest) (#300).
Nothing goes to the SD root: that directory belongs to every piece of homebrew
on the card, and a `README.txt` or `LICENSE` there is somebody else's file.

This is an engineering reading of the licences, written down so the next change
to what is linked starts from it rather than re-deriving it. It is not legal
advice.

## What is linked, and under what

Read off the linker's own map files (`sysmodule/build/sys-rommsync.map`,
`overlay/build/ovl-rommsync.map`), not off the Makefiles alone: the specs file
links libc, libstdc++ and libgcc without any `-l` naming them. Versions are the
pinned image's (`devkitpro/devkita64:20260219`, [`scripts/toolchain.sh`](../scripts/toolchain.sh))
and the submodule pointer's.

| Component | Version | Licence | Taken as | In |
|---|---|---|---|---|
| libultrahand (libtesla, libultra) | v2.5.4 | GPLv2; libtesla GPL-2.0-or-later; libultra GPLv2 *and* CC-BY-4.0 | GPL-2.0-only | overlay |
| cJSON (vendored in libultrahand) | 1.7.18 | MIT | MIT | overlay |
| stb_truetype (vendored in libtesla) | 1.26 | MIT OR Unlicense | MIT | overlay |
| Mbed TLS | 2.28.10 | Apache-2.0 OR GPL-2.0-or-later | **GPL-2.0-or-later** | overlay |
| curl | 7.69.1 | curl (MIT-style) | curl | overlay |
| zlib, minizip | 1.3.1 | Zlib | Zlib | overlay |
| libnx | 4.12.0 | ISC | ISC | both |
| newlib, libsysbase | 4.6.0.20260123 | newlib's collection (BSD-style) | as is | both |
| libstdc++, libgcc | 15.2.0 | GPL-3.0-or-later WITH GCC-exception-3.1 | as is | both |

The overlay links curl, zlib, minizip and Mbed TLS only because libultrahand's
objects reference them (its updater and package handling); ovl-rommsync itself
does no networking ([`switch.mk`](../switch.mk), the `ROMMSYNC_ULTRAHAND` block).

## The conclusions

**libultrahand is GPL-2.0, and the overlay binary with it.** Its top-level
`LICENSE` is the GPL-2.0 text and its README says "Licensed under GPLv2", with
no "or any later version" grant at that level. libtesla's header does grant
"or later", and libultra's files say "Licensed under both GPLv2 and CC-BY-4.0";
the narrowest common reading is GPL-2.0-only, and that is the one taken. A
statically linked `.ovl` is one work, so the binary as a whole is distributed
under GPL-2.0. Everything else in it has to be compatible with that, and is:

- **Mbed TLS** is the one that needed a choice. Apache-2.0 is incompatible with
  GPL-2.0 (its patent and indemnity terms are "further restrictions" under
  GPL-2.0 §6). Mbed TLS 2.28 is dual-licensed — every header in the image says
  `SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later` — so the overlay
  takes it under GPL-2.0-or-later. The issue expected 3.x; the image ships
  2.28.10, and the 2.28 branch carries the same dual licence.
- **curl, zlib, minizip, libnx, cJSON, stb_truetype, newlib** are permissive
  and GPL-2.0-compatible. What they ask is attribution: their copyright and
  permission notices in the distribution. `THIRD_PARTY_NOTICES.txt` carries
  each one in full.
- **libstdc++ and libgcc** are GPL-3.0 with the GCC Runtime Library Exception,
  which lets a program compiled by GCC link them under whatever terms it likes.
  That exception is also why GPL-3.0 code inside a GPL-2.0-only binary is no
  conflict: the runtime's licence does not reach the combined work at all. They
  put no obligation on either binary; they are in the notices so the list is
  complete.

**The sysmodule is MIT.** It links libnx, newlib and the GCC runtime and
nothing else — no libultrahand, no portlibs — so nothing makes it a GPL work.

**The source offer is the tag, and an archive of it on the release.** GPL-2.0
§3 lets the source be offered "from the same place" as the binary. GitHub's
automatic "Source code" archives hold libultrahand as an empty directory, so
every release also carries `rommsync-nx-<VERSION>-source.tar.gz` (#320):
the tagged tree with every submodule's tree, at the commit the tag pins,
inside it. [`scripts/source-archive.sh`](../scripts/source-archive.sh) builds
it out of git objects, deterministically, and the release job checksums,
attests and uploads it beside the zip. The notes and `THIRD_PARTY_NOTICES.txt`
both link it, and `tree/v<VERSION>` besides; the notices name the source of
every other component at its exact version.

`ctest -R package.source` holds the archive to exactly the tag plus the pinned
submodule; `ctest -R package.source_builds` unpacks it alone and builds both
binaries in the pinned image with Docker's network cut off.

What it does not carry is the weakest part of this, and worth knowing: the
Mbed TLS, curl and zlib sources are linked at their upstream tags rather than
hosted here. Those come prebuilt in the pinned devkitPro image, which is the
one thing a rebuilder brings, and the archive builds offline against it. The
strictest reading of §3 — "complete source code ... for all modules it
contains" offered by the distributor — would also attach those portlibs'
source tarballs; #320 made that optional and it is not done.

## Keeping it true

- `ctest -R package.notices` refuses a `-l` in `switch.mk` or a target's
  Makefile, or a `.gitmodules` submodule, with no manifest row — and checks
  every row's licence text reaches the generated notices, the libultrahand row
  names the pinned commit, and the shipped GPL text is libultrahand's own.
- `ctest -R package.builds` does the same against the real link maps from a
  devkitPro build, binary by binary: an archive the linker pulled in with no
  row, or in a binary its row does not list, is red.
- `scripts/package.sh` runs the static check before it packages anything, so a
  release job cannot zip a build the manifest does not cover.

Moving the image pin or the libultrahand submodule means revisiting the
versions in the manifest; adding a library means a row and its text under
`packaging/licenses/`. A new licence that is not GPL-2.0-compatible cannot go
into the overlay at all.

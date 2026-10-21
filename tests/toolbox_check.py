#!/usr/bin/env python3
"""Is this a `toolbox.json` ovl-sysmodules would actually list -- and survive?

`atmosphere/contents/<TID>/toolbox.json` is not Atmosphere's file: it is the one
[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules) reads to build
its list, and it is the difference between a sysmodule a user can toggle and one
that is simply absent from the only screen the install guide sends them to (#33).

That overlay's `GuiMain::GuiMain` skips a directory whose `toolbox.json` is
missing, is larger than 4096 bytes, does not parse, or lacks `tid` (a string),
`name` (a string) or `requires_reboot` (a bool). None of that is reported
anywhere: the module is not in the list, and nothing on the console says why.

Some of it is worse than a skip (M9-12, #211). ovl-sysmodules builds
`-fno-exceptions` and vendors nlohmann/json, which then defines
`JSON_THROW(exception)` as `std::abort()`, and its `gui_main.cpp` has no
try/catch: a wrong-typed value -- `"requires_reboot": "false"`, a string --
aborts the whole overlay, taking every other sysmodule's entry down with it. So
this is strict where that overlay is fatal: the exact key set, each key's exact
JSON type, and a `tid` that is 16 hex digits.

  tests/toolbox_check.py <path> <expected tid> <expected version> [--program-id HEX]

`tid` must equal the expected tid AND the name of the directory the file sits
in, since that directory is what Atmosphere loads from. `--program-id` is the
NPDM's, as `scripts/npdm-check.py --nsp ... --print` reads it out of the built
`.nsp` -- the id the kernel actually gives the process, not the JSON that
generated it.

Used by `tests/test_package.sh` against the *packaged* file, so what is checked
is what a user unzips rather than the template it came from.
"""
import argparse
import json
import os
import re
import sys

# ovl-sysmodules reads the file into a 4096-byte static buffer and skips
# anything longer, without reading it.
MAX_BYTES = 4096

# Exactly these. The three ovl-sysmodules requires, plus `version`, which it
# appends to the row's label (#129) -- and which is held to VERSION below,
# because a version that does not move with the build names the wrong one on a
# support thread. Anything else is a key nobody reads, in a file a typo in
# which is fatal to somebody else's overlay.
KEYS = {"tid": str, "name": str, "version": str, "requires_reboot": bool}

TID = re.compile(r"[0-9A-Fa-f]{16}")


def check(raw, tid, version, directory, program_id):
    """The first reason this file is not one to ship, or None."""
    if not raw:
        return "toolbox.json is empty"
    if len(raw) > MAX_BYTES:
        return f"toolbox.json is {len(raw)} bytes; ovl-sysmodules reads at most {MAX_BYTES}"

    duplicates = []

    def pairs(items):
        seen = {}
        for key, value in items:
            if key in seen:
                duplicates.append(key)
            seen[key] = value
        return seen

    try:
        doc = json.loads(raw, object_pairs_hook=pairs)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        return f"toolbox.json does not parse ({error}); the module is skipped in silence"
    if not isinstance(doc, dict):
        return "toolbox.json is not a JSON object"
    # Python keeps the last of two; whether nlohmann does is not a question to
    # leave to it.
    if duplicates:
        return f"toolbox.json names {duplicates[0]!r} twice"

    missing = sorted(KEYS.keys() - doc.keys())
    extra = sorted(doc.keys() - KEYS.keys())
    if missing:
        return f"toolbox.json has no {missing[0]!r}; ovl-sysmodules skips the module"
    if extra:
        return f"toolbox.json carries {extra[0]!r}, which is not one of {sorted(KEYS)}"
    for key, kind in KEYS.items():
        # `bool` is a subclass of `int` and `str` is not one of `bool`, so the
        # comparison is on the exact type: a JSON `true` where a string belongs,
        # or a `"false"` where a bool does, is the abort described above.
        if type(doc[key]) is not kind:
            return (f"toolbox.json's {key!r} is a JSON {type(doc[key]).__name__}, not a "
                    f"{kind.__name__}; nlohmann aborts ovl-sysmodules on that")

    if not TID.fullmatch(doc["tid"]):
        return f"toolbox.json's tid {doc['tid']!r} is not 16 hex digits"
    if doc["tid"] != tid:
        return f"toolbox.json says tid {doc['tid']!r}, the archive installs under {tid}"
    if doc["tid"] != directory:
        return f"toolbox.json says tid {doc['tid']!r} and sits in {directory!r}"
    # `strtoul(..., 16)`, which is what an unsubstituted placeholder becomes.
    if int(doc["tid"], 16) == 0:
        return "toolbox.json's tid reads as program id 0"
    if program_id is not None and int(doc["tid"], 16) != int(program_id, 16):
        return (f"toolbox.json says tid {doc['tid']}, the built NPDM's program id is "
                f"{program_id}; ovl-sysmodules would start a process that is not this one")
    if not doc["name"]:
        return "toolbox.json's name is empty; the list row would have no label"
    # The claim this project has to keep: sys-rommsync is killed outright with
    # `pmshellTerminateProgram` and relaunched with no gap, which is what the
    # `toggle.*` suite proves. `true` here would move it to the Static section,
    # where the switch does nothing until the console is rebooted.
    if doc["requires_reboot"] is not False:
        return "requires_reboot must be false: sys-rommsync is a dynamic module"
    if doc["version"] != version:
        return f"toolbox.json says version {doc['version']!r}, this build is {version}"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("path")
    parser.add_argument("tid")
    parser.add_argument("version")
    parser.add_argument("--program-id",
                        help="the built NPDM's program id, as npdm-check.py --print reads it")
    args = parser.parse_args()

    with open(args.path, "rb") as handle:
        raw = handle.read()
    directory = os.path.basename(os.path.dirname(os.path.abspath(args.path)))
    problem = check(raw, args.tid, args.version, directory, args.program_id)
    if problem:
        print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    print(f"ok: ovl-sysmodules would list {args.tid} as a dynamic module, and survive reading it")
    return 0


if __name__ == "__main__":
    sys.exit(main())

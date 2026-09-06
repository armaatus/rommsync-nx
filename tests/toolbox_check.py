#!/usr/bin/env python3
"""Is this a `toolbox.json` ovl-sysmodules would actually list?

`atmosphere/contents/<TID>/toolbox.json` is not Atmosphere's file: it is the one
[ovl-sysmodules](https://github.com/ppkantorski/ovl-sysmodules) reads to build
its list, and it is the difference between a sysmodule a user can toggle and one
that is simply absent from the only screen the install guide sends them to (#33).

That overlay's `GuiMain::GuiMain` skips a directory whose `toolbox.json` is
missing, is larger than 4096 bytes, does not parse, or lacks `tid` (a string),
`name` (a string) or `requires_reboot` (a bool). None of that is reported
anywhere: the module is not in the list, and nothing on the console says why.
So each of those is checked here.

  tests/toolbox_check.py <path> <expected tid> <expected version>

Used by `tests/test_package.sh layout` against the *packaged* file, so what is
checked is what a user unzips rather than the template it came from.
"""
import json
import sys

# ovl-sysmodules reads the file into a 4096-byte static buffer and skips
# anything longer, without reading it.
MAX_BYTES = 4096


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    path, tid, version = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(path, "rb") as handle:
        raw = handle.read()
    if not raw:
        return fail("toolbox.json is empty")
    if len(raw) > MAX_BYTES:
        return fail(f"toolbox.json is {len(raw)} bytes; ovl-sysmodules reads at most {MAX_BYTES}")

    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as error:
        return fail(f"toolbox.json does not parse ({error}); the module is skipped in silence")

    for key, kind, what in (("tid", str, "a string"),
                            ("name", str, "a string"),
                            ("requires_reboot", bool, "a bool")):
        if key not in doc:
            return fail(f"toolbox.json has no {key!r}; ovl-sysmodules skips the module")
        if not isinstance(doc[key], kind) or isinstance(doc[key], bool) != (kind is bool):
            return fail(f"toolbox.json's {key!r} is not {what}; ovl-sysmodules skips the module")

    if doc["tid"].upper() != tid:
        return fail(f"toolbox.json says tid {doc['tid']!r}, the archive installs under {tid}")
    # `strtoul(..., 16)`, which is what an unsubstituted placeholder becomes.
    if int(doc["tid"], 16) == 0:
        return fail("toolbox.json's tid reads as program id 0")
    if not doc["name"]:
        return fail("toolbox.json's name is empty; the list row would have no label")
    # The claim this project has to keep: sys-rommsync is killed outright with
    # `pmshellTerminateProgram` and relaunched with no gap, which is what the
    # `toggle.*` suite proves. `true` here would move it to the Static section,
    # where the switch does nothing until the console is rebooted.
    if doc["requires_reboot"] is not False:
        return fail("requires_reboot must be false: sys-rommsync is a dynamic module")
    # Optional to ovl-sysmodules -- it appends it to the row's label -- and
    # required here, because a version that does not move with VERSION is worse
    # than none: it names the wrong build on a support thread.
    if doc.get("version") != version:
        return fail(f"toolbox.json says version {doc.get('version')!r}, this build is {version}")

    print(f"ok: ovl-sysmodules would list {doc['name']!r} ({doc['tid']}) as a dynamic module")
    return 0


def fail(message: str) -> int:
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())

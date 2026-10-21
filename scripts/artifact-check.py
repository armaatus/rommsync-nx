#!/usr/bin/env python3
"""What the console's loaders read out of the two shipped artifacts (M9-12, #211).

Each check here is a failure that builds green, packages, installs, and is then
silent on a console:

    scripts/artifact-check.py lny2 FILE...
        MOD0 carries libnx's `LNY2` stamp, version >= 1 -- the mark of a build
        against the HOS 21.0.0 TLS ABI (libnx >= 4.10.0). Ultrahand reads it out
        of the `.ovl` on disk, and on Atmosphere >= 1.10.0 hides or refuses to
        launch an overlay without it, with nothing a user can act on.
        `scripts/toolchain.sh libnx` says the toolchain *should* stamp it; this
        says it did.

    scripts/artifact-check.py nacp OVL --title T --version V
        the overlay's NACP parses the way Ultrahand's `getOverlayInfo` parses it
        (the NRO's size, the `ASET` header after it, the NACP that names), and
        carries the menu's name and version. An entry whose NACP does not parse
        is skipped from the menu in silence.

    scripts/artifact-check.py version FILE... [--version V]
        the build marker `ROMMSYNC_NX_VERSION=<V>` is a string in FILE, the way
        `strings FILE | grep` would find it -- which catches `--gc-sections` or
        LTO eating it, and makes a user's uploaded file identifiable from a bug
        report. Without `--version` it prints what each file says it is.

    scripts/artifact-check.py --self-test
        the parsers against hand-built vectors; reads no artifact.

FILE is a `.nsp` (the sysmodule: its `main` NSO is read), an `.nso`, or an
NRO -- `.nro` or `.ovl`, which is an NRO with assets and `ULTR` appended.

**The `.nsp` needs unpacking for `strings` to mean anything.** elf2nso
LZ4-compresses all three NSO segments and has no switch to stop it (the header's
flags read 0x3f), so a string in the sysmodule's code is not in the `.nsp` as
plain bytes: part of it is, and the rest is a back-reference. `version` and
`lny2` therefore decompress the NSO first. The overlay is an NRO, which is
stored uncompressed, so for the `.ovl` this is literally `strings`.

**There is no NPDM in the overlay.** It runs inside nx-ovlloader's process and
inherits that process's capabilities, so any SVC or service question about
`ovl-rommsync` is a question about nx-ovlloader's NPDM, not one this script or
`scripts/npdm-check.py` can answer from our build.

No keys and no tools: NSO, NRO and NACP are plain structure.
"""
import argparse
import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# The PFS0 reader is npdm-check.py's, loaded rather than copied: one parser for
# one format, held by `npdm.parser`'s vectors.
_spec = importlib.util.spec_from_file_location("npdm_check", os.path.join(HERE, "npdm-check.py"))
npdm_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(npdm_check)
read_pfs0 = npdm_check.read_pfs0

# core/include/rommsync/version.hpp.in's `kBuildMarker`, up to the version.
MARKER_PREFIX = b"ROMMSYNC_NX_VERSION="

# libnx's crt0 (nx/source/runtime/switch_crt0.s): MOD0, then LNY0 at +28, LNY1
# at +40, LNY2 at +52 with its version word at +56. The offsets are the ones
# libtesla's detectLNY2FromBuffers reads -- the check Ultrahand applies.
LNY2_AT = 52
LNY2_VERSION_AT = 56
MOD0_EXTENT = 60

NACP_SIZE = 0x4000
NACP_DISPLAY_VERSION = 0x3060


class Failure(Exception):
    """The file is not the shape it claims, or a check ran and said no."""


# --- the containers ----------------------------------------------------------


def lz4_block(src, size):
    """One LZ4 block (no frame), as elf2nso writes each segment."""
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        length = token >> 4
        if length == 15:
            while True:
                extra = src[i]
                i += 1
                length += extra
                if extra != 255:
                    break
        out += src[i : i + length]
        i += length
        if i >= len(src):
            break
        offset = src[i] | src[i + 1] << 8
        i += 2
        if offset == 0 or offset > len(out):
            raise Failure("LZ4 match offset %d outside the %d bytes decoded" % (offset, len(out)))
        length = token & 15
        if length == 15:
            while True:
                extra = src[i]
                i += 1
                length += extra
                if extra != 255:
                    break
        # Byte by byte, because a match may overlap the bytes it is producing.
        for _ in range(length + 4):
            out.append(out[-offset])
    if len(out) != size:
        raise Failure("an LZ4 segment decoded to %d bytes, not the %d its header says" % (len(out), size))
    return bytes(out)


def nso_segments(nso):
    """text, rodata and data, decompressed where the header's flags say so."""
    if nso[:4] != b"NSO0":
        raise Failure("not an NSO (no NSO0 magic)")
    flags = struct.unpack_from("<I", nso, 0xC)[0]
    compressed = struct.unpack_from("<III", nso, 0x60)
    segments = []
    for index, header in enumerate((0x10, 0x20, 0x30)):
        file_offset, _, size = struct.unpack_from("<III", nso, header)
        if flags & (1 << index):
            blob = nso[file_offset : file_offset + compressed[index]]
            segments.append(lz4_block(blob, size))
        else:
            segments.append(nso[file_offset : file_offset + size])
    return segments


def load_image(path):
    """(kind, bytes): an NRO as stored, or the main NSO's segments decompressed."""
    blob = _read(path)
    if blob[:4] == b"PFS0":
        files = read_pfs0(blob)
        if "main" not in files:
            raise Failure("holds no `main` NSO (%s)" % ", ".join(sorted(files)))
        return "nso", nso_segments(files["main"])
    if blob[:4] == b"NSO0":
        return "nso", nso_segments(blob)
    if blob[0x10:0x14] == b"NRO0":
        return "nro", [blob]
    raise Failure("neither a PFS0, an NSO nor an NRO")


# --- the checks --------------------------------------------------------------


def lny2_version(kind, segments):
    """The LNY2 version word, found the way the loader finds MOD0."""
    # crt0's second word either way: `_start` branches over it, and
    # `.word __nx_mod0 - _start` is what it holds.
    blob = segments[0]
    mod0_rel, = struct.unpack_from("<I", blob, 0x4)
    if mod0_rel == 0:
        raise Failure("no MOD0 offset in the header; not a libnx image")
    where = mod0_rel
    if kind == "nro":
        # libtesla's usingLNY2: relative to the text segment's file offset.
        where += struct.unpack_from("<I", blob, 0x20)[0]
    if where + MOD0_EXTENT > len(blob):
        raise Failure("MOD0 offset 0x%x lies past the image" % where)
    if blob[where : where + 4] != b"MOD0":
        raise Failure("no MOD0 at 0x%x" % where)
    if blob[where + LNY2_AT : where + LNY2_AT + 4] != b"LNY2":
        raise Failure("MOD0 at 0x%x carries no LNY2 stamp: built with libnx < 4.10.0, "
                      "which Ultrahand hides on Atmosphere >= 1.10.0" % where)
    return struct.unpack_from("<I", blob, where + LNY2_VERSION_AT)[0]


def check_lny2(path):
    kind, segments = load_image(path)
    version = lny2_version(kind, segments)
    if version < 1:
        raise Failure("LNY2 version is %d; the HOS 21.0.0 TLS ABI is version 1" % version)
    return "LNY2 version %d" % version


def overlay_info(blob):
    """(name, version) out of an NRO's NACP, by getOverlayInfo's route."""
    if blob[0x10:0x14] != b"NRO0":
        raise Failure("not an NRO (no NRO0 magic at 0x10)")
    nro_size, = struct.unpack_from("<I", blob, 0x18)
    if blob[nro_size : nro_size + 4] != b"ASET":
        raise Failure("no ASET header at the NRO's end (0x%x)" % nro_size)
    nacp_offset, nacp_size = struct.unpack_from("<QQ", blob, nro_size + 0x18)
    if nacp_size != NACP_SIZE:
        raise Failure("the assets hold a %d-byte NACP, not %d" % (nacp_size, NACP_SIZE))
    nacp = blob[nro_size + nacp_offset : nro_size + nacp_offset + nacp_size]
    if len(nacp) != NACP_SIZE:
        raise Failure("the NACP runs past the end of the file")
    name = _cstring(nacp[:0x200])
    version = _cstring(nacp[NACP_DISPLAY_VERSION : NACP_DISPLAY_VERSION + 0x10])
    return name, version


def check_nacp(path, title, version):
    name, found = overlay_info(_read(path))
    if name != title:
        raise Failure("the NACP names %r, not %r" % (name, title))
    if found != version:
        raise Failure("the NACP says version %r, this build is %r" % (found, version))
    return "NACP %r %s" % (name, found)


def strings(blob):
    """What `strings` prints: printable ASCII runs of four or more."""
    return re.findall(rb"[\x20-\x7e]{4,}", blob)


def markers(path):
    kind, segments = load_image(path)
    found = []
    for segment in segments:
        # Anywhere in a run, as `strings | grep` would match it: the bytes
        # before a string are not its business.
        found += [s.split(MARKER_PREFIX, 1)[1].decode("ascii")
                  for s in strings(segment) if MARKER_PREFIX in s]
    return found


def check_version(path, version):
    found = markers(path)
    if version is None:
        if not found:
            raise Failure("no %s marker" % MARKER_PREFIX.decode())
        return ", ".join(found)
    if version not in found:
        raise Failure("no %s%s string (found: %s)" % (
            MARKER_PREFIX.decode(), version, ", ".join(found) or "none"))
    if len(set(found)) != 1:
        raise Failure("more than one version marker: %s" % ", ".join(found))
    return "%s%s" % (MARKER_PREFIX.decode(), version)


# --- vectors ------------------------------------------------------------------
# Hand-built from the formats, not from an artifact, so a parser drifting onto
# the wrong offset fails on every push, where nothing can build a module.


def _lz4_literals(data):
    """A valid LZ4 block of literals only: the decoder's simplest input."""
    length = len(data)
    if length < 15:
        return bytes([length << 4]) + data
    rest, out = length - 15, bytearray([0xF0])
    while rest >= 255:
        out.append(255)
        rest -= 255
    out.append(rest)
    return bytes(out) + data


def _mod0(lny2=True, version=1):
    words = b"MOD0" + b"\0" * 24 + b"LNY0" + b"\0" * 8 + b"LNY1" + b"\0" * 8
    tail = (b"LNY2" + struct.pack("<I", version)) if lny2 else b"\0" * 8
    return words + tail


def _text(lny2=True, version=1):
    # `b startup` then the MOD0 offset word, as crt0 lays them out.
    head = struct.pack("<II", 0x14000002, 0x40) + b"\0" * 0x38
    return head + _mod0(lny2, version) + b"\0" * 4


def _nso(text, rodata, compressed=(True, True, True)):
    segments = [text, rodata, b"data"]
    stored = [_lz4_literals(s) if c else s for s, c in zip(segments, compressed)]
    header = bytearray(0x100)
    header[0:4] = b"NSO0"
    struct.pack_into("<I", header, 0xC, sum(1 << i for i, c in enumerate(compressed) if c))
    offset = len(header)
    for index, base in enumerate((0x10, 0x20, 0x30)):
        struct.pack_into("<III", header, base, offset, 0, len(segments[index]))
        struct.pack_into("<I", header, 0x60 + 4 * index, len(stored[index]))
        offset += len(stored[index])
    return bytes(header) + b"".join(stored)


def _nro(text, title=b"ovl-fixture", version=b"9.9.9", ultr=True):
    blob = bytearray(text)
    blob[0x10:0x14] = b"NRO0"
    struct.pack_into("<I", blob, 0x18, len(text))
    struct.pack_into("<I", blob, 0x20, 0)
    nacp = bytearray(NACP_SIZE)
    nacp[: len(title)] = title
    nacp[0x3060 : 0x3060 + len(version)] = version
    aset = bytearray(0x38)
    aset[0:4] = b"ASET"
    struct.pack_into("<QQ", aset, 0x18, len(aset), len(nacp))
    return bytes(blob) + bytes(aset) + bytes(nacp) + (b"ULTR" if ultr else b"")


def self_test():
    import tempfile

    failures = []

    def expect(what, ok):
        if not ok:
            failures.append(what)

    def raises(what, call):
        try:
            call()
        except (Failure, struct.error, IndexError):
            return
        failures.append(what + " was accepted")

    # LZ4: literals, then an overlapping match ("abcd" + 8 more of it), then
    # literals again -- the case a slice copy instead of a byte copy gets wrong.
    block = bytes([0x44]) + b"abcd" + b"\x04\x00" + bytes([0x10]) + b"x"
    expect("an overlapping LZ4 match", lz4_block(block, 13) == b"abcdabcdabcdx")
    long = bytes(range(256)) * 2
    expect("an LZ4 literal run past 255 bytes", lz4_block(_lz4_literals(long), len(long)) == long)
    raises("an LZ4 match reaching before the output", lambda: lz4_block(b"\x14a\x05\x00", 5))
    raises("an LZ4 block shorter than its header says", lambda: lz4_block(_lz4_literals(b"abc"), 4))

    marker = MARKER_PREFIX + b"9.9.9\0"
    with tempfile.TemporaryDirectory() as tmp:
        def write(name, blob):
            path = os.path.join(tmp, name)
            with open(path, "wb") as handle:
                handle.write(blob)
            return path

        good_nsp = write("good.nsp", npdm_check._pfs0({
            "main.npdm": b"META", "main": _nso(_text(), b"\0junk" + marker, (True, False, True))}))
        # Text compressed and rodata not: each segment's own flag bit decides.
        expect("LNY2 in a compressed NSO inside a PFS0", check_lny2(good_nsp) == "LNY2 version 1")
        expect("the marker in a compressed NSO", check_version(good_nsp, "9.9.9"))
        raw_nso = write("raw.nso", _nso(_text(), marker, (False, False, False)))
        expect("LNY2 in an uncompressed NSO", check_lny2(raw_nso) == "LNY2 version 1")

        raises("an NSO whose MOD0 has no LNY2",
               lambda: check_lny2(write("old.nso", _nso(_text(lny2=False), marker))))
        raises("an LNY2 of version 0",
               lambda: check_lny2(write("v0.nso", _nso(_text(version=0), marker))))
        moved = bytearray(_text())
        struct.pack_into("<I", moved, 4, 0x44)
        raises("a MOD0 offset that misses MOD0",
               lambda: check_lny2(write("moved.nso", _nso(bytes(moved), marker))))
        raises("a PFS0 without `main`",
               lambda: check_lny2(write("nomain.nsp", npdm_check._pfs0({"main.npdm": b"META"}))))

        ovl = _nro(_text() + marker)
        good_ovl = write("good.ovl", ovl)
        expect("LNY2 in an NRO", check_lny2(good_ovl) == "LNY2 version 1")
        expect("the marker in an NRO", check_version(good_ovl, "9.9.9"))
        expect("the marker read back without --version", check_version(good_ovl, None) == "9.9.9")
        expect("the NACP read back", check_nacp(good_ovl, "ovl-fixture", "9.9.9"))
        raises("an NRO without LNY2", lambda: check_lny2(write("old.ovl", _nro(_text(lny2=False)))))
        raises("a NACP with another version",
               lambda: check_nacp(good_ovl, "ovl-fixture", "9.9.1"))
        raises("a NACP with another name", lambda: check_nacp(good_ovl, "ovl-other", "9.9.9"))
        no_aset = bytearray(ovl)
        no_aset[len(_text() + marker)] ^= 0xFF
        raises("an NRO without its ASET header",
               lambda: check_nacp(write("noaset.ovl", bytes(no_aset)), "ovl-fixture", "9.9.9"))
        raises("a truncated NACP",
               lambda: check_nacp(write("short.ovl", ovl[:-0x100]), "ovl-fixture", "9.9.9"))

        # `strings` semantics: a prefix of the version is not the version, and
        # a marker spelled for another build is not this one's.
        raises("the marker of 9.9.9 asked for 9.9",
               lambda: check_version(good_ovl, "9.9"))
        raises("an image with no marker",
               lambda: check_version(write("none.ovl", _nro(_text())), "9.9.9"))
        two = write("two.ovl", _nro(_text() + marker + MARKER_PREFIX + b"1.0.0\0"))
        raises("two different markers", lambda: check_version(two, "9.9.9"))

    for failure in failures:
        print("FAIL: self-test: %s" % failure, file=sys.stderr)
    if not failures:
        print("ok: LZ4, NSO, NRO, MOD0/LNY2, NACP and the marker read as the formats say")
    return 1 if failures else 0


# --- entry point -------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true",
                        help="check the parsers against hand-built vectors; reads no artifact")
    sub = parser.add_subparsers(dest="check")
    lny2 = sub.add_parser("lny2", help="MOD0 carries LNY2 >= 1")
    lny2.add_argument("files", nargs="+")
    nacp = sub.add_parser("nacp", help="the overlay's NACP parses, with this name and version")
    nacp.add_argument("files", nargs="+")
    nacp.add_argument("--title", required=True)
    nacp.add_argument("--version", required=True)
    version = sub.add_parser("version", help="the build marker is a string in the file")
    version.add_argument("files", nargs="+")
    version.add_argument("--version")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.check is None:
        parser.error("a check, or --self-test")

    status = 0
    for path in args.files:
        try:
            if args.check == "lny2":
                said = check_lny2(path)
            elif args.check == "nacp":
                said = check_nacp(path, args.title, args.version)
            else:
                said = check_version(path, args.version)
            print("ok: %s: %s" % (path, said))
        # `struct.error` and `IndexError` are a file truncated past an offset
        # this reads; `OSError` one that is not there. All are a FAIL line.
        except (Failure, npdm_check.Failure, struct.error, IndexError, OSError) as failure:
            print("FAIL: %s: %s" % (path, failure), file=sys.stderr)
            status = 1
    return status


def _cstring(blob):
    return blob.split(b"\0", 1)[0].decode("utf-8", "replace")


def _read(path):
    with open(path, "rb") as handle:
        return handle.read()


if __name__ == "__main__":
    sys.exit(main())

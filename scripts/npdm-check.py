#!/usr/bin/env python3
"""The capabilities the sysmodule's npdm grants, held against the ones its code uses (M9-3, #196).

An SVC a process was not granted is not a soft failure on Horizon. The kernel
refuses the call and the process dies, so the two the linked `sys-rommsync.elf`
issued without declaring -- `svcReturnFromException` (0x28) and
`svcUnmapTransferMemory` (0x52) -- were a crash nobody could have seen coming
from a green `ctest`: one of them is on libnx's exception path, which runs
exactly when something else has already gone wrong.

Nothing about that is decidable from the tree. `sysmodule/sys-rommsync.json` is
npdmtool's *input*; what the kernel reads is the NPDM npdmtool wrote into the
`.nsp`, and what the process *calls* is whatever `svc` instructions survived
`--gc-sections` in the linked ELF. So this reads both built artifacts and diffs
them.

    scripts/npdm-check.py --nsp sysmodule/sys-rommsync.nsp \
                          --elf sysmodule/sys-rommsync.elf \
                          --json sysmodule/sys-rommsync.json

`--print` dumps the declared SVC set and the SAC instead of checking anything,
with the filesystem mask spelled out bit by bit. `--json` alone checks the
config npdmtool will be handed, which needs no toolchain (`npdm.grants`).

**This is the sysmodule's npdm and only the sysmodule's.** `ovl-rommsync` has no
npdm of its own: `overlay/Makefile` builds an NRO plus a NACP and appends the
`ULTR` signature, and an overlay executes inside nx-ovlloader's process and
inherits *its* capabilities. An SVC check for the overlay is a check against
nx-ovlloader's NPDM, not against this one, and running this script at the
overlay would answer a question nobody asked.

No keys and no tools. The NPDM is not encrypted and not signed in any way that
matters here -- npdmtool zeroes the ACID signature, and Atmosphere does not check
it for a `/atmosphere/contents` sysmodule anyway (boot2 launches with
`StorageId::None`, and `ldr_meta.cpp` only validates when the storage id differs)
-- so the whole file is plain structure. `nstool -v` would print the same thing
in prose; parsing it here keeps the test to a Python file and the two artifacts
the build already produces.
"""
import argparse
import bisect
import io
import json
import os
import re
import struct
import sys

# --- what the npdm says ------------------------------------------------------


def read_pfs0(blob):
    """The files inside a PFS0 archive, by name.

    A `.nsp` is a PFS0 holding the exefs: `main` (the NSO) and `main.npdm`. The
    header is a fixed 0x10 bytes, a 0x18-byte entry per file, then the string
    table; file offsets are relative to the end of all three.
    """
    if blob[:4] != b"PFS0":
        raise Failure("not a PFS0 archive (no magic)")
    count, string_table_size = struct.unpack_from("<II", blob, 4)
    entries_end = 0x10 + count * 0x18
    strings = entries_end
    data = entries_end + string_table_size
    files = {}
    for i in range(count):
        offset, size, name_offset, _ = struct.unpack_from("<QQII", blob, 0x10 + i * 0x18)
        start = strings + name_offset
        name = blob[start : blob.index(b"\0", start)].decode("utf-8")
        files[name] = blob[data + offset : data + offset + size]
    return files


class Npdm:
    """A parsed NPDM: the META header, its ACI0, and its ACID.

    npdmtool writes the permissions into **both** ACI0 and ACID and zeroes the
    signature; `fs` ANDs the two, so a capability granted in one and not the
    other is not granted. For a sysmodule under `/atmosphere/contents` the ACI0
    side is the one that decides -- see the module docstring -- so the checks
    below read ACI0 for what the process may do and ACID only for the two flags
    that live nowhere else, plus the coherence check between them.
    """

    def __init__(self, blob):
        if blob[:4] != b"META":
            raise Failure("not an NPDM (no META magic)")
        self.blob = blob
        self.flags = blob[0xC]
        self.main_thread_priority = blob[0xE]
        self.default_cpu_id = blob[0xF]
        self.name = _cstring(blob[0x20:0x30])
        aci0_offset, aci0_size, acid_offset, acid_size = struct.unpack_from("<IIII", blob, 0x70)
        self.aci0 = Aci0(blob[aci0_offset : aci0_offset + aci0_size])
        self.acid = Acid(blob[acid_offset : acid_offset + acid_size])

    @property
    def is_64_bit(self):
        return bool(self.flags & 1)

    @property
    def address_space_type(self):
        return (self.flags >> 1) & 0x7


class Aci0:
    """ACI0: the program id, and the three permission blocks keyed off +0x20."""

    def __init__(self, blob):
        if blob[:4] != b"ACI0":
            raise Failure("ACI0 is not where the META header says it is")
        self.program_id = struct.unpack_from("<Q", blob, 0x10)[0]
        (fs_offset, fs_size, sac_offset, sac_size,
         kc_offset, kc_size) = struct.unpack_from("<IIIIII", blob, 0x20)
        self.filesystem_permissions = struct.unpack_from("<Q", blob, fs_offset + 4)[0]
        self.service_access = parse_sac(blob[sac_offset : sac_offset + sac_size])
        self.kernel_capabilities = blob[kc_offset : kc_offset + kc_size]
        self.syscalls = parse_syscalls(self.kernel_capabilities)
        self.handle_table_size = parse_handle_table_size(self.kernel_capabilities)


class Acid:
    """ACID: 0x100 of signature, 0x100 of modulus, then the header at +0x200.

    `is_retail` and `pool_partition` exist **only** here -- there is no ACI0
    field for either -- so the two acceptance checks that name them read this
    side. `is_retail: false` is not cosmetic: loader requires
    `AcidFlag_Production` and a module without it does not launch, silently.
    """

    HEADER = 0x200

    def __init__(self, blob):
        if blob[self.HEADER : self.HEADER + 4] != b"ACID":
            raise Failure("ACID is not where the META header says it is")
        self.signature = blob[:0x100]
        base = self.HEADER
        self.flags = struct.unpack_from("<I", blob, base + 0xC)[0]
        self.program_id_min, self.program_id_max = struct.unpack_from("<QQ", blob, base + 0x10)
        # The three offsets in this header are measured from the start of the
        # ACID -- the signature -- and not from the header they sit in. ACI0's
        # are measured from ACI0, which has no signature in front of it, so the
        # two look like the same convention until a 0x200 says otherwise.
        (fs_offset, fs_size, sac_offset, sac_size,
         kc_offset, kc_size) = struct.unpack_from("<IIIIII", blob, base + 0x20)
        # An FsAccessControl, not ACI0's FsAccessHeader: different structures of
        # different lengths that agree only on the permissions bitmask at +0x04,
        # which is the field `fs` ANDs and therefore the field worth comparing.
        self.filesystem_permissions = struct.unpack_from("<Q", blob, fs_offset + 4)[0]
        self.service_access = parse_sac(blob[sac_offset : sac_offset + sac_size])
        self.kernel_capabilities = blob[kc_offset : kc_offset + kc_size]
        self.syscalls = parse_syscalls(self.kernel_capabilities)
        self.handle_table_size = parse_handle_table_size(self.kernel_capabilities)

    @property
    def is_retail(self):
        return bool(self.flags & 1)

    @property
    def pool_partition(self):
        return (self.flags >> 2) & 0x3


def parse_sac(blob):
    """The service access control list: a length/flags byte, then that many name bytes.

    The low three bits are `len - 1` and the top bit says the entry is a service
    the process may *host* rather than one it may open. Both are returned, the
    host entries prefixed `host:`, because `service_host` and `service_access`
    are two different grants and a check that conflated them would pass on a
    module that may open `rommsync` but not register it.
    """
    names = []
    i = 0
    while i < len(blob):
        control = blob[i]
        length = (control & 0x7) + 1
        i += 1
        name = blob[i : i + length].decode("utf-8", "replace")
        names.append(("host:" if control & 0x80 else "") + name)
        i += length
    return names


def parse_syscalls(blob):
    """The SVC numbers an `EnableSystemCalls` capability descriptor grants.

    Kernel capability descriptors are u32s whose *type* is the count of set low
    bits: 3 for the thread-priority/core flags, 4 for this one, 14 for the
    minimum kernel version, and so on. `EnableSystemCalls` is therefore the low
    five bits reading `0b01111` -- four ones and the terminating zero -- with a
    24-bit mask above it and a 3-bit index above that. The index picks the block
    of 24, so the SVC number is `index * 24 + bit`.
    """
    granted = set()
    for i in range(0, len(blob) - 3, 4):
        word = struct.unpack_from("<I", blob, i)[0]
        if word & 0x1F != 0x0F:
            continue
        mask = (word >> 5) & 0xFFFFFF
        index = (word >> 29) & 0x7
        for bit in range(24):
            if mask >> bit & 1:
                granted.add(index * 24 + bit)
    return granted


def parse_handle_table_size(blob):
    """The `HandleTableSize` descriptor's value, or None if there is none.

    Fifteen set low bits identify it and the size is the ten above them. Since
    M9-11 (#210) the value is derived, in `HANDLE_BUDGET`, and `check_grants`
    holds the NPDM to it.
    """
    for i in range(0, len(blob) - 3, 4):
        word = struct.unpack_from("<I", blob, i)[0]
        if word & 0xFFFF == 0x7FFF:
            return (word >> 16) & 0x3FF
    return None


# --- what the ELF does -------------------------------------------------------

# `svc #imm16` on AArch64. The immediate is bits 5..20; everything else in the
# instruction is fixed, which is what makes a masked compare exact rather than a
# heuristic. `hvc`/`smc` differ in the low three bits and `brk`/`hlt` in bits
# 21..23, so none of them can be mistaken for an SVC here.
SVC_MASK = 0xFFE0001F
SVC_MATCH = 0xD4000001


def svc_immediate(word):
    """The SVC number `word` calls, or None if it is not an `svc` instruction."""
    if word & SVC_MASK != SVC_MATCH:
        return None
    return (word >> 5) & 0xFFFF


class Elf:
    """Just enough ELF64 to answer two questions about a linked image.

    Which SVCs does it issue, and what is the name of the function issuing each
    one. `nm` and `objdump` would answer both, but they live in the devkitPro
    container and this script has to be runnable beside the artifacts rather
    than only inside the image that built them.
    """

    SHF_ALLOC = 0x2
    SHF_EXECINSTR = 0x4
    SHT_PROGBITS = 1
    SHT_SYMTAB = 2
    STT_FUNC = 2

    def __init__(self, blob):
        if blob[:4] != b"\x7fELF" or blob[4] != 2 or blob[5] != 1:
            raise Failure("not a little-endian 64-bit ELF")
        self.blob = blob
        table_offset = struct.unpack_from("<Q", blob, 0x28)[0]
        entry_size, count, names_index = struct.unpack_from("<HHH", blob, 0x3A)
        self.sections = []
        for i in range(count):
            at = table_offset + i * entry_size
            (name, kind, flags, addr, offset, size,
             link, _info, _align, entries) = struct.unpack_from("<IIQQQQIIQQ", blob, at)
            self.sections.append(dict(name=name, kind=kind, flags=flags, addr=addr,
                                      offset=offset, size=size, link=link, entries=entries))
        self._names = self.sections[names_index] if count else None
        self._functions = self._read_functions()

    def section_name(self, section):
        return _cstring_at(self.blob, self._names["offset"] + section["name"])

    def _read_functions(self):
        """`(address, size, name)` for every STT_FUNC symbol, address-sorted.

        libnx's SVC wrappers are hand-written assembly and most carry a size of
        zero, so a symbol's extent is taken as "up to the next symbol" rather
        than from `st_size`. Without that every `svc` past the wrapper's first
        instruction -- `svcQueryMemory`'s is its second -- reports no owner, and
        the failure message names a number instead of a function.
        """
        functions = []
        for section in self.sections:
            if section["kind"] != self.SHT_SYMTAB or not section["entries"]:
                continue
            strings = self.sections[section["link"]]
            for at in range(0, section["size"], section["entries"]):
                name, info, _other, _shndx, value, size = struct.unpack_from(
                    "<IBBHQQ", self.blob, section["offset"] + at)
                if info & 0xF == self.STT_FUNC and value:
                    functions.append((value, size,
                                      _cstring_at(self.blob, strings["offset"] + name)))
        functions.sort()
        return functions

    def owner(self, address):
        index = bisect.bisect_right(self._functions, (address, 1 << 62, "")) - 1
        if index < 0:
            return None
        value, size, name = self._functions[index]
        end = value + size if size else (
            self._functions[index + 1][0] if index + 1 < len(self._functions) else value + 4)
        return name if value <= address < max(end, value + 4) else None

    def svc_calls(self):
        """`{svc number: {function name, ...}}` over every executable section.

        Only `SHF_ALLOC | SHF_EXECINSTR` PROGBITS sections are scanned, which
        keeps a debug section holding a copy of the code, and `.rodata`, out of
        the answer.

        It does **not** keep out AArch64 literal pools, which the assembler
        emits inside `.text`: a constant whose word happens to match
        `0xD4xxxxx1` is indistinguishable from an `svc` here and would be
        reported as a call this process makes. Every one of the 29 found today
        lands on a libnx `svc*` wrapper, so nothing is being papered over -- but
        a failure naming a function that plainly issues no SVC is this, and the
        answer is to look at the address, not to widen the grant.
        """
        calls = {}
        wanted = self.SHF_ALLOC | self.SHF_EXECINSTR
        for section in self.sections:
            if section["kind"] != self.SHT_PROGBITS or section["flags"] & wanted != wanted:
                continue
            body = self.blob[section["offset"] : section["offset"] + section["size"]]
            for at in range(0, len(body) - 3, 4):
                number = svc_immediate(struct.unpack_from("<I", body, at)[0])
                if number is None:
                    continue
                calls.setdefault(number, set()).add(
                    self.owner(section["addr"] + at) or
                    "%s+0x%x" % (self.section_name(section), at))
        return calls


# --- the checks --------------------------------------------------------------


class Failure(Exception):
    """A malformed input. Distinct from a check that ran and said no."""


def check_syscalls(npdm, elf, names):
    """Every SVC the image issues is one the npdm grants.

    The other direction is deliberately *not* a failure: a declared SVC nothing
    calls is a capability held for no reason, which is a narrowing to make and
    not a build breaker. M9-11 (#210) narrowed the filesystem mask, the handle
    table and the address space and left this set alone; it is printed so that
    whoever narrows it has a list to start from.
    """
    called = elf.svc_calls()
    granted = npdm.aci0.syscalls
    problems = []
    for number in sorted(called):
        if number in granted:
            continue
        problems.append(
            "0x%02x (%s) is issued by the linked ELF and not declared in the npdm. The "
            "kernel refuses an undeclared SVC and the process dies -- there is no soft "
            "failure and no crash report to read (#196)"
            % (number, ", ".join(sorted(called[number]))))
    unused = sorted(granted - set(called))
    if unused:
        print("note: %d declared SVCs nothing in the image calls: %s"
              % (len(unused), ", ".join(_named(n, names) for n in unused)))
    if problems:
        return problems
    print("ok: all %d SVCs the image issues are declared (%d declared in total)"
          % (len(called), len(granted)))
    return []


def check_flags(npdm):
    """`is_retail` and `pool_partition`, which live only in the ACID.

    Both are the kind of mistake that produces no error: without
    `AcidFlag_Production` loader refuses the module and nothing says so, and a
    pool partition other than 2 puts a system module's memory in the wrong pool.
    """
    problems = []
    if not npdm.acid.is_retail:
        problems.append(
            "the ACID does not carry AcidFlag_Production (`is_retail: true`); loader "
            "refuses the module and the only symptom is a sysmodule that is not running")
    if npdm.acid.pool_partition != 2:
        problems.append("pool_partition is %d, not 2 (system module)" % npdm.acid.pool_partition)
    if not npdm.is_64_bit:
        problems.append("the npdm does not declare a 64-bit process")
    if problems:
        return problems
    print("ok: is_retail, pool_partition 2, 64-bit")
    return []


def check_coherent(npdm):
    """ACI0 against ACID, which is the `[WARNING]` an `nstool -v` would print.

    `fs` ANDs the two, so anything ACI0 asks for and ACID does not grant is a
    permission the process does not have -- and it is granted-looking in the
    file that a person reads. npdmtool writes both from one `permissions` block,
    so a mismatch means the input said two different things.
    """
    problems = []
    missing = sorted(npdm.aci0.syscalls - npdm.acid.syscalls)
    if missing:
        problems.append("ACI0 grants SVCs the ACID does not: %s; fs ANDs the two, so the "
                        "process does not have them" % ", ".join("0x%02x" % n for n in missing))
    extra = sorted(set(npdm.aci0.service_access) - set(npdm.acid.service_access))
    if extra:
        problems.append("ACI0's SAC holds entries the ACID's does not: %s" % ", ".join(extra))
    if npdm.aci0.filesystem_permissions & ~npdm.acid.filesystem_permissions:
        problems.append("ACI0 asks for filesystem permissions 0x%016X and the ACID grants "
                        "0x%016X" % (npdm.aci0.filesystem_permissions,
                                     npdm.acid.filesystem_permissions))
    if not (npdm.acid.program_id_min <= npdm.aci0.program_id <= npdm.acid.program_id_max):
        problems.append("the ACI0 program id 0x%016X is outside the ACID range "
                        "0x%016X..0x%016X" % (npdm.aci0.program_id, npdm.acid.program_id_min,
                                              npdm.acid.program_id_max))
    if problems:
        return problems
    print("ok: ACI0 and ACID agree on syscalls, services, filesystem access and program id")
    return []


def check_program_id(npdm, source):
    """The program id in the artifact is the one the config asked for.

    `scripts/package.sh` reads `program_id` out of the same config to name
    `atmosphere/contents/<id>/`, and Atmosphere launches a sysmodule by that
    directory. So a config npdmtool read differently from the way package.sh
    read it puts the module in a directory nothing launches it from, with no
    error anywhere -- and M9-3 renamed exactly that key. `check_coherent`
    cannot see this: npdmtool writes ACI0's id and the ACID's range from one
    input, so those two agree by construction.
    """
    if source is None:
        return []
    declared = json.loads(source).get("program_id")
    if declared is None:
        return ["sys-rommsync.json has no `program_id`; npdmtool falls back to the "
                "deprecated `title_id`, and scripts/package.sh reads neither"]
    if int(str(declared), 0) != npdm.aci0.program_id:
        return ["the NPDM carries program id 0x%016X and sys-rommsync.json asks for %s; "
                "scripts/package.sh names atmosphere/contents/<id>/ from the config, so "
                "the module would install where nothing launches it"
                % (npdm.aci0.program_id, declared)]
    print("ok: the NPDM's program id is the one sys-rommsync.json asks for")
    return []


PLACEHOLDER = re.compile(r"@[A-Z][A-Z0-9_]*@")


def check_placeholders(npdm, source):
    """No `@PLACEHOLDER@` reached the artifact.

    Nothing templates `sys-rommsync.json` today. `scripts/package.sh` templates
    the files beside it and `tests/test_package.sh` asks this same question of
    those, so the day the npdm joins them is the day a substituted-away program
    id would otherwise ship as the literal string.
    """
    problems = []
    if PLACEHOLDER.search(npdm.name):
        problems.append("the npdm name is %r, an unsubstituted @PLACEHOLDER@" % npdm.name)
    for entry in npdm.aci0.service_access:
        if PLACEHOLDER.search(entry):
            problems.append("the SAC holds an unsubstituted @PLACEHOLDER@: %s" % entry)
    if source is not None:
        found = PLACEHOLDER.search(source)
        if found:
            problems.append("sys-rommsync.json holds an unsubstituted %s" % found.group(0))
    if problems:
        return problems
    print("ok: no @PLACEHOLDER@ survived into the npdm")
    return []


# --- the three grants M9-11 (#210) decided -----------------------------------
#
# Each of these was a devkitPro template value until #210, and a template is
# exactly where the next copy-paste comes from. So each is held to the value
# the sysmodule's README derives, in the built NPDM (`npdm.svcs`) and in the
# config that produces it (`npdm.grants`, which needs no toolchain and so runs
# on every push). Exact rather than "at most": a narrower value is a decision
# too, and it deserves the same edit to the README the wider one does.

# fs's names for the bits of the permission mask, as Atmosphere's fssrv spells
# them (`fssrv_access_control_bits.hpp`, accurate to 13.4.0). The mask is what
# `fs` checks a mount against: `CanMountSdCardRead/Write` is `Debug | SdCard`,
# `CanFormatSdCard` is `FormatSdCard`, and `FullPermission` satisfies every
# check there is -- so the template's 0xFFFF... grants everything twice over.
FS_PERMISSION_NAMES = {
    0: "ApplicationInfo", 1: "BootModeControl", 2: "Calibration", 3: "SystemSaveData",
    4: "GameCard", 5: "SaveDataBackUp", 6: "SaveDataManagement", 7: "BisAllRaw",
    8: "GameCardRaw", 9: "GameCardPrivate", 10: "SetTime", 11: "ContentManager",
    12: "ImageManager", 13: "CreateSaveData", 14: "SystemSaveDataManagement",
    15: "BisFileSystem", 16: "SystemUpdate", 17: "SaveDataMeta", 18: "DeviceSaveData",
    19: "SettingsControl", 20: "SystemData", 21: "SdCard", 22: "Host", 23: "FillBis",
    24: "CorruptSaveData", 25: "SaveDataForDebug", 26: "FormatSdCard", 27: "GetRightsId",
    28: "RegisterExternalKey", 29: "RegisterUpdatePartition", 30: "SaveDataTransfer",
    31: "DeviceDetection", 32: "AccessFailureResolution", 33: "SaveDataTransferVersion2",
    34: "RegisterProgramIndexMapInfo", 35: "CreateOwnSaveData", 36: "MoveCacheStorage",
    62: "Debug", 63: "FullPermission",
}

# `sdmc:` through libnx's fsdev, which is `OpenSdCardFileSystem` and nothing
# else -- the whole of what this process asks fs for. sysmodule/README.md has
# the bit to add, and why, for each facility that would need another.
FS_SD_CARD = 1 << 21
FS_PERMISSIONS = FS_SD_CARD

# 39-bit (`AddressSpaceType_64Bit`, 2.0.0+). 1 is the 36-bit layout 1.0.0
# shipped, kept for compatibility; sysmodule/README.md says why 3.
ADDRESS_SPACE_TYPE = 3
ADDRESS_SPACE_NAMES = {0: "32-bit", 1: "36-bit (deprecated)", 2: "32-bit, no alias region",
                       3: "39-bit"}

# The handle table's size lives where its count does, beside the terms it is
# derived from and the `static_assert` that holds the two together.
HANDLE_BUDGET = "sysmodule/source/handle_budget.hpp"
HANDLE_TABLE_SIZE = re.compile(r"\bkHandleTableSize\s*=\s*(\d+)\s*;")


class Grants:
    """The three values #210 narrowed, read from one place or another."""

    def __init__(self, fs_permissions, handle_table_size, address_space_type):
        self.fs_permissions = fs_permissions
        self.handle_table_size = handle_table_size
        self.address_space_type = address_space_type


def fs_permission_names(mask):
    """`SdCard (21), FormatSdCard (26)` for a mask -- what `nstool -v` would list."""
    bits = [bit for bit in range(64) if mask >> bit & 1]
    named = ["%s (%d)" % (FS_PERMISSION_NAMES[bit], bit) for bit in bits
             if bit in FS_PERMISSION_NAMES]
    unnamed = [bit for bit in bits if bit not in FS_PERMISSION_NAMES]
    if unnamed:
        named.append("%d unnamed bit%s" % (len(unnamed), "" if len(unnamed) == 1 else "s"))
    return ", ".join(named) or "nothing"


def read_handle_table_size(text):
    found = HANDLE_TABLE_SIZE.search(text)
    if found is None:
        raise Failure("%s declares no `kHandleTableSize = N;`" % HANDLE_BUDGET)
    return int(found.group(1))


def expected_grants(repo):
    with open("%s/%s" % (repo, HANDLE_BUDGET), encoding="utf-8") as header:
        return Grants(FS_PERMISSIONS, read_handle_table_size(header.read()), ADDRESS_SPACE_TYPE)


def _grant_problems(side, observed, expected):
    """Each way `observed` -- one side's three values -- differs from `expected`."""
    problems = []
    wider = observed.fs_permissions & ~expected.fs_permissions
    if wider:
        problems.append(
            "%s grants filesystem rights the sysmodule does not use: %s. A bit the "
            "process was never granted is one a bug cannot reach; add one only with its "
            "row in sysmodule/README.md (#210)" % (side, fs_permission_names(wider)))
    narrower = expected.fs_permissions & ~observed.fs_permissions
    if narrower:
        problems.append(
            "%s does not grant %s, which `sdmc:` needs: every read and write of the "
            "card would be refused" % (side, fs_permission_names(narrower)))
    if observed.handle_table_size != expected.handle_table_size:
        problems.append(
            "%s sizes the handle table at %s and %s derives %d; change the count there, "
            "not the number here (#210)" % (side, observed.handle_table_size, HANDLE_BUDGET,
                                            expected.handle_table_size))
    if observed.address_space_type != expected.address_space_type:
        problems.append(
            "%s asks for address space type %s (%s), not %d (%s); sysmodule/README.md "
            "says why (#210)" % (side, observed.address_space_type,
                                 ADDRESS_SPACE_NAMES.get(observed.address_space_type, "unknown"),
                                 expected.address_space_type,
                                 ADDRESS_SPACE_NAMES[expected.address_space_type]))
    return problems


def _granted(what, problems, expected):
    if not problems:
        print("ok: %s filesystem %s, handle table %d, address space %s"
              % (what, fs_permission_names(expected.fs_permissions), expected.handle_table_size,
                 ADDRESS_SPACE_NAMES[expected.address_space_type]))
    return problems


def check_grants(npdm, expected):
    """The built NPDM grants exactly what #210 decided, on both sides.

    Both, because `check_coherent` only asks that ACI0 not exceed the ACID: an
    ACID that still carried the template's mask, or a wider handle table, would
    pass it, and would be a widened declaration waiting for an ACI0 edit to make
    it real. The address space lives in the META header, not in either side, so
    it is asked once.
    """
    problems = _grant_problems(
        "ACI0", Grants(npdm.aci0.filesystem_permissions, npdm.aci0.handle_table_size,
                       npdm.address_space_type), expected)
    problems += _grant_problems(
        "the ACID", Grants(npdm.acid.filesystem_permissions, npdm.acid.handle_table_size,
                           expected.address_space_type), expected)
    return _granted("the NPDM grants", problems, expected)


def check_config_grants(source, expected):
    """The same three, in the npdmtool input -- so a push finds them without a toolchain."""
    config = json.loads(source)
    try:
        fs_permissions = int(str(config["filesystem_access"]["permissions"]), 0)
    except (KeyError, TypeError, ValueError):
        return ["sys-rommsync.json has no readable `filesystem_access.permissions`"]
    handle_table_size = None
    for block in config.get("kernel_capabilities", []):
        if block.get("type") == "handle_table_size":
            handle_table_size = int(str(block.get("value")), 0)
    observed = Grants(fs_permissions, handle_table_size, config.get("address_space_type"))
    return _granted("sys-rommsync.json grants",
                    _grant_problems("sys-rommsync.json", observed, expected), expected)


def _named(number, names):
    return "%s (0x%02x)" % (names[number], number) if number in names else "0x%02x" % number


def _cstring(blob):
    end = blob.find(b"\0")
    return blob[: end if end >= 0 else len(blob)].decode("utf-8", "replace")


def _cstring_at(blob, at):
    return _cstring(blob[at : at + 256])


def syscall_names(source):
    """`{number: name}` from the npdmtool input, for readable output.

    The input is where the human-facing names are; the npdm itself holds only a
    bit in a mask. Names for *undeclared* SVCs come from the ELF's own symbols
    instead, so this is presentation and never the check.
    """
    names = {}
    for block in json.loads(source).get("kernel_capabilities", []):
        if block.get("type") == "syscalls":
            for name, number in block.get("value", {}).items():
                names[int(str(number), 0)] = name
    return names


# --- self-test ---------------------------------------------------------------
#
# The check above needs a cross-compiled `.nsp`, so it skips on every runner
# this project has today (M9-12, #211) -- which would leave the parser itself
# with no coverage anywhere, in a script whose entire job is reading byte
# offsets correctly. These vectors are hand-built from the format, not from the
# artifact, so a parser that drifted onto the wrong offset fails here on every
# push even where nothing can build a module.


def _pfs0(files):
    names = b"".join(name.encode() + b"\0" for name in files)
    header = struct.pack("<4sIII", b"PFS0", len(files), len(names), 0)
    entries, data, offset = b"", b"", 0
    name_offset = 0
    for name, body in files.items():
        entries += struct.pack("<QQII", offset, len(body), name_offset, 0)
        data += body
        offset += len(body)
        name_offset += len(name) + 1
    return header + entries + names + data


def _syscall_descriptors(numbers):
    words = b""
    for index in range(8):
        mask = 0
        for number in numbers:
            if number // 24 == index:
                mask |= 1 << (number % 24)
        if mask:
            words += struct.pack("<I", 0x0F | (mask << 5) | (index << 29))
    return words


# The fixture's constants are chosen so that a parser reading the *wrong* offset
# gets a wrong answer rather than a lucky one. A program id that is not zero and
# an ACID range pinned to exactly it; different filesystem masks either side, so
# the subset test cannot pass by comparing a field to itself; a handle table
# size past 512, so the value's mask has to be the full ten bits; and an SVC
# immediate past 0xFF in the ELF, so the instruction decoder has to read all
# sixteen. Each of those was a mutation that kept `npdm.parser` green before.
FIXTURE_PROGRAM_ID = 0x4200DEADBEEF0042
FIXTURE_HANDLE_TABLE = 0x2AB      # 683: sets bits the 10-bit field only just holds
FIXTURE_ACID_FS = 0x800000000000001F
FIXTURE_ACI0_FS = 0x0000000000000003


def _handle_table_descriptor(size):
    return struct.pack("<I", 0x7FFF | (size << 16))


def _npdm(syscalls, acid_flags=0x9, name=b"fixture", program_id=FIXTURE_PROGRAM_ID,
          aci0_fs=FIXTURE_ACI0_FS, acid_fs=FIXTURE_ACID_FS, handle_table=FIXTURE_HANDLE_TABLE,
          meta_flags=0x3, acid_handle_table=None):
    kc = _syscall_descriptors(syscalls) + _handle_table_descriptor(handle_table)
    # npdmtool writes one block into both sides; a different ACID one is how a
    # reader that took ACI0's value for both would be caught.
    acid_kc = _syscall_descriptors(syscalls) + _handle_table_descriptor(
        handle_table if acid_handle_table is None else acid_handle_table)
    aci0_fs_block = struct.pack("<IQ", 1, aci0_fs)
    acid_fs_block = struct.pack("<IQ", 1, acid_fs)
    sac = b"\x06fsp-srv"
    aci0 = struct.pack("<4s12xQ8x", b"ACI0", program_id)
    body_at = 0x40
    fs = aci0_fs_block
    aci0 += struct.pack("<IIIIII", body_at, len(fs), body_at + len(fs), len(sac),
                        body_at + len(fs) + len(sac), len(kc)) + b"\0" * 8
    aci0 = aci0.ljust(body_at, b"\0") + fs + sac + kc
    # The range is pinned to the id rather than left wide open: a program id read
    # from the wrong offset comes back as zero, and only a pinned range says so.
    acid_body = struct.pack("<4sIIIQQ", b"ACID", 0, 0, acid_flags,
                            program_id, program_id)
    # Offsets from the start of the ACID, signature included -- the 0x200 the
    # real header measures from and the fixture would hide if it did not.
    fs = acid_fs_block
    signed_at = 0x200 + body_at
    acid_body += struct.pack("<IIIIII", signed_at, len(fs), signed_at + len(fs), len(sac),
                             signed_at + len(fs) + len(sac), len(acid_kc))
    acid_body = acid_body.ljust(body_at, b"\0") + fs + sac + acid_kc
    acid = b"\0" * 0x200 + acid_body
    meta = bytearray(0x80)
    meta[0:4] = b"META"
    meta[0xC] = meta_flags
    meta[0x20 : 0x20 + len(name)] = name
    aci0_at = 0x80
    acid_at = aci0_at + len(aci0)
    struct.pack_into("<IIII", meta, 0x70, aci0_at, len(aci0), acid_at, len(acid))
    return bytes(meta) + aci0 + acid


def _elf(svcs):
    """An ELF64 with one executable section holding `svc #n` for each n, and a symtab."""
    text = b"".join(struct.pack("<I", SVC_MATCH | (number << 5)) for number in svcs)
    names = b"\0.text\0.shstrtab\0.symtab\0.strtab\0"
    symbol_names = b"\0" + b"".join(("svc_%d" % n).encode() + b"\0" for n in svcs)
    base = 0x1000
    symbols = b"\0" * 24
    at = 1
    for i, number in enumerate(svcs):
        symbols += struct.pack("<IBBHQQ", at, Elf.STT_FUNC, 0, 1, base + i * 4, 4)
        at += len("svc_%d" % number) + 1
    header = bytearray(0x40)
    header[0:6] = b"\x7fELF\x02\x01"
    body = 0x40
    offsets = {}
    blobs = [("text", text), ("shstrtab", names), ("symtab", symbols), ("strtab", symbol_names)]
    payload = b""
    for key, blob in blobs:
        offsets[key] = body + len(payload)
        payload += blob
    table = body + len(payload)
    struct.pack_into("<Q", header, 0x28, table)
    struct.pack_into("<HHH", header, 0x3A, 64, 5, 2)
    sections = struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    sections += struct.pack("<IIQQQQIIQQ", 1, Elf.SHT_PROGBITS,
                            Elf.SHF_ALLOC | Elf.SHF_EXECINSTR, base,
                            offsets["text"], len(text), 0, 0, 4, 0)
    sections += struct.pack("<IIQQQQIIQQ", 7, 3, 0, 0, offsets["shstrtab"], len(names),
                            0, 0, 1, 0)
    sections += struct.pack("<IIQQQQIIQQ", 17, Elf.SHT_SYMTAB, 0, 0, offsets["symtab"],
                            len(symbols), 4, 0, 8, 24)
    sections += struct.pack("<IIQQQQIIQQ", 25, 3, 0, 0, offsets["strtab"],
                            len(symbol_names), 0, 0, 1, 0)
    return bytes(header) + payload + sections


def self_test():
    failures = []

    def expect(condition, what):
        if not condition:
            failures.append(what)

    def quietly(check, *arguments):
        """Run a check for its verdict without its narration.

        The checks print what they found, which is right when they are reading
        the real module and wrong here: an `ok: is_retail, pool_partition 2` in
        `npdm.parser`'s output would read as a statement about the sysmodule,
        and it is a statement about a fixture built four lines above.
        """
        saved, sys.stdout = sys.stdout, io.StringIO()
        try:
            return check(*arguments)
        finally:
            sys.stdout = saved

    # The instruction decoder, against the words devkitA64 actually emitted for
    # the two SVCs #196 found, and against three instructions that share the
    # `svc`'s top byte and must not be mistaken for one.
    expect(svc_immediate(0xD4000501) == 0x28, "svc #0x28 (0xd4000501) did not decode")
    expect(svc_immediate(0xD4000A41) == 0x52, "svc #0x52 (0xd4000a41) did not decode")
    expect(svc_immediate(0xD4000502) is None, "hvc decoded as an svc")
    expect(svc_immediate(0xD4000503) is None, "smc decoded as an svc")
    expect(svc_immediate(0xD4200000) is None, "brk decoded as an svc")

    # The descriptor decoder, on a block boundary: 23 is the top bit of index 0
    # and 24 is the bottom bit of index 1, which is exactly where an off-by-one
    # in the shift or the mask width shows up.
    expect(parse_syscalls(_syscall_descriptors({0, 23, 24, 0x52})) == {0, 23, 24, 0x52},
           "the syscall mask did not round-trip across a 24-bit block boundary")
    expect(parse_syscalls(struct.pack("<I", 0x3FFF)) == set(),
           "a min-kernel-version descriptor was read as a syscall mask")
    # 0x407FFF is the descriptor npdmtool writes for `handle_table_size: 64`:
    # fifteen low ones, then the size. The min-kernel-version descriptor beside
    # it has fourteen, so a mask one bit wide either way reads the wrong one.
    expect(parse_handle_table_size(struct.pack("<II", 0x183FFF, 0x407FFF)) == 64,
           "the handle table size was not read from its descriptor")
    expect(parse_handle_table_size(_handle_table_descriptor(1023)) == 1023,
           "the handle table size lost its top bits")

    # ...and the whole path, on a synthetic module that calls one SVC it never
    # declared. This is #196 in miniature, and it is what says the diff still
    # reports rather than passing over.
    nsp = _pfs0({"main.npdm": _npdm({0x01, 0x21}), "main": b"stub"})
    npdm = Npdm(read_pfs0(nsp)["main.npdm"])
    # 0x1234 is not a real SVC and is not meant to be: it is the immediate that
    # says the decoder reads sixteen bits rather than the eight every genuine
    # number fits in.
    elf = Elf(_elf([0x01, 0x21, 0x52, 0x1234]))
    expect(npdm.aci0.syscalls == {0x01, 0x21}, "the fixture's declared set did not survive")
    expect(npdm.aci0.program_id == FIXTURE_PROGRAM_ID, "the ACI0 program id was misread")
    expect(npdm.aci0.handle_table_size == FIXTURE_HANDLE_TABLE,
           "the fixture's handle table size was misread")
    expect(npdm.aci0.filesystem_permissions == FIXTURE_ACI0_FS,
           "the ACI0 filesystem permissions were misread")
    expect(npdm.acid.filesystem_permissions == FIXTURE_ACID_FS,
           "the ACID filesystem permissions were misread")
    expect(set(elf.svc_calls()) == {0x01, 0x21, 0x52, 0x1234},
           "the fixture's called set did not survive")
    expect(elf.owner(0x1008) == "svc_82", "the symbol owning an svc was not found")
    expect(len(quietly(check_syscalls, npdm, elf, {})) == 2,
           "an undeclared SVC was not reported")
    expect(quietly(check_coherent, npdm) == [], "the coherent fixture was reported incoherent")
    expect(quietly(check_flags, npdm) == [], "the fixture's ACID flags were misread")
    expect(quietly(check_program_id, npdm,
                   '{"program_id": "0x%016X"}' % FIXTURE_PROGRAM_ID) == [],
           "the matching program id was reported as a mismatch")
    expect(len(quietly(check_program_id, npdm, '{"program_id": "0x1"}')) == 1,
           "a program id the config did not ask for passed")
    # ACI0 asking for a filesystem right the ACID does not grant: `fs` ANDs the
    # two, so this is a permission the module looks granted and does not have.
    expect(len(quietly(check_coherent,
                       Npdm(_npdm({0x01}, aci0_fs=0xF, acid_fs=0x1)))) == 1,
           "an ACI0 filesystem mask wider than the ACID's passed")

    # The negative half of each of the other two checks.
    expect(len(quietly(check_flags, Npdm(_npdm({0x01}, acid_flags=0x8)))) == 1,
           "a module without AcidFlag_Production passed")
    expect(len(quietly(check_flags, Npdm(_npdm({0x01}, acid_flags=0x1)))) == 1,
           "pool_partition 0 passed")
    expect(len(quietly(check_placeholders, Npdm(_npdm({0x01}, name=b"@NAME@")), None)) == 1,
           "an unsubstituted placeholder in the npdm name passed")

    # The grants #210 decided. The expected table size is deliberately not the
    # tree's: these vectors test the comparison, and the tree's value is
    # `npdm.grants`'s to hold.
    expected = Grants(FS_SD_CARD, 48, 3)

    def grants_npdm(aci0_fs=FS_SD_CARD, acid_fs=FS_SD_CARD, handle_table=48, meta_flags=0x7,
                    acid_handle_table=None):
        return Npdm(_npdm({0x01}, aci0_fs=aci0_fs, acid_fs=acid_fs, handle_table=handle_table,
                          meta_flags=meta_flags, acid_handle_table=acid_handle_table))

    template = 0xFFFFFFFFFFFFFFFF
    expect(grants_npdm().address_space_type == 3, "address space type 3 was misread")
    expect(quietly(check_grants, grants_npdm(), expected) == [],
           "an NPDM granting exactly what was decided was reported")
    wide = quietly(check_grants, grants_npdm(aci0_fs=template, acid_fs=template), expected)
    expect(len(wide) == 2 and all("FormatSdCard (26)" in p and "CorruptSaveData (24)" in p
                                  for p in wide),
           "the template's mask passed, or was reported without naming what it grants")
    # The case `check_coherent` cannot see: ACI0 narrowed, ACID still the template.
    expect(len(quietly(check_grants, grants_npdm(acid_fs=template), expected)) == 1,
           "an ACID still carrying the template's mask passed")
    expect(len(quietly(check_grants, grants_npdm(aci0_fs=0, acid_fs=0), expected)) == 2,
           "a mask without SdCard passed")
    # The fixture writes one capability block into both sides, as npdmtool does,
    # so a wrong table is two findings -- and one would mean a side went unread.
    expect(len(quietly(check_grants, grants_npdm(handle_table=1023), expected)) == 2,
           "a handle table widened to 1023 passed, on ACI0 or on the ACID")
    expect(len(quietly(check_grants, grants_npdm(handle_table=32), expected)) == 2,
           "a handle table below the derived size passed, on ACI0 or on the ACID")
    acid_wider = grants_npdm(acid_handle_table=FIXTURE_HANDLE_TABLE)
    expect(acid_wider.acid.handle_table_size == FIXTURE_HANDLE_TABLE,
           "the ACID's handle table size was misread")
    expect(len(quietly(check_grants, acid_wider, expected)) == 1,
           "an ACID handle table wider than ACI0's passed")
    expect(len(quietly(check_grants, grants_npdm(meta_flags=0x3), expected)) == 1,
           "address space type 1 passed")

    # ...and the config, which is what a push checks without a toolchain.
    def npdmtool_input(permissions, handles, space):
        return json.dumps({"address_space_type": space,
                           "filesystem_access": {"permissions": permissions},
                           "kernel_capabilities": [{"type": "handle_table_size",
                                                    "value": handles}]})

    expect(quietly(check_config_grants, npdmtool_input("0x0000000000200000", 48, 3), expected) == [],
           "a config granting exactly what was decided was reported")
    expect(len(quietly(check_config_grants, npdmtool_input("0xffffffffffffffff", 64, 1), expected)) == 3,
           "the template's three values did not each fail")
    expect(read_handle_table_size("inline constexpr std::size_t kHandleTableSize = 96;") == 96,
           "kHandleTableSize was not read out of its header")
    try:
        read_handle_table_size("kHandleTableSize is mentioned, not declared")
        failures.append("a header with no kHandleTableSize declaration was accepted")
    except Failure:
        pass

    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("ok: the npdm and ELF readers agree with hand-built vectors")
    return 0


# --- entry point -------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nsp", help="the built .nsp; its main.npdm is the one read")
    parser.add_argument("--npdm", help="a bare .npdm, instead of --nsp")
    parser.add_argument("--elf", help="the linked ELF whose svc instructions are the called set")
    parser.add_argument("--json", help="npdmtool's input, read only for SVC names and placeholders")
    parser.add_argument("--print", dest="dump", action="store_true",
                        help="print the declared SVCs and the SAC instead of checking")
    parser.add_argument("--self-test", action="store_true",
                        help="check the parsers against hand-built vectors; reads no artifact")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.nsp and not args.npdm and not args.json:
        parser.error("one of --nsp, --npdm or --json is required")

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if not args.nsp and not args.npdm:
        # The config alone: what npdmtool will be asked for, checked without
        # npdmtool (`npdm.grants`).
        try:
            problems = check_config_grants(_read(args.json).decode("utf-8"),
                                           expected_grants(repo))
        except (Failure, OSError, ValueError) as failure:
            return _malformed(failure)
        for problem in problems:
            print("FAIL: " + problem, file=sys.stderr)
        return 1 if problems else 0

    try:
        if args.nsp:
            files = read_pfs0(_read(args.nsp))
            if "main.npdm" not in files:
                raise Failure("%s holds no main.npdm (%s)" % (args.nsp, ", ".join(sorted(files))))
            npdm = Npdm(files["main.npdm"])
        else:
            npdm = Npdm(_read(args.npdm))
        source = _read(args.json).decode("utf-8") if args.json else None
        names = syscall_names(source) if source else {}
        elf = Elf(_read(args.elf)) if args.elf else None
    # `Failure` is a file that is the wrong shape; `struct.error` and
    # `IndexError` are one truncated past an offset this reads; `OSError` is one
    # that is not there. All three are the same answer to the caller, and a
    # traceback would be the one error path in this script that does not say
    # FAIL -- which matters most for `--print`, the one a person reads.
    except (Failure, struct.error, IndexError, OSError, ValueError) as failure:
        return _malformed(failure)

    if args.dump:
        print("name: %s" % npdm.name)
        print("program_id: 0x%016X" % npdm.aci0.program_id)
        print("service_access: %s" % " ".join(npdm.aci0.service_access))
        # The three fields M9-11 (#210) decided, the mask named bit by bit: a
        # hex number is not what a reviewer can check a grant against.
        print("filesystem_permissions: 0x%016X (%s)" % (
            npdm.aci0.filesystem_permissions,
            fs_permission_names(npdm.aci0.filesystem_permissions)))
        print("handle_table_size: %s" % npdm.aci0.handle_table_size)
        print("address_space_type: %d (%s)" % (
            npdm.address_space_type, ADDRESS_SPACE_NAMES.get(npdm.address_space_type, "?")))
        print("syscalls: %s" % " ".join(_named(n, names) for n in sorted(npdm.aci0.syscalls)))
        return 0

    try:
        expected = expected_grants(repo)
    except (Failure, OSError) as failure:
        return _malformed(failure)
    problems = []
    problems += check_flags(npdm)
    problems += check_coherent(npdm)
    problems += check_program_id(npdm, source)
    problems += check_placeholders(npdm, source)
    problems += check_grants(npdm, expected)
    if elf is not None:
        problems += check_syscalls(npdm, elf, names)
    else:
        print("note: no --elf, so nothing checked what the image actually calls")
    for problem in problems:
        print("FAIL: " + problem, file=sys.stderr)
    return 1 if problems else 0


def _malformed(failure):
    print("FAIL: %s: %s" % (type(failure).__name__, failure), file=sys.stderr)
    return 1


def _read(path):
    with open(path, "rb") as handle:
        return handle.read()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""What `__appInit` promises, checked against the npdm and against itself (M9-1, #195).

Nothing in `sysmodule/source/main.cpp` can be executed off a console, so the
three bugs #195 found were all invisible to a green `ctest`: a `smExit()` that
takes `getaddrinfo` down with it, a `timeInitialize()` asking for a service the
npdm does not grant, and a service acquisition that parks forever in `sm` with
no log, no crash report and no symptom. Each of them is decidable from the text
of the two files that disagree, which is what this reads.

It is deliberately NOT the built `.nsp`: M9-3 (#196) lands that parser, and it
needs devkitPro and a cross-compile, so it skips on every runner this project
has today (M9-12, #211). The SAC and the source are in the tree, so these run
everywhere and on every push.

Four phases, one CTest entry each, so a red run names the promise that broke:

  dns       `__appInit` leaves the `sm` session open, because libnx re-opens
            `sfdnsres` off it on every `getaddrinfo` -- so an `smExit()` there
            makes every `server.url` naming a host unresolvable.

  clock     the `TimeServiceType` the sysmodule declares is the one
            `sys-rommsync.json` grants. libnx defaults to `time:u`; the npdm
            grants `time:s`; sm validates the SAC before anything else and
            fails the request outright.

  bounded   every service `__appInit` acquires is waited for with a bound
            first. A service in the SAC but not yet registered makes
            Atmosphere's sm DEFER the request rather than answer it, and a
            deferred request is never returned.

  comments  the two claims in this file that are false: that newlib exports no
            `fsync`, and -- by way of a second `nifmInitialize` -- that the
            parameters of a refcounted second init do anything at all.
"""
import argparse
import json
import re
import sys

# libnx's `TimeServiceType` and the service each value makes `timeInitialize`
# ask sm for (nx/source/services/time.c). `TimeServiceType_SystemUser` asks for
# `time:su`, which is a different grant again.
TIME_SERVICE = {
    "TimeServiceType_User": "time:u",
    "TimeServiceType_Menu": "time:a",
    "TimeServiceType_System": "time:s",
    "TimeServiceType_Repair": "time:r",
    "TimeServiceType_SystemUser": "time:su",
}

# Every libnx initialiser `__appInit` may call, and the service name sm is asked
# for when it does. `socketInitialize` opens `bsd:u` and `sfdnsres`; `nifm` is
# the one whose type argument decides.
SERVICE_OF_INIT = {
    "setsysInitialize": ["set:sys"],
    "fsInitialize": ["fsp-srv"],
    "timeInitialize": None,          # decided by __nx_time_service_type, see `clock`
    "nifmInitialize": ["nifm:u"],
    "socketInitialize": ["bsd:u", "sfdnsres"],
    "sslInitialize": ["ssl"],
}


def fail(message):
    print("FAIL: " + message, file=sys.stderr)
    return 1


def app_init(source):
    """The body of `__appInit`, brace-matched rather than grepped for.

    A `smExit()` in `__appExit` is correct and a `smExit()` in `__appInit` is
    the bug, so the two cannot be told apart by searching the file.

    Comments come out **first**, not after. `__appInit`'s own comments quote sm
    internals -- `R_UNLESS(service_info != nullptr, ...)` and friends -- and one
    brace in one of them would end the extracted body early, after which every
    phase below passes on whatever was left rather than on the function.
    """
    source = uncommented(source)
    at = source.find("void __appInit(void)")
    if at < 0:
        raise SystemExit(fail("no __appInit in sysmodule/source/main.cpp"))
    open_brace = source.index("{", at)
    depth = 0
    for i in range(open_brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace : i + 1]
    raise SystemExit(fail("__appInit has no closing brace"))


def uncommented(body):
    """`body` with `//` and `/* */` comments removed.

    Every phase below asks what the code CALLS, and this file's comments quote
    calls constantly -- including the ones being argued against.
    """
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    return re.sub(r"//[^\n]*", " ", body)


def phase_dns(repo):
    body = app_init(read(repo, "sysmodule/source/main.cpp"))
    if re.search(r"\bsmExit\s*\(", body):
        return fail(
            "__appInit calls smExit(); libnx re-opens `sfdnsres` off the sm session on "
            "every getaddrinfo (_sfdnsresDispatchImpl), so closing it makes every "
            "server.url naming a host answer kUnresolvedHost (#195)")
    print("ok: __appInit leaves the sm session open for getaddrinfo")
    return 0


def declared_time_service(repo):
    """The `TimeServiceType_*` main.cpp declares, or None.

    Read by two phases -- `clock` checks it against the SAC, `bounded` needs it
    to know which service `timeInitialize()` will ask for -- so it is read once
    here rather than by two copies of the same regex.
    """
    found = re.search(r"__nx_time_service_type\s*=\s*(TimeServiceType_\w+)",
                      uncommented(read(repo, "sysmodule/source/main.cpp")))
    return found.group(1) if found else None


def phase_clock(repo):
    declared = declared_time_service(repo)
    if declared is None:
        return fail(
            "sysmodule/source/main.cpp does not set __nx_time_service_type; libnx "
            "defaults it to TimeServiceType_User, which asks sm for `time:u` (#195)")
    wanted = TIME_SERVICE.get(declared)
    if wanted is None:
        return fail("unknown TimeServiceType " + declared)
    sac = npdm(repo)["service_access"]
    if wanted not in sac:
        return fail(
            "main.cpp declares %s, which asks sm for `%s`, and sys-rommsync.json grants "
            "%s. sm validates the SAC before the registration check and returns "
            "sm::ResultNotAllowed, so this fails on every boot (#195)"
            % (declared, wanted, sac))
    print("ok: %s asks for `%s`, which the npdm grants" % (declared, wanted))
    return 0


def phase_bounded(repo):
    body = app_init(read(repo, "sysmodule/source/main.cpp"))
    # `WaitForService` and `WaitForServiceOrAbort`: what differs is what the
    # caller does about a timeout, not whether it waited.
    waited = set(re.findall(r"WaitForService\w*\s*\(\s*\"([^\"]+)\"", body))
    failures = 0
    for call, services in SERVICE_OF_INIT.items():
        if not re.search(r"\b" + call + r"\s*\(", body):
            continue
        if services is None:
            declared = declared_time_service(repo)
            services = [TIME_SERVICE[declared]] if declared in TIME_SERVICE else []
        for service in services:
            if service not in waited:
                failures += fail(
                    "__appInit calls %s() without waiting for `%s` first. A service in "
                    "the SAC but not yet registered makes sm defer the request and never "
                    "answer it: the console boots, the sysmodule is inert, and there is "
                    "no crash report (#195)" % (call, service))
    # `NetworkInitialize()` acquires four sessions behind one call, and `nifm:u`
    # is one of them: since #195 moved the only `nifmInitialize` in the build in
    # there, the literal-call rule above no longer sees it, and a deleted
    # `WaitForService("nifm:u")` would leave this phase green over exactly the
    # unbounded `smGetService` it exists to catch.
    if re.search(r"\bNetworkInitialize\s*\(", body):
        for service in ("nifm:u", "bsd:u", "sfdnsres", "ssl"):
            if service not in waited:
                failures += fail(
                    "__appInit calls NetworkInitialize() without waiting for `%s` first "
                    "(#195)" % service)
    if failures:
        return 1
    print("ok: every service __appInit acquires is waited for with a bound")
    return 0


def phase_comments(repo):
    source = read(repo, "sysmodule/source/main.cpp")
    failures = 0

    # `fsync(fileno(fp))` compiles and links against libnx -- newlib's stub
    # reaches `fsdev_fsync` -> `fsFileFlush`. `fsdevCommitDevice` is still the
    # right call, for savedata's sake; the reason given for it was not.
    flowed = re.sub(r"\s*(?://+|/\*+|\*+/)?\s*\n\s*(?:///?|\*)?\s*", " ", source)
    if re.search(r"newlib\b[^.]*exports no `?fsync", flowed):
        failures += fail(
            "the HorizonFileSync comment still claims devkitA64's newlib exports no "
            "fsync; it does (fsdev_fsync -> fsFileFlush), and the reason for "
            "fsdevCommitDevice is savedata's commit, not a missing primitive (#195)")

    # One init, not two. The second call's parameters are silently ignored --
    # `nifmInitialize(NifmServiceType_System)` beside an earlier
    # `nifmInitialize(NifmServiceType_User)` would read as a change and be none.
    inits = 0
    for path in ("sysmodule/source/main.cpp", "sysmodule/source/http/ssl_http_client.cpp"):
        inits += len(re.findall(r"\bnifmInitialize\s*\(", uncommented(read(repo, path))))
    if inits > 1:
        failures += fail(
            "nifmInitialize is called %d times; it refcounts, so the second call's "
            "NifmServiceType is silently ignored (#195)" % inits)
    if failures:
        return 1
    print("ok: the fsync reason is right and nifm is initialised once")
    return 0


def read(repo, relative):
    with open(repo + "/" + relative, "r", encoding="utf-8") as handle:
        return handle.read()


def npdm(repo):
    return json.loads(read(repo, "sysmodule/sys-rommsync.json"))


PHASES = {
    "dns": phase_dns,
    "clock": phase_clock,
    "bounded": phase_bounded,
    "comments": phase_comments,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=sorted(PHASES))
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    return PHASES[args.phase](args.repo.rstrip("/"))


if __name__ == "__main__":
    sys.exit(main())

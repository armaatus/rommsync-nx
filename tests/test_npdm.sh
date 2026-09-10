#!/usr/bin/env bash
# The sysmodule's NPDM, checked against the image it ships beside (M9-3, #196).
#
#   test_npdm.sh svcs   cross-compiles sysmodule/ in devkitpro/devkita64 and
#                       runs scripts/npdm-check.py over the .nsp and the linked
#                       ELF: every `svc` instruction the image contains is a
#                       capability the NPDM grants, ACI0 and ACID agree, the
#                       ACID carries AcidFlag_Production and pool partition 2,
#                       and no @PLACEHOLDER@ reached the artifact.
#
# The parser half is `npdm.parser` in tests/CMakeLists.txt -- the same script's
# --self-test, which reads no artifact and so runs everywhere. This entry cannot:
# an NPDM only exists once npdmtool has run, and npdmtool is in the container.
# It therefore skips on every runner this project has today, which is M9-12's
# (#211) problem to fix and not a reason to invent a second mechanism here.
#
# This is the sysmodule's NPDM and only the sysmodule's. `ovl-rommsync` has none:
# an overlay runs inside nx-ovlloader's process and inherits its capabilities, so
# the same question asked about the overlay is a question about nx-ovlloader's
# NPDM (scripts/npdm-check.py says so at more length).
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="devkitpro/devkita64:latest"
SKIP=77

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

phase_svcs() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$IMAGE" >/dev/null 2>&1 ||
    skip "$IMAGE not pulled (docker pull $IMAGE)"

  # A copy, not the worktree, for the reasons tests/test_switch_build.sh gives
  # in full: the container builds as root and three worktrees may be running
  # this at once. Only the sysmodule is built -- the overlay has no NPDM, so
  # building it here would cost two minutes to produce nothing this reads.
  SCRATCH="$(mktemp -d)"
  cp "$REPO_ROOT/VERSION" "$REPO_ROOT/switch.mk" "$SCRATCH/"
  cp -R "$REPO_ROOT/core" "$REPO_ROOT/sysmodule" "$SCRATCH/"
  # Sources only. A .nsp left from a local build is what make would accept as
  # already up to date, and every assertion below would then be reading the
  # previous build's capabilities. Named by extension rather than cleared with a
  # `sys-rommsync.*` glob, which would also take sys-rommsync.json -- npdmtool's
  # input, without which there is no NPDM at all.
  local ext
  rm -rf "$SCRATCH/sysmodule/build"
  for ext in nsp nso npdm elf map lst; do
    rm -f "$SCRATCH/sysmodule/sys-rommsync.$ext"
  done

  local log="$SCRATCH/build.log"
  if ! docker run --rm --user "$(id -u):$(id -g)" -v "$SCRATCH:/work" -w /work "$IMAGE" \
        bash -lc 'make -C sysmodule -j"$(nproc)"' >"$log" 2>&1; then
    cat "$log" >&2
    fail "the sysmodule cross-compile failed"
  fi

  local nsp="$SCRATCH/sysmodule/sys-rommsync.nsp"
  local elf="$SCRATCH/sysmodule/sys-rommsync.elf"
  [ -s "$nsp" ] || fail "no sys-rommsync.nsp"
  # Asserted rather than assumed: the ELF is where the called set comes from, and
  # an absent one would make the diff below pass over an empty set of calls.
  [ -s "$elf" ] || fail "no sys-rommsync.elf; the SVC diff would pass vacuously"

  "${PYTHON:-python3}" "$REPO_ROOT/scripts/npdm-check.py" \
      --nsp "$nsp" --elf "$elf" --json "$SCRATCH/sysmodule/sys-rommsync.json" ||
    fail "the built NPDM does not cover what the image does"

  echo "ok: the NPDM grants every SVC the linked sysmodule issues"
}

case "${1:-}" in
  svcs) phase_svcs ;;
  *)    echo "usage: $0 svcs" >&2; exit 2 ;;
esac

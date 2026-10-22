#!/usr/bin/env bash
# The devkitPro toolchain every Switch build runs in, named once (#199).
#
# Sourced by the scripts that `docker run` it, so they share one name:
#
#   . scripts/toolchain.sh; docker run ... "$DEVKITPRO_IMAGE" ...
#
# and run inside the image itself, by the `switch-build` CI job and by
# `ctest -R toolchain.libnx`, to assert what the pin promises:
#
#   scripts/toolchain.sh libnx     the libnx here is >= LIBNX_FLOOR
#   scripts/toolchain.sh image     print DEVKITPRO_IMAGE
#
# A dated tag rather than `:latest`, because `:latest` moves under us: a libnx
# rename (v4.6.0's NORETURN -> NX_NORETURN broke ovl-sysmodules, its #32) would
# first show as a red build on a PR that touched nothing Switch-side. The
# `switch-build-canary` CI job still builds against `:latest`, allowed to fail,
# so a break is news from CI rather than from someone's unrelated PR.
#
# GitHub Actions cannot read this file -- a job's `container:` is resolved
# before any step runs -- so ci.yml repeats the tag, and the documented build
# commands do too. `ctest -R toolchain.pins` fails on any of them disagreeing
# with the line below. Moving the pin is docs/DEVELOPMENT.md#the-toolchain-pin.
DEVKITPRO_IMAGE="devkitpro/devkita64:20260219"

# HOS 21.0.0 changed the userland<->kernel TLS ABI. Atmosphere: "All homebrew
# software may need to be re-compiled with the latest libnx (>= 4.10.0)", and
# Ultrahand hides an overlay whose MOD0 lacks the LNY2 stamp libnx 4.10.0 began
# writing (#211). A pin below this builds green and ships something that does
# not appear, or crashes on exit, on any current console.
LIBNX_FLOOR="4.10.0"

# version_at_least VERSION FLOOR
#   0  VERSION >= FLOOR
#   1  VERSION <  FLOOR
#   2  VERSION or FLOOR is not dotted numbers (a pacman `-pkgrel` is dropped)
#
# Numeric per component, because the floor itself is the case a string compare
# gets wrong: "4.9.3" sorts after "4.10.0". Missing components count as zero.
version_at_least() {
  local version="${1%%-*}" floor="${2%%-*}"
  local re='^[0-9]+(\.[0-9]+)*$'
  [[ "$version" =~ $re && "$floor" =~ $re ]] || return 2
  local -a v f
  IFS=. read -r -a v <<<"$version"
  IFS=. read -r -a f <<<"$floor"
  local i n=$(( ${#v[@]} > ${#f[@]} ? ${#v[@]} : ${#f[@]} ))
  for (( i = 0; i < n; i++ )); do
    # 10# so a component like "08" is decimal, not an invalid octal.
    local a=$(( 10#${v[i]:-0} )) b=$(( 10#${f[i]:-0} ))
    (( a > b )) && return 0
    (( a < b )) && return 1
  done
  return 0
}

# Asked of the package manager, because libnx installs no version header: the
# only record of which libnx a devkitPro image carries is pacman's database.
# scripts/release-notes.sh reads the same thing for the release notes.
toolchain_check_libnx() {
  local q version rc=0
  q="$(dkp-pacman -Q libnx 2>/dev/null)" || {
    echo "::error::dkp-pacman cannot say which libnx this is; not inside a devkitPro image?" >&2
    return 1
  }
  version="${q#libnx }"
  # Captured with `||`: run directly this script is `set -e`, and a bare call
  # returning 1 or 2 would end it before either message below could print.
  version_at_least "$version" "$LIBNX_FLOOR" || rc=$?
  case $rc in
    0) echo "ok: libnx $version >= $LIBNX_FLOOR" ;;
    1) echo "::error::libnx $version is below $LIBNX_FLOOR; homebrew built with it" \
            "breaks on HOS 21.0.0 and Ultrahand will not list the overlay" >&2
       return 1 ;;
    *) echo "::error::cannot read a libnx version out of '$q'" >&2
       return 1 ;;
  esac
}

# --- running something in it ----------------------------------------------------
#
# The ctest entries that need the toolchain (`ctest -L toolchain`) reach it one
# of two ways. On a workstation they `docker run` the pinned image over a copy of
# the tree. In CI they are already inside it -- the `switch-build` job's
# container is the pin, and it has no Docker to start another (M9-12, #211) --
# so tests/toolchain_suite.sh sets ROMMSYNC_IN_TOOLCHAIN=1 and the same command
# runs in place. Explicit rather than sniffed: a devkitPro installed natively on
# a laptop is not the pin, and must not be mistaken for it.

# toolchain_here -- is this shell inside the pinned image already?
toolchain_here() { [ "${ROMMSYNC_IN_TOOLCHAIN:-}" = 1 ]; }

# toolchain_missing -- prints why the toolchain cannot be reached, and succeeds,
# when it cannot; fails silently when it can. For a test's skip message.
toolchain_missing() {
  if toolchain_here; then
    [ -x "${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-gcc" ] && return 1
    echo "ROMMSYNC_IN_TOOLCHAIN=1, but there is no devkitA64 here"
    return 0
  fi
  command -v docker >/dev/null 2>&1 || { echo "no docker"; return 0; }
  docker info >/dev/null 2>&1 || { echo "docker daemon not running"; return 0; }
  docker image inspect "$DEVKITPRO_IMAGE" >/dev/null 2>&1 ||
    { echo "$DEVKITPRO_IMAGE not pulled (docker pull $DEVKITPRO_IMAGE)"; return 0; }
  return 1
}

# toolchain_run DIR SCRIPT [ARG...] -- `bash -lc SCRIPT` with DIR as its working
# directory, inside the pinned toolchain, ARGs as its "$@". A login shell either
# way, because the image's /etc/profile.d is what puts devkitPro on PATH.
#
# --user so a container build leaves files this user can delete: Docker Desktop
# maps ownership back on macOS, but on Linux the default root would leave a
# temp directory rm -rf cannot clear.
toolchain_run() {
  local dir="$1" script="$2"
  shift 2
  if toolchain_here; then
    ( cd "$dir" && bash -lc "$script" _ "$@" )
  else
    docker run --rm --user "$(id -u):$(id -g)" -v "$dir:/work" -w /work \
      "$DEVKITPRO_IMAGE" bash -lc "$script" _ "$@"
  fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -euo pipefail
  case "${1:-}" in
    image) echo "$DEVKITPRO_IMAGE" ;;
    libnx) toolchain_check_libnx ;;
    *)     echo "usage: $0 {image|libnx}" >&2; exit 2 ;;
  esac
fi

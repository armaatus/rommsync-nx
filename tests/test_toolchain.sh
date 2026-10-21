#!/usr/bin/env bash
# M9-8 (#199): the two things every Switch build depends on and nothing in this
# repo produces -- the devkitPro image and the libultrahand submodule -- are
# pinned, and the pins say what they are.
#
#   test_toolchain.sh pins       every place that names the devkitPro image names
#                                the dated tag scripts/toolchain.sh pins, except
#                                one canary job on `:latest` that CI lets fail.
#                                This is the regression: every build pulled
#                                `:latest`, so a devkitPro release could turn an
#                                unrelated PR red with nothing here having moved.
#   test_toolchain.sh submodule  the libultrahand gitlink is the commit of the
#                                tag .gitmodules records beside it -- not a
#                                `main` head, which upstream calls a
#                                pre-release "in constant revision".
#   test_toolchain.sh floor      the libnx version comparison, on literals: a
#                                plain string compare reads 4.9 as newer than
#                                4.10, which is exactly the floor in question.
#   test_toolchain.sh libnx      the pinned image's libnx really is >= the floor
#                                (HOS 21.0.0's TLS ABI; #199, #211). Needs Docker
#                                and the image, so it skips without them; the
#                                `switch-build` CI job runs the same check in the
#                                container and is the enforcement there.
#
# `pins`, `submodule` and `floor` read the tree only, so they never skip.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN="$REPO_ROOT/scripts/toolchain.sh"
CI="$REPO_ROOT/.github/workflows/ci.yml"
SKIP=77

SCRATCH=""
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT
# Split so this file's own text is not one of the hits phase_pins counts.
LATEST="devkitpro/devkita64"':latest'

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit $SKIP; }

load_toolchain() {
  [ -f "$TOOLCHAIN" ] || fail "no scripts/toolchain.sh; nothing names the pinned image"
  # shellcheck source=../scripts/toolchain.sh
  . "$TOOLCHAIN"
}

# One job of ci.yml, from `  <id>:` up to the next 2-space key. The same
# extraction test_switch_build.sh uses, for the reason it gives.
ci_job() {
  awk -v id="$1" '$0 == "  " id ":" { in_job = 1; next }
       in_job && /^  [^ #]+:/ { in_job = 0 }
       in_job { print }' "$CI"
}

# --- the image ----------------------------------------------------------------

phase_pins() {
  [[ "$DEVKITPRO_IMAGE" =~ ^devkitpro/devkita64:[0-9]{8}$ ]] ||
    fail "DEVKITPRO_IMAGE is '$DEVKITPRO_IMAGE'; it must be a dated devkita64 tag"

  # Every tracked or new file in the tree, so a script added next month that
  # types its own image name is caught too. The pattern cannot match itself:
  # in this file the colon is followed by a bracket.
  local hits line text bad="" latest=0
  hits="$(git -C "$REPO_ROOT" grep --untracked -n -I -o -E \
            'devkitpro/devkita64:[A-Za-z0-9._-]+' -- . ':!overlay/lib' || true)"
  [ -n "$hits" ] || fail "nothing in the tree names the devkitPro image at all"
  while IFS= read -r line; do
    text="${line#*:*:}"
    if [ "$text" = "$DEVKITPRO_IMAGE" ]; then
      continue
    elif [ "${line%%:*}" = .github/workflows/ci.yml ] &&
         [ "$text" = "$LATEST" ]; then
      latest=$((latest + 1))   # the canary, checked below
    else
      bad+="  $line"$'\n'
    fi
  done <<<"$hits"
  [ -z "$bad" ] || fail "these name a devkitPro image other than $DEVKITPRO_IMAGE:
${bad}Moving the pin is scripts/toolchain.sh plus every line above; see docs/DEVELOPMENT.md#the-toolchain-pin"

  # The builds that matter run in the pin: what a PR is checked by, and what a
  # tag publishes.
  local job
  for job in switch-build release; do
    grep -qxF "    container: $DEVKITPRO_IMAGE" <<<"$(ci_job "$job")" ||
      fail "the $job job does not run in $DEVKITPRO_IMAGE"
  done
  grep -q 'scripts/toolchain.sh libnx' <<<"$(ci_job switch-build)" ||
    fail "switch-build does not assert the image's libnx floor"

  # ...and `:latest` appears exactly once, in a canary that cannot block a PR.
  [ "$latest" -eq 1 ] ||
    fail "ci.yml names :latest $latest times; it belongs to the canary job alone"
  local canary
  canary="$(ci_job switch-build-canary)"
  [ -n "$canary" ] || fail "no switch-build-canary job in ci.yml"
  grep -qxF "    container: $LATEST" <<<"$canary" ||
    fail "switch-build-canary does not build against :latest"
  grep -qxF '    continue-on-error: true' <<<"$canary" ||
    fail "switch-build-canary is not allowed to fail; a devkitPro release would block every PR"
  local target
  for target in sysmodule overlay tlsprobe; do
    grep -q -- "make -C $target" <<<"$canary" ||
      fail "switch-build-canary does not build $target"
  done
  grep -q 'scripts/toolchain.sh libnx' <<<"$canary" ||
    fail "switch-build-canary does not check :latest's libnx floor"
  # Nothing may wait on it, or allowed-to-fail stops meaning anything.
  if grep -qE '^    needs:.*switch-build-canary' "$CI"; then
    fail "a job needs switch-build-canary"
  fi

  echo "ok: every build runs in $DEVKITPRO_IMAGE; :latest only in the canary, which may fail"
}

# --- libultrahand -------------------------------------------------------------

phase_submodule() {
  local path=overlay/lib/libultrahand recorded tag sha pinned
  # The comment beside the entry, which is the only place the tag name can live:
  # a gitlink is a bare sha, and the library carries no version macro (#199).
  recorded="$(sed -nE 's/^# Pinned at tag (v[0-9][0-9A-Za-z.-]*) = ([0-9a-f]{40}).*/\1 \2/p' \
                "$REPO_ROOT/.gitmodules")"
  [ -n "$recorded" ] ||
    fail ".gitmodules records no '# Pinned at tag vX.Y.Z = <sha>' line for $path"
  [ "$(wc -l <<<"$recorded")" -eq 1 ] || fail ".gitmodules records more than one pin"
  tag="${recorded% *}"
  sha="${recorded#* }"

  # The index, not HEAD: it is what the next commit records and what a
  # `git submodule update` checks out.
  pinned="$(git -C "$REPO_ROOT" rev-parse ":$path" 2>/dev/null)" ||
    fail "$path is not a submodule in the index"
  [ "$pinned" = "$sha" ] ||
    fail "$path is pinned at $pinned, but .gitmodules says $tag = $sha"

  # Where the submodule is checked out with its tags -- a developer's clone,
  # not CI's depth-1 one -- the tag itself must agree, and the checkout must
  # be the pin: a build from anything else is not the build the pin describes.
  if [ -e "$REPO_ROOT/$path/.git" ]; then
    local head
    head="$(git -C "$REPO_ROOT/$path" rev-parse HEAD)"
    [ "$head" = "$sha" ] ||
      fail "$path is checked out at $head, not the pinned $sha (git submodule update)"
    local tagged
    if tagged="$(git -C "$REPO_ROOT/$path" rev-parse -q --verify "refs/tags/$tag^{commit}")"; then
      [ "$tagged" = "$sha" ] || fail "$tag is $tagged upstream, not the recorded $sha"
    else
      echo "note: $tag is not fetched in this checkout; the recorded sha stands alone"
    fi
  fi

  echo "ok: libultrahand is pinned at $tag ($sha)"
}

# --- the libnx floor ----------------------------------------------------------

phase_floor() {
  local version floor want got
  # version  floor   expected (0 = at least, 1 = below, 2 = unreadable)
  while read -r version floor want; do
    [ "$version" = - ] && version=""
    version_at_least "$version" "$floor"
    got=$?
    [ "$got" = "$want" ] ||
      fail "version_at_least '$version' '$floor' answered $got, expected $want"
  done <<'EOF'
4.12.0-1  4.10.0  0
4.10.0    4.10.0  0
4.10      4.10.0  0
10.0.0    4.10.0  0
4.10.1    4.10.0  0
4.9.3     4.10.0  1
4.9.99-2  4.10.0  1
3.20.0    4.10.0  1
4.10.0    4.10.1  1
-         4.10.0  2
abc       4.10.0  2
4..1      4.10.0  2
EOF
  echo "ok: version_at_least orders 4.9 below 4.10 and refuses what it cannot read"
}

# The check as CI runs it -- executed, under its own `set -e` -- against a stub
# dkp-pacman, so the failures it exists to explain can be produced on any host.
# A red switch-build whose only output is an exit status is the regression:
# the comparison's non-zero status once ended the script before either
# ::error:: line could print.
phase_check() {
  local stub out rc pkg want_rc want_text
  SCRATCH="$(mktemp -d)"
  stub="$SCRATCH/bin"
  mkdir -p "$stub"
  while IFS='|' read -r pkg want_rc want_text; do
    printf '#!/bin/sh\n[ "$1 $2" = "-Q libnx" ] || exit 1\necho "%s"\n' "$pkg" \
      >"$stub/dkp-pacman"
    chmod +x "$stub/dkp-pacman"
    out="$(PATH="$stub:$PATH" bash "$TOOLCHAIN" libnx 2>&1)"
    rc=$?
    [ "$rc" = "$want_rc" ] ||
      fail "toolchain.sh libnx exited $rc for '$pkg', expected $want_rc: $out"
    grep -qF -- "$want_text" <<<"$out" ||
      fail "toolchain.sh libnx said '$out' for '$pkg'; expected it to say '$want_text'"
  done <<'EOF'
libnx 4.12.0-1|0|ok: libnx 4.12.0-1 >= 4.10.0
libnx 4.9.3-1|1|::error::libnx 4.9.3-1 is below 4.10.0
libnx garbage|1|::error::cannot read a libnx version out of 'libnx garbage'
EOF
  # ...and no dkp-pacman at all is a failure that says where it is, not a pass.
  printf '#!/bin/sh\nexit 1\n' >"$stub/dkp-pacman"
  out="$(PATH="$stub:$PATH" bash "$TOOLCHAIN" libnx 2>&1)" &&
    fail "toolchain.sh libnx passed with no libnx to ask about"
  grep -qF 'not inside a devkitPro image' <<<"$out" ||
    fail "toolchain.sh libnx failed silently without dkp-pacman: '$out'"
  echo "ok: toolchain.sh libnx passes 4.12, and says why it refuses 4.9 and garbage"
}

phase_libnx() {
  command -v docker >/dev/null 2>&1 || skip "no docker"
  docker info >/dev/null 2>&1 || skip "docker daemon not running"
  docker image inspect "$DEVKITPRO_IMAGE" >/dev/null 2>&1 ||
    skip "$DEVKITPRO_IMAGE not pulled (docker pull $DEVKITPRO_IMAGE)"
  docker run --rm -v "$REPO_ROOT/scripts:/scripts:ro" "$DEVKITPRO_IMAGE" \
    bash -lc 'bash /scripts/toolchain.sh libnx' ||
    fail "$DEVKITPRO_IMAGE's libnx is not one toolchain.sh accepts (output above)"
}

case "${1:-}" in
  pins)      load_toolchain; phase_pins ;;
  submodule) phase_submodule ;;
  floor)     load_toolchain; phase_floor ;;
  check)     load_toolchain; phase_check ;;
  libnx)     load_toolchain; phase_libnx ;;
  *)         echo "usage: $0 {pins|submodule|floor|check|libnx}" >&2; exit 2 ;;
esac

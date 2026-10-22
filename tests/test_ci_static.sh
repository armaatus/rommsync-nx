#!/usr/bin/env bash
# The `static` job in .github/workflows/ci.yml: the cheap checks every push gets
# first, on the scripts that build, check and ship everything else.
#
#   test_ci_static.sh python   every Python file under scripts/ and server/ is
#                              one the job byte-compiles. It compiled server/
#                              alone, so a syntax error in artifact-check.py,
#                              npdm-check.py or stack-budget.py waited for
#                              host-tests' build and RomM, or switch-build's
#                              cross-compile, to be reported (#256).
#
# Reads the checkout only, so it never skips.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CI="$REPO_ROOT/.github/workflows/ci.yml"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The job named $1 in ci.yml, as tests/test_soak.sh reads one.
ci_job() {
  awk -v job="  $1:" '$0 == job { in_job = 1; next }
       in_job && /^  [^ #]+:/ { in_job = 0 }
       in_job { print }' "$CI"
}

phase_python() {
  local job line
  job="$(ci_job static)"
  [ -n "$job" ] || fail "ci.yml has no static job"
  line="$(grep -v '^[[:space:]]*#' <<<"$job" | grep -m1 'compileall')" ||
    fail "the static job byte-compiles no Python at all"

  # Its arguments, without the flags: the directories (or files) it covers.
  local covered=() word
  for word in ${line#*compileall}; do
    case "$word" in -*) ;; *) covered+=("${word%/}") ;; esac
  done
  [ "${#covered[@]}" -gt 0 ] || fail "compileall is given nothing to compile: $line"

  # Derived from the tree rather than listed, so a script added next week is
  # held to it without an edit here.
  local file root ok missed=()
  while IFS= read -r file; do
    ok=""
    for root in "${covered[@]}"; do
      case "$file" in "$root" | "$root"/*) ok=1 ;; esac
    done
    [ -n "$ok" ] || missed+=("$file")
  done < <(cd "$REPO_ROOT" && find scripts server -name '*.py' -not -path '*/__pycache__/*' | sort)

  [ "${#missed[@]}" -eq 0 ] ||
    fail "the static job does not byte-compile ${missed[*]}; a syntax error there would \
wait for a job that builds first (#256)"
  echo "ok: the static job byte-compiles every Python file under scripts/ and server/"
}

case "${1:-}" in
  python) phase_python ;;
  *)      echo "usage: $0 python" >&2; exit 2 ;;
esac

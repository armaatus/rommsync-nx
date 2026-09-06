#!/usr/bin/env bash
# The v1 gate -- M8-1 (#43), the thing CLAUDE.md hard rule 1 points at.
#
#   scripts/v1-gate.sh                 run every row's CTest groups, evaluate
#                                      all eight rows, print the verdict
#   scripts/v1-gate.sh --dry           ...without running anything: which rows
#                                      would be decided, and which are held
#   scripts/v1-gate.sh --audit         only: is the gate itself still well
#                                      formed? (this is what `ctest -R gate`
#                                      runs -- see below)
#   scripts/v1-gate.sh --rows          the rows, one per line, machine-readable
#
#   --build-dir DIR                    where CTest lives (default: ./build, or
#                                      $ROMMSYNC_BUILD_DIR)
#
# WHY THIS FILE EXISTS. The gate was eight sentences in an issue body, repeated
# in docs/TESTING.md and alluded to in a dozen source comments, and nothing
# evaluated any of them. docs/TESTING.md says exactly what is wrong with that,
# one milestone down, about the M0 exit gate: "A box checked because someone
# believes it is not checked." So each row here names the command that
# demonstrates it, and a row whose evidence has been renamed away fails rather
# than quietly passing.
#
# WHAT IT DOES NOT DO. It does not decide anything about a console, and it never
# prints PASS for the gate as a whole while a row is held. Two of the eight rows
# cannot be settled anywhere in this repository -- not by more tests, not by a
# better argument -- and the gate's job is to say so precisely rather than to
# leave them looking like work somebody forgot.
#
# EXIT CODES, which are the actual answer:
#
#   0   every row holds. The gate passes. Hardware work (M8-2, #44) may begin.
#   1   a row this machine can decide is FAILING.
#   2   usage.
#   3   nothing failing, but a row is HELD -- it needs a console, or a person.
#
# 3 is the expected answer today. It is deliberately not 0: a gate that exits 0
# while two of its rows have never been executed anywhere is the same
# green-that-checked-nothing this file exists to prevent.
#
# AND 0 IS UNREACHABLE UNTIL SOMEBODY EDITS THIS FILE. There is no flag, file or
# environment variable that ticks a `console` row, and that is the design rather
# than an omission: no input a script can take is evidence that a handshake
# happened on a console, so accepting one would be a checkbox pretending to be a
# measurement. When a console has answered a row, the answer goes into the docs
# and the row stops being `console` in the same commit -- a reviewed edit, by a
# person, is the attestation. Each console row says where its answer goes.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROMMSYNC_BUILD_DIR:-$REPO_ROOT/build}"
MODE="run"

die() { echo "v1-gate.sh: $*" >&2; exit 2; }

# The header comment above, minus its leading `#`, up to the first blank line
# that is not part of it. Reading BASH_SOURCE rather than rebuilding the path
# means a copy of this script under another name still prints its own header.
usage() {
  sed -n '2,/^set -uo pipefail$/p' "${BASH_SOURCE[0]}" | sed 's/^#\{1,\} \{0,1\}//; /^set -uo/d'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --run)        MODE="run"; shift ;;
    --dry)        MODE="dry"; shift ;;
    --audit)      MODE="audit"; shift ;;
    --rows)       MODE="rows"; shift ;;
    --build-dir)  [ $# -ge 2 ] || die "--build-dir needs a path"; BUILD_DIR="$2"; shift 2 ;;
    -h|--help)    usage; exit 0 ;;
    *)            die "unknown argument '$1'" ;;
  esac
done

# --- the rows -----------------------------------------------------------------
# id|kind|claim
#
# The order is the order of the checklist in issue #43, and the claims are its
# words. Three kinds:
#
#   suite    a claim about the host suite. Demonstrated by CTest groups named in
#            `row_tests` below; `--run` runs them.
#   repo     a claim about the state of this repository, decided by `row_repo`.
#   console  a claim nothing off a console can settle. Always HELD. `row_note`
#            says what would settle it and where the answer goes.
#
# A row may be `suite` and still carry a `row_repo` predicate -- `backup` is,
# because "every save-overwrite path" is a claim about the tree and not only
# about which tests exist.
gate_rows() {
  cat <<'ROWS'
sync|suite|Full sync engine (M2) passes on host + docker RomM, including conflict / partial-failure / resume.
downloads|suite|Downloads (M3) pass with Range resume + hash verify against docker RomM.
auth|suite|Auth (M1) full device-code flow + 401/refresh proven on host + docker RomM.
ipc|suite|Config + IPC (M5) proven on host harness.
ssl|console|HttpClient ssl-service backend proven in a Ryujinx NRO (M0-1), or on an isolated NRO on a backup SD.
backup|suite|Every save-overwrite path shown to back up first (SYNC_PROTOCOL hard rule) -- verified by tests.
release|repo|A tagged, released v1 build exists (M6).
media|console|A known-good NAND/SD backup exists; testing will be on a spare/backup SD or emuMMC, not the daily driver.
ROWS
}

# The CTest groups that demonstrate a `suite` row. Each entry is a CTest regex;
# a regex that matches nothing registered is a FAILING row, not an empty one --
# that is the whole point, because the way this rots is a rename.
row_tests() {
  case "$1" in
    sync)
      # The engine, end to end, plus the three edge cases the checklist names by
      # hand: `harness.conflict`, `harness.partial` and `harness.resume` are the
      # ones that force the case rather than waiting for a server to produce it.
      cat <<'T'
^sync\.
^execute\.
^states\.
^complete\.
^tick\.
^scan\.
^core\.state_db$
^core\.md5$
^core\.sha1
^harness\.conflict$
^harness\.partial$
^harness\.resume$
^harness\.same_timestamp$
T
      ;;
    downloads)
      # `Range` resume and hash verify are named in the box, so they are named
      # here rather than left inside `download.*`: both transports are held to
      # them, out of one source file compiled twice (tests/test_http_native.cpp).
      cat <<'T'
^download\.
^rom\.
^toggle\.download$
^http\.range
^http\.resume
^wire\.range
^wire\.resume
^harness\.content_hash$
^harness\.multifile$
T
      ;;
    auth)
      # M1 whole: the device-code flow, the 401/refresh gate, the token and
      # device records, and the three `engine.*` entries M1-6 added when the
      # console got a pairing thread of its own.
      cat <<'T'
^auth\.
^pair\.
^device\.
^core\.token_store$
^core\.device_identity$
^engine\.pairs$
^engine\.repairs$
^engine\.nonblocking$
^engine\.unauthenticated$
^harness\.expired$
T
      ;;
    ipc)
      # "on host harness" is the operative half: `overlay.*` is in here because
      # the overlay's half of the protocol is exercised by the host suite, and
      # the drawing it does on top of that is M8-2's to verify, not this row's.
      cat <<'T'
^core\.config$
^config\.
^ipc\.
^lists\.
^overlay\.
^engine\.config$
^engine\.commands$
T
      ;;
    backup)
      # Hard rule 2. `harness.backup` is the server-side half; `execute.*` and
      # `states.*` are the two paths that overwrite bytes a player cares about;
      # `tick.backupdir` and `tick.durable` are the ones that check the backup
      # survives the interruption it exists for.
      cat <<'T'
^harness\.backup$
^execute\.
^states\.overwrite$
^states\.keeps_both$
^tick\.backupdir$
^tick\.durable$
T
      ;;
    *) : ;;
  esac
}

# --- repo predicates ----------------------------------------------------------
# Each prints its reasoning on stdout and answers with an exit status: 0 holds,
# 1 does not, **2 is not known from here**. The third is not a courtesy: a row
# nobody can decide is HELD, and calling that FAIL would make "we could not ask"
# indistinguishable from "we asked and the answer is no".

# Every place in `core/` that commits bytes over an existing file, counted per
# file and per helper and classified. This is the word "every" in the box above,
# which nothing checked before: the tests prove that each save path backs up
# first, and this proves the set of them is still the set somebody classified.
#
# Pinned per file rather than per line, so editing inside a function does not
# move it. The limit is honest and worth stating: two call sites swapping files
# would balance out and pass. What it does catch is the thing that actually
# happens -- a new overwrite path appearing in a file that had none, or in a
# file of its own.
# file | CommitStaged | CopyAtomically | WriteAtomically | the call that lands
# ON A SAVE, empty when none does | what it is
#
# The fifth field is what makes this more than a headcount: a row that names a
# landing call is a save-overwrite path, and the check below insists a CALL to
# `sync::BackUpFirst` comes before it in the file. Adding a save path is then a
# census edit rather than a code edit -- which is the point, since the row this
# serves is the one that says there are only these.
SAVE_SITE_CENSUS='core/src/auth_gate.cpp|0|0|1||not a save: the auth backoff record
core/src/conflict_log.cpp|0|0|1||not a save: the conflict log M7-1 writes
core/src/conflict_record.cpp|0|1|0|io::CopyAtomically|SAVE (M7-1 restore): putting a backup back IS an overwrite, so sync::BackUpFirst runs first
core/src/device_identity.cpp|0|0|2||not a save: device.dat
core/src/download.cpp|1|0|1||not a save: rom bytes, and the queue record
core/src/play_sessions.cpp|0|0|1||not a save: the play-session buffer M7-4 writes
core/src/state_db.cpp|0|0|1||not a save: the sync baseline
core/src/state_sync.cpp|1|0|0|io::CommitStaged|SAVE STATE: sync::BackUpFirst runs first
core/src/sync_execute.cpp|1|1|0|io::CommitStaged|SAVE: sync::BackUpFirst runs first, and IS the CopyAtomically here
core/src/token_store.cpp|0|0|1||not a save: token.dat
sysmodule/source/engine.cpp|0|0|1||not a save: config.ini, written back by the engine'

# Where the census looks. `core/` is where the engine lives and where a save
# path belongs, but the box says EVERY save-overwrite path and the two Horizon
# targets can write files too -- sysmodule/source/engine.cpp already does. A
# census that only read core/ would answer a narrower question than the row asks.
SAVE_SITE_ROOTS='core/src sysmodule/source overlay/source'

# Comments stripped, then matched anywhere on the line. Calls, not mentions:
# these helpers are discussed in prose in files that never call them, so the
# prose has to go -- but anchoring at the left instead (`^[^/]*io::X(`) refuses
# any line carrying a `/` before the call, which `dir / name` does, and a save
# path written that way would be invisible to the one check that says which files
# can land on a save at all.
#
# Both comment forms are handled, `/* */` blocks included and across lines: see
# strip_comments for why that is not tidiness.
strip_comments() {
  # One line out per line in -- the ordering check below compares line NUMBERS,
  # so a filter that dropped lines would quietly compare the wrong ones.
  #
  # Both comment forms, because either can carry the literal text of a call.
  # `//` alone was not enough: a `/* ... BackUpFirst(...) removed here ... */`
  # block does not start with a letter, so the definition filter did not exclude
  # it either, and a save path whose real backup call had been deleted would have
  # matched the comment instead and read as protected. That is the hard rule this
  # whole row exists for, so it gets the state machine rather than a regex.
  #
  # `://` is left alone so a `"http://"` in a string does not eat its own line.
  # A `/*` inside a string literal would still fool this; there is none in the
  # sources it reads, and saying so is better than implying it cannot happen.
  awk '
    {
      line = $0; out = ""
      while (length(line) > 0) {
        if (inblk) {
          i = index(line, "*/")
          if (i == 0) { line = ""; break }
          line = substr(line, i + 2); inblk = 0; continue
        }
        a = index(line, "//"); b = index(line, "/*")
        while (a > 1 && substr(line, a - 1, 1) == ":") {
          j = index(substr(line, a + 2), "//")
          if (j == 0) { a = 0; break }
          a = a + 2 + j - 1
        }
        if (a == 0 && b == 0) { out = out line; line = ""; break }
        if (b != 0 && (a == 0 || b < a)) {
          out = out substr(line, 1, b - 1); line = substr(line, b + 2); inblk = 1; continue
        }
        out = out substr(line, 1, a - 1); line = ""; break
      }
      print out
    }' "$1"
}

count_calls() {
  local n
  [ -f "$REPO_ROOT/$1" ] || { echo 0; return 0; }
  n="$(strip_comments "$REPO_ROOT/$1" | grep -c "io::$2(")"
  echo "${n:-0}"
}

repo_backup() {
  local ok=0 line file want_commit want_copy want_write why
  local got_commit got_copy got_write f rel

  while IFS='|' read -r file want_commit want_copy want_write lands why; do
    [ -n "$file" ] || continue
    if [ ! -f "$REPO_ROOT/$file" ]; then
      echo "    census names $file, which is not in the tree"
      ok=1
      continue
    fi
    got_commit="$(count_calls "$file" CommitStaged)"
    got_copy="$(count_calls "$file" CopyAtomically)"
    got_write="$(count_calls "$file" WriteAtomically)"
    if [ "$got_commit" != "$want_commit" ] || [ "$got_copy" != "$want_copy" ] \
       || [ "$got_write" != "$want_write" ]; then
      echo "    $file commits ${got_commit}/${got_copy}/${got_write}" \
           "(CommitStaged/CopyAtomically/WriteAtomically), census says" \
           "${want_commit}/${want_copy}/${want_write} -- $why"
      echo "    classify the new site: if it can land on a save, it backs up first."
      ok=1
    fi
  done <<EOF
$SAVE_SITE_CENSUS
EOF

  # A file that calls one of the three and is not in the census at all is the
  # case the loop above cannot see, and is the one a new overwrite path takes.
  local root
  for root in $SAVE_SITE_ROOTS; do
    [ -d "$REPO_ROOT/$root" ] || continue
    for f in $(find "$REPO_ROOT/$root" -name '*.cpp' 2>/dev/null); do
    rel="${f#$REPO_ROOT/}"
    [ "$rel" = "core/src/atomic_file.cpp" ] && continue   # it defines them
    if strip_comments "$f" | grep -q "io::\(CommitStaged\|CopyAtomically\|WriteAtomically\)("; then
      case "$SAVE_SITE_CENSUS" in
        *"$rel|"*) ;;
        *) echo "    $rel commits over a file and is not in the census"
           echo "    classify it: if it can land on a save, it backs up first."
           ok=1 ;;
      esac
    fi
    done
  done

  # ...and every row that lands on a save still CALLS the backup, ahead of it.
  #
  # A call and not a mention, and not the definition either: `BackUpFirst` is
  # defined in sync_execute.cpp, so a pattern that accepts any occurrence is
  # satisfied by the definition and would sit green through the deletion of the
  # only call to it -- in the one file where that deletion destroys a save.
  # Definitions and declarations start in column 1 (a return type); calls are
  # indented. `strip_comments` takes the prose, since both files discuss the
  # helper at length. Line numbers survive it -- it blanks lines, never drops
  # them -- which is what makes the ordering below mean anything.
  #
  # POSIX classes rather than \s throughout: BSD grep reads \s as a literal `s`,
  # which is not a filter at all on the machine most of this is written on.
  #
  # Line order is a crude proof and is not pretending otherwise; what it holds is
  # that deleting the backup call cannot go unnoticed.
  local saves=0
  while IFS='|' read -r file want_commit want_copy want_write lands why; do
    [ -n "$lands" ] || continue
    [ -f "$REPO_ROOT/$file" ] || continue
    saves=$((saves + 1))
    local backup_line land_line
    backup_line="$(strip_comments "$REPO_ROOT/$file" | grep -n "BackUpFirst(" \
                   | grep -vE '^[0-9]+:[A-Za-z_]' | head -1 | cut -d: -f1)"
    land_line="$(strip_comments "$REPO_ROOT/$file" | grep -n "$lands(" \
                 | head -1 | cut -d: -f1)"
    if [ -z "$backup_line" ] || [ -z "$land_line" ] || [ "$backup_line" -ge "$land_line" ]; then
      echo "    $file writes onto a save with no CALL to BackUpFirst ahead of it"
      ok=1
    fi
  done <<EOF
$SAVE_SITE_CENSUS
EOF
  [ "$saves" -ge 1 ] || { echo "    the census names no save-overwrite path at all"; ok=1; }

  local counted
  counted="$(echo "$SAVE_SITE_CENSUS" | grep -c .)"
  [ "$ok" -eq 0 ] && echo "    $counted files commit over an existing file; $saves of them can land on a save, and each calls BackUpFirst first"
  return "$ok"
}

# The only network call in this file, and CLAUDE.md is explicit that every one of
# them carries a timeout. A `gh` that hangs -- slow DNS, an auth prompt nobody is
# there to answer, a stalled connection to api.github.com -- would otherwise hang
# the gate, which a person runs by hand before touching a console.
#
# A killed call produces no output, and no output is already "could not ask" a
# few lines down: unknown, which is HELD and not a pass. So the timeout needs no
# special case, only a bound.
#
# `timeout(1)` is GNU and is not on a stock macOS, where it is `gtimeout` if
# coreutils is installed and absent otherwise -- hence the third branch, which
# does the same job with a watchdog. The watchdog's own output goes to /dev/null,
# and the caller sends this function's stdout to a file rather than a pipe: see
# the call site for why that second part is not tidiness.
GH_TIMEOUT="${ROMMSYNC_GATE_GH_TIMEOUT:-15}"

bounded() {
  local secs="$1"; shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$secs" env "$@"
    return $?
  fi
  if command -v gtimeout >/dev/null 2>&1; then
    gtimeout "$secs" env "$@"
    return $?
  fi
  local pid watch rc
  env "$@" &
  pid=$!
  ( sleep "$secs"; kill -TERM "$pid" 2>/dev/null ) >/dev/null 2>&1 &
  watch=$!
  wait "$pid"
  rc=$?
  kill "$watch" 2>/dev/null
  wait "$watch" 2>/dev/null
  return "$rc"
}

# A tagged, released v1 build. Two halves, because either alone is a release
# nobody can install: a tag whose major version is at least 1, reachable from
# `main`, and a published release carrying both assets. The second half needs
# the network, so it degrades to "unknown" rather than to "true".
repo_release() {
  local tags tag version ok=0
  tags="$(git -C "$REPO_ROOT" tag --list 'v[1-9]*' 2>/dev/null)"
  if [ -z "$tags" ]; then
    echo "    no v1 tag in this repository (git tag --list 'v[1-9]*' is empty)"
    echo "    VERSION is $(sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}' "$REPO_ROOT/VERSION" 2>/dev/null)"
    echo "    the machinery is built and tested -- ctest -R 'version|release' --"
    echo "    so what is missing is the decision to cut it, not the path: bump"
    echo "    VERSION and push a v1 tag on main, and .github/workflows/ci.yml"
    echo "    builds, packages, checksums and publishes it."
    return 1
  fi

  # Newest first, so a repo with v1.0.0 and v1.1.0 is judged on the latest.
  tag="$(echo "$tags" | sort -V | tail -1)"
  if ! git -C "$REPO_ROOT" merge-base --is-ancestor "$tag" origin/main 2>/dev/null \
     && ! git -C "$REPO_ROOT" merge-base --is-ancestor "$tag" main 2>/dev/null; then
    echo "    $tag is not reachable from main -- ci.yml refuses to publish it"
    ok=1
  fi
  version="$(sed -n '1{s/^[[:space:]]*//;s/[[:space:]]*$//;p;}' "$REPO_ROOT/VERSION" 2>/dev/null)"
  if [ "$tag" != "v$version" ]; then
    echo "    $tag disagrees with VERSION ($version) -- ctest -R version.tag is what says so on a tag push"
    ok=1
  fi

  # The one half of this row that needs the network. "gh is not installed" and
  # "gh could not answer" are one branch on purpose: both leave whether the tag
  # was published UNKNOWN, and an unknown is not a pass. A tag with no release
  # behind it installs nothing.
  if [ "$MODE" = "dry" ]; then
    echo "    whether $tag was published is not looked up in --dry"
    # Unknown only if nothing already known is wrong: a tag off main, or one that
    # disagrees with VERSION, is a finding this machine has already made, and
    # reporting it as "not looked up" would lose it.
    [ "$ok" -eq 0 ] && return 2
    return 1
  fi
  local view="" answer
  if command -v gh >/dev/null 2>&1; then
    # Into a FILE, not a pipe. A `gh` that spawns something of its own leaves
    # that child holding whatever stdout it inherited: killing `gh` on the
    # timeout would then still leave this function blocked on a pipe nobody is
    # writing to, which is the hang the timeout exists to prevent, wearing a
    # different hat. A file cannot be held open against us.
    answer="$(mktemp "${TMPDIR:-/tmp}/v1gate-gh.XXXXXX")"
    bounded "$GH_TIMEOUT" GH_PAGER=cat gh release view "$tag" --json isDraft,assets \
      >"$answer" 2>/dev/null
    view="$(cat "$answer" 2>/dev/null)"
    rm -f "$answer"
  fi
  if [ -z "$view" ]; then
    echo "    no published release for $tag is visible from here -- gh is absent"
    echo "    or could not answer, and a tag on its own installs nothing"
    # Unknown, not refused. Either way it does not open hardware.
    [ "$ok" -eq 0 ] && return 2
    return 1
  else
    case "$view" in
      *'"isDraft":true'*) echo "    the release for $tag is still a DRAFT"; ok=1 ;;
    esac
    case "$view" in
      *SHA256SUMS*) ;;
      *) echo "    the release for $tag carries no SHA256SUMS"; ok=1 ;;
    esac
    case "$view" in
      *".zip"*) ;;
      *) echo "    the release for $tag carries no zip"; ok=1 ;;
    esac
  fi

  [ "$ok" -eq 0 ] && echo "    $tag is published, on main, and agrees with VERSION"
  return "$ok"
}

row_repo() {
  case "$1" in
    backup)  repo_backup ;;
    release) repo_release ;;
    *)       return 0 ;;
  esac
}

# --- what a console row is waiting for ----------------------------------------
# These are not TODOs. They are the two things this repository is structurally
# unable to answer, written out so that the first console run has an agenda
# rather than a vibe, and so that nobody mistakes them for tests somebody could
# have written and did not.
row_note() {
  case "$1" in
    ssl)
      cat <<'NOTE'
    sysmodule/source/http/ssl_http_client.cpp is about twenty libnx calls that
    no test can reach; switch.builds proves they compile and link for aarch64
    and nothing proves they run. The emulator rung is unavailable here (Ryujinx
    needs prod.keys and a firmware dump, which hard rule 1 forbids), and it
    could not have retired the whole risk anyway: it stubs ImportServerPki and
    SetVerifyOption, so it settles the transport half and not the PKI half
    (docs/DEVELOPMENT.md#m0-1-the-measurement-and-the-decision).

    Run tlsprobe/ first -- a manually launched .nro, never an auto-boot
    sysmodule -- then a build carrying ssl_http_client.cpp. Three questions the
    probe does not answer and one run does:
      1. does sslConnectionPoll bound a read the way SslStream::Wait assumes,
         so a stall ends on http::Request's timeout and not on
         SslIoMode_Blocking's five minutes?
      2. does sslConnectionSetIoTimeout [16.0.0+] do anything? The probe never
         called it.
      3. who owns the descriptor when socketSslConnectionSetSocketDescriptor
         returns -1 with errno == ENOENT? If the service did not take it, every
         failed handshake leaks an fd against a handle_table_size of 64. The
         backend closes nothing and says so in a comment, because closing on a
         guess is a double close.
    The answers go in docs/DEVELOPMENT.md, beside the measurement they correct,
    and this row stops being a `console` row in that same commit. Nothing this
    script can be handed will tick it.
NOTE
      ;;
    media)
      cat <<'NOTE'
    Nothing in a repository can attest to this, and a checkbox that says
    otherwise is worse than none. Before M8-2 (#44):
      - a NAND backup exists, is verified, and is not on the console;
      - the SD in the console is a spare or an emuMMC, not the daily driver;
      - the RomM it points at is the disposable fixture collection, never a
        production library (CLAUDE.md hard rule 1).
    Also settled by the same first boot, and currently a target rather than a
    result: the compatibility line in scripts/release-notes.sh
    (ATMOSPHERE_TARGET) and the title id 0x4200000000524D53, which nothing has
    checked against an installed set (sysmodule/README.md).

    As with `ssl`: nothing this script can be handed ticks this row. It stops
    being a `console` row in the commit that records what was actually done,
    reviewed like any other change.
NOTE
      ;;
    *) : ;;
  esac
}

# --- CTest, and what counts as evidence ---------------------------------------

REGISTERED=""
load_registered() {
  [ -n "$REGISTERED" ] && return 0
  if [ ! -d "$BUILD_DIR" ]; then
    echo "v1-gate.sh: no build directory at $BUILD_DIR -- cmake -S . -B build first" >&2
    return 1
  fi
  REGISTERED="$(ctest --test-dir "$BUILD_DIR" -N 2>/dev/null | sed -n 's/^ *Test *#[0-9]*: //p')"
  [ -n "$REGISTERED" ] || {
    echo "v1-gate.sh: $BUILD_DIR registers no tests" >&2
    return 1
  }
  return 0
}

matching() { echo "$REGISTERED" | grep -E "$1" || true; }

# Failures and skips from the last `--run`, one name per line. A SKIPPED test is
# not evidence: rig.smoke skips when RomM is down and most of this gate depends
# on it, so a row containing one is HELD rather than PASS. Reading a skip as a
# pass is precisely the failure docs/TESTING.md's M0 gate was written against.
RAN_FAILED=""
RAN_SKIPPED=""
RAN_SEEN=""
# What the run exited with, or the word `transcript` when there was no run to
# exit -- quoting "ctest exited 0" for a recorded fixture would put a number in
# front of somebody that no ctest ever produced.
RAN_STATUS=0

# $ROMMSYNC_GATE_TRANSCRIPT is a seam: a file holding what a
# `ctest` run printed, used instead of running it. It exists so tests/test_v1_gate.sh
# can drive the classification below -- and in particular the rule that a SKIP is
# not a pass -- without a rig, a docker RomM and four minutes. Nothing else sets it.
# Results go into the file named by $2 rather than onto stdout, because the
# caller needs `RAN_STATUS` as well as the results, and a command substitution
# runs this in a subshell where an assignment to it goes nowhere. That is not a
# hypothetical: read through `$(run_groups ...)`, this function set RAN_STATUS
# faithfully and the caller saw the value it was initialised with, so a run that
# died reported "ctest exited 0" -- a number no ctest had produced.
run_groups() {
  local regex="$1" into="$2" out
  if [ -n "${ROMMSYNC_GATE_TRANSCRIPT:-}" ]; then
    out="$(cat "$ROMMSYNC_GATE_TRANSCRIPT")"
    RAN_STATUS="transcript"
  else
    out="$(ctest --test-dir "$BUILD_DIR" -R "$regex" --output-on-failure 2>&1)"
    RAN_STATUS=$?
  fi
  echo "$out" | awk '
    /Test +#[0-9]+:/ {
      for (i = 1; i <= NF; i++) if ($i ~ /^#[0-9]+:$/) { name = $(i + 1); break }
      if (name == "") next
      if ($0 ~ /Skipped/)      print "SKIP " name
      else if ($0 ~ /Passed/)  print "PASS " name
      else                     print "FAIL " name
      name = ""
    }' > "$into"
}

# --- evaluation ---------------------------------------------------------------

FAILING=0
HELD=0

evaluate_row() {
  local id="$1" kind="$2" claim="$3"
  local status="PASS" detail="" pattern hits missing="" bad="" skipped="" silent=""

  if [ "$kind" = "console" ]; then
    status="HELD"
  fi

  if [ "$kind" = "suite" ]; then
    while IFS= read -r pattern; do
      [ -n "$pattern" ] || continue
      hits="$(matching "$pattern")"
      if [ -z "$hits" ]; then
        missing="$missing $pattern"
        continue
      fi
      if [ "$MODE" = "run" ]; then
        local name
        while IFS= read -r name; do
          [ -n "$name" ] || continue
          case "$RAN_FAILED" in *"|$name|"*) bad="$bad $name" ;; esac
          case "$RAN_SKIPPED" in *"|$name|"*) skipped="$skipped $name" ;; esac
          # ...and a test that reported NOTHING. Reading "not in the failures"
          # as "passed" makes an aborted run -- ctest dying, a run interrupted
          # after thirty of two hundred tests -- come back as five green rows
          # over evidence nobody ever saw.
          case "$RAN_SEEN" in
            *"|$name|"*) ;;
            *) silent="$silent $name" ;;
          esac
        done <<EOF
$hits
EOF
      fi
    done <<EOF
$(row_tests "$id")
EOF
    if [ -n "$missing" ]; then
      status="FAIL"
      detail="$detail
    names a CTest group that is not registered:$missing"
    fi
    if [ -n "$bad" ]; then
      status="FAIL"
      detail="$detail
    failing:$bad"
    fi
    if [ -n "$silent" ]; then
      status="FAIL"
      detail="$detail
    named by this row and never reported a result:$silent
    $(if [ "$RAN_STATUS" = "transcript" ]; then
        echo "the recorded run in \$ROMMSYNC_GATE_TRANSCRIPT stops before them"
      else
        echo "the run did not finish (ctest exited $RAN_STATUS) -- rerun it"
      fi); a test that said nothing is not a test that passed"
    fi
    if [ "$MODE" = "dry" ] && [ "$status" != "FAIL" ]; then
      status="HELD"
      detail="$detail
    registered, and not run this invocation -- --dry reads the gate, it does not
    decide it. Run scripts/v1-gate.sh without --dry."
    fi
    if [ -n "$skipped" ] && [ "$status" != "FAIL" ]; then
      status="HELD"
      detail="$detail
    skipped, so it demonstrated nothing this run:$skipped
    a skip is not a pass -- start the rig (scripts/orca/compose.sh up -d) and run again"
    fi
  fi

  # `repo` predicates run for any row that has one, including a `suite` row.
  local repo_out repo_status
  repo_out="$(row_repo "$id")"
  repo_status=$?
  # A repo predicate that says no fails the row outright, even when the row was
  # already HELD for another reason. `backup` is a suite row WITH a predicate:
  # if its tests skipped because RomM is down -- the ordinary case -- a census
  # breach would otherwise print its text under a HELD heading and never be
  # counted, which is a save-overwrite path going unclassified in silence.
  if [ "$repo_status" -eq 2 ]; then
    [ "$status" = "FAIL" ] || status="HELD"
  elif [ "$repo_status" -ne 0 ]; then
    status="FAIL"
  fi
  if [ -n "$repo_out" ]; then
    detail="$detail
$repo_out"
  fi

  if [ "$status" = "HELD" ]; then
    detail="$detail
$(row_note "$id")"
  fi

  printf '[%s] %-9s %s\n' "$status" "$id" "$claim"
  [ -n "$detail" ] && echo "$detail" | sed '/^$/d'

  case "$status" in
    FAIL) FAILING=$((FAILING + 1)) ;;
    HELD) HELD=$((HELD + 1)) ;;
  esac
  return 0
}

# --audit is the CTest-facing mode, and it deliberately answers a NARROWER
# question than the gate: is every row still pointing at something that exists?
# The verdict -- does the gate pass -- is not a thing `ctest` should ever report
# green on, because it does not, and will not until a console has run the probe.
audit() {
  local ok=0 id kind claim pattern
  load_registered || return 1
  while IFS='|' read -r id kind claim; do
    [ -n "$id" ] || continue
    case "$kind" in
      suite|repo|console) ;;
      *) echo "audit: row '$id' has an unknown kind '$kind'"; ok=1; continue ;;
    esac
    [ -n "$claim" ] || { echo "audit: row '$id' has no claim"; ok=1; }
    if [ "$kind" = "suite" ]; then
      local any=0
      while IFS= read -r pattern; do
        [ -n "$pattern" ] || continue
        any=1
        if [ -z "$(matching "$pattern")" ]; then
          echo "audit: row '$id' names '$pattern', which matches no registered test"
          ok=1
        fi
      done <<EOF
$(row_tests "$id")
EOF
      [ "$any" -eq 1 ] || { echo "audit: suite row '$id' names no tests"; ok=1; }
    fi
    if [ "$kind" = "console" ]; then
      [ -n "$(row_note "$id")" ] || {
        echo "audit: console row '$id' does not say what would settle it"; ok=1; }
    fi
  done <<EOF
$(gate_rows)
EOF

  # The census is machinery too: it is what makes the `backup` row's "every"
  # mean something, so it is audited on every ctest rather than only when
  # somebody runs the gate by hand.
  local census
  census="$(repo_backup)" || { echo "$census"; ok=1; }

  [ "$ok" -eq 0 ] && echo "ok: $(gate_rows | grep -c .) gate rows, each pointing at something that exists"
  return "$ok"
}

case "$MODE" in
  rows)
    gate_rows
    exit 0
    ;;
  audit)
    audit
    exit $?
    ;;
esac

load_registered || exit 1

if [ "$MODE" = "run" ]; then
  # One CTest invocation over the union of every suite row, rather than one per
  # row: the groups overlap heavily (`harness.resume` is evidence for two rows)
  # and running a test twice would double the rig's work to learn nothing.
  union=""
  while IFS='|' read -r id kind claim; do
    [ "$kind" = "suite" ] || continue
    while IFS= read -r pattern; do
      [ -n "$pattern" ] || continue
      union="${union:+$union|}$pattern"
    done <<EOF
$(row_tests "$id")
EOF
  done <<EOF
$(gate_rows)
EOF
  if [ -n "${ROMMSYNC_GATE_TRANSCRIPT:-}" ]; then
    echo "Reading a recorded run: $ROMMSYNC_GATE_TRANSCRIPT"
  else
    echo "Running the gate's evidence: ctest -R '$union'"
  fi
  echo
  results_file="$(mktemp "${TMPDIR:-/tmp}/v1gate-run.XXXXXX")"
  run_groups "$union" "$results_file"
  results="$(cat "$results_file")"
  rm -f "$results_file"
  RAN_FAILED="|$(echo "$results" | sed -n 's/^FAIL //p' | tr '\n' '|')"
  RAN_SKIPPED="|$(echo "$results" | sed -n 's/^SKIP //p' | tr '\n' '|')"
  RAN_SEEN="|$(echo "$results" | sed -n 's/^[A-Z]* //p' | tr '\n' '|')"
fi

echo "The v1 gate -- M8-1 (#43). Hardware work begins when every row is PASS."
echo

while IFS='|' read -r id kind claim; do
  [ -n "$id" ] || continue
  evaluate_row "$id" "$kind" "$claim"
  echo
done <<EOF
$(gate_rows)
EOF

if [ "$FAILING" -gt 0 ]; then
  echo "GATE FAILS: $FAILING row(s) failing, $HELD held. Nothing on hardware."
  exit 1
fi
if [ "$HELD" -gt 0 ]; then
  echo "GATE HELD: $HELD row(s) need a console or a person. Nothing on hardware."
  exit 3
fi
echo "GATE PASSES. M8-2 (#44) may begin, on a spare SD, NRO first."
exit 0

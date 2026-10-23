#!/usr/bin/env bash
# The save-overwrite protocol on lazyfs, cut at every call (#250).
#
#   test_lazyfs.sh DRIVER        DRIVER is the built tests/test_lazyfs
#   test_lazyfs.sh --workflow    .github/workflows/lazyfs.yml runs this nightly,
#                                at a pinned lazyfs, unable to skip
#
# tests/crash_disk.hpp models what a card that does not journal keeps after a
# power cut; this is the check on the model, against a real page cache that can
# be told to forget. lazyfs (github.com/dsrhaslab/lazyfs) holds every write in
# its own cache until an fsync, and on command drops what was never synced
# (`lazyfs::clear-cache`), or persists part of one write, or some of a run of
# them, and dies (`torn-op`, `torn-seq`).
#
# Two overwrites run on a lazyfs mount: `BackUpFirst` then `WriteAtomically` (a
# restore, a state), and a download's order -- the body staged in pieces,
# backed up, `CommitStaged`. For every call each makes to `io::Disk` there is a
# run that cuts the power in front of that call: lazyfs drops its unsynced data
# and the driver dies with it. Then lazyfs is unmounted, and what reached its
# root is swept as the next boot would sweep it and judged as `crash_disk.hpp`
# judges a replay: the save is its old bytes or its new ones, and the new ones
# only beside a backup of the old. A cut after the last call must find the new
# bytes, because the overwrite had returned. A download whose body is renamed
# into place unsynced must be caught at some cut -- the check's own check. Then
# the torn faults, each with lazyfs dying after it: the first or second half of
# one write, and the first or second of a run of writes, to each file a save's
# bytes pass through on the way.
#
# **Linux and FUSE only**, so everywhere else this skips with the reason, and so
# does a Linux with no lazyfs built: set LAZYFS to its executable
# (lazyfs/build/lazyfs in a checkout built with its own build.sh). The nightly
# .github/workflows/lazyfs.yml builds one at a pinned commit and sets
# ROMMSYNC_REQUIRE_LAZYFS=1, which makes a skip a failure: in that job nothing
# can be missing.
#
# What lazyfs does NOT model is as much a finding as what it does: it passes
# metadata -- create, rename, unlink -- straight to the filesystem under it, so
# a name is durable the moment it is made. crash_disk.hpp is the harsher of the
# two there (a directory's unsynced names may be lost), and nothing here can
# check that assumption. Flying writes and a sync that lies are beyond both.
set -uo pipefail

DRIVER="${1:-}"

fail() { echo "FAIL: $*" >&2; exit 1; }

# The job that runs this exists, runs nightly and on demand, runs this script,
# cannot skip, and builds lazyfs at a commit rather than at whatever its main
# is that night. Needs nothing but the checkout, so it never skips.
if [ "$DRIVER" = --workflow ]; then
  WORKFLOW="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.github/workflows/lazyfs.yml"
  [ -f "$WORKFLOW" ] || fail "no .github/workflows/lazyfs.yml; nothing runs this (#250)"
  body="$(grep -v '^[[:space:]]*#' "$WORKFLOW")"
  grep -qE "cron: *'[^']+'" <<<"$body" || fail "lazyfs.yml runs on no schedule"
  grep -q 'workflow_dispatch:' <<<"$body" || fail "lazyfs.yml cannot be run on demand"
  grep -q 'tests/test_lazyfs.sh' <<<"$body" || fail "lazyfs.yml does not run tests/test_lazyfs.sh"
  grep -q 'ROMMSYNC_REQUIRE_LAZYFS=1' <<<"$body" ||
    fail "lazyfs.yml lets the check skip, and a skip there would read as green"
  grep -qE 'LAZYFS_REF: *[0-9a-f]{40}$' <<<"$body" ||
    fail "lazyfs.yml does not pin lazyfs to a commit"
  echo "ok: lazyfs.yml runs this nightly at a pinned lazyfs, and cannot skip"
  exit 0
fi
skip() {
  if [ -n "${ROMMSYNC_REQUIRE_LAZYFS:-}" ]; then
    fail "$* (and ROMMSYNC_REQUIRE_LAZYFS is set, so this job cannot skip)"
  fi
  echo "skip: $*"
  exit 77
}

[ -x "$DRIVER" ] || fail "usage: $0 DRIVER (the built tests/test_lazyfs)"
[ "$(uname -s)" = Linux ] || skip "lazyfs is a FUSE filesystem for Linux, and this is $(uname -s)"
[ -n "${LAZYFS:-}" ] || skip "LAZYFS is not set to a built lazyfs (.github/workflows/lazyfs.yml builds one)"
[ -x "$LAZYFS" ] || skip "LAZYFS=$LAZYFS is not an executable"
[ -c /dev/fuse ] || skip "no /dev/fuse"
command -v fusermount3 >/dev/null || skip "no fusermount3 (the fuse3 package)"
command -v mountpoint >/dev/null || skip "no mountpoint (util-linux)"

WORK="$(mktemp -d)"
ROOT="$WORK/root"
MNT="$WORK/mnt"
FIFO="$WORK/faults.fifo"
DONE="$WORK/done.fifo"
CONFIG="$WORK/lazyfs.toml"
LOG="$WORK/lazyfs.log"
LAZY_PID=""
KEEPER_PID=""

unmount() {
  # Asked of /proc/mounts rather than `mountpoint`, which answers no for the
  # mount a crashed lazyfs leaves -- "transport endpoint is not connected" --
  # and that is the one most in need of taking down.
  if grep -q " $MNT fuse" /proc/mounts 2>/dev/null; then
    fusermount3 -u "$MNT" 2>/dev/null || fusermount3 -uz "$MNT" 2>/dev/null || true
  fi
  if [ -n "$LAZY_PID" ]; then
    # A lazyfs that crashed itself is gone already; one that was unmounted
    # leaves on its own. Neither should outlive its run.
    local waited=0
    while kill -0 "$LAZY_PID" 2>/dev/null && [ "$waited" -lt 50 ]; do
      sleep 0.1
      waited=$(( waited + 1 ))
    done
    kill -9 "$LAZY_PID" 2>/dev/null || true
    wait "$KEEPER_PID" 2>/dev/null || true
    LAZY_PID=""
  fi
}
cleanup() { unmount; rm -rf "$WORK"; }
trap cleanup EXIT

# fresh -- an empty root holding the card before the overwrite, and an empty
# mount point.
fresh() {
  unmount
  rm -rf "$ROOT" "$MNT"
  mkdir -p "$ROOT" "$MNT"
  "$DRIVER" seed "$ROOT" || fail "could not seed the card"
}

# mount_lazyfs [INJECTIONS] -- lazyfs over $ROOT at $MNT, with the TOML text
# INJECTIONS appended to its configuration.
mount_lazyfs() {
  rm -f "$FIFO" "$DONE"
  mkfifo "$FIFO" "$DONE"
  cat >"$CONFIG" <<EOF
[faults]
fifo_path="$FIFO"
fifo_path_completed="$DONE"
[cache]
apply_eviction=false
[cache.simple]
custom_size="64mb"
blocks_per_page=1
[filesystem]
log_all_operations=false
logfile=""
${1:-}
EOF
  # Started from a subshell that waits for it and answers 0, with its own
  # stderr dropped: lazyfs aborts on its way out of every unmount, and the
  # shell's "Aborted" for that says nothing about a run. Its pid comes back
  # through a file, because the subshell's is not the one to watch.
  rm -f "$WORK/lazyfs.pid"
  ( "$LAZYFS" "$MNT" --config-path "$CONFIG" -o modules=subdir -o subdir="$ROOT" -f \
      >>"$LOG" 2>&1 &
    echo $! >"$WORK/lazyfs.pid"
    wait
    exit 0 ) 2>/dev/null &
  KEEPER_PID=$!
  local tries=0
  until [ -s "$WORK/lazyfs.pid" ] || [ "$tries" -gt 50 ]; do
    sleep 0.1
    tries=$(( tries + 1 ))
  done
  LAZY_PID="$(cat "$WORK/lazyfs.pid" 2>/dev/null)"
  [ -n "$LAZY_PID" ] || fail "lazyfs did not start"
  tries=0
  until mountpoint -q "$MNT"; do
    tries=$(( tries + 1 ))
    if [ "$tries" -gt 100 ] || ! kill -0 "$LAZY_PID" 2>/dev/null; then
      tail -20 "$LOG" >&2
      fail "lazyfs did not mount $MNT"
    fi
    sleep 0.1
  done
}

# judge WHAT [new] -- lazyfs's root after the run, as the next boot sees it.
judge() {
  local what="$1" out
  shift
  if ! out="$("$DRIVER" judge "$ROOT" "$@" 2>&1)"; then
    echo "$out" >&2
    tail -20 "$LOG" >&2
    fail "$what: $out"
  fi
}


# sweep PROTOCOL -- run it whole to count its calls to `io::Disk`, then once
# for each with the power cut in front of it, and once after the last.
sweep() {
  local protocol="$1" out calls cut status
  fresh
  mount_lazyfs
  out="$("$DRIVER" run "$MNT" 0 "$FIFO" "$DONE" "$protocol" 2>&1)" ||
    { echo "$out" >&2; fail "$protocol did not complete on lazyfs with no fault armed"; }
  calls="$(sed -n 's/^calls \([0-9][0-9]*\)$/\1/p' <<<"$out")"
  [ -n "$calls" ] && [ "$calls" -gt 0 ] ||
    fail "the driver did not say how many calls $protocol made: $out"
  unmount
  judge "$protocol with no fault" new

  for cut in $(seq 1 $(( calls + 1 ))); do
    fresh
    mount_lazyfs
    out="$("$DRIVER" run "$MNT" "$cut" "$FIFO" "$DONE" "$protocol" 2>&1)"
    status=$?
    [ "$status" -eq 10 ] ||
      { echo "$out" >&2; fail "$protocol, cut $cut: the driver exited $status, not at the cut"; }
    unmount
    if [ "$cut" -gt "$calls" ]; then
      judge "$protocol, a power cut after it returned" new
    else
      judge "$protocol, a power cut in front of call $cut of $calls"
    fi
  done
  echo "ok: $protocol -- a power cut in front of each of its $calls calls, and after the last," \
       "leaves old or new beside a backup"
}

# torn PROTOCOL TARGET KIND PERSIST -- lazyfs persists part PERSIST of the first
# write to TARGET (torn-op, split in two), or write PERSIST of the first run of
# writes to it (torn-seq), and dies. The run's own exit status is not judged --
# every call after the crash fails -- only what reached the root.
torn() {
  local protocol="$1" target="$2" kind="$3" persist="$4" fault waited
  if [ "$kind" = torn-op ]; then
    fault="type=\"torn-op\"
file=\"$ROOT/$target\"
occurrence=1
parts=2
persist=[$persist]"
  else
    fault="type=\"torn-seq\"
op=\"write\"
file=\"$ROOT/$target\"
occurrence=1
persist=[$persist]"
  fi
  fresh
  mount_lazyfs "[[injection]]
$fault"
  "$DRIVER" run "$MNT" 0 "$FIFO" "$DONE" "$protocol" >/dev/null 2>&1
  # The fault has to have fired -- lazyfs gone -- or the judge below reads an
  # overwrite nothing tore, and passes for the wrong reason.
  waited=0
  while kill -0 "$LAZY_PID" 2>/dev/null && [ "$waited" -lt 20 ]; do
    sleep 0.1
    waited=$(( waited + 1 ))
  done
  if kill -0 "$LAZY_PID" 2>/dev/null; then
    tail -20 "$LOG" >&2
    fail "$protocol, $kind persist=[$persist] on $target never fired: lazyfs is still running"
  fi
  unmount
  judge "$protocol, $kind persist=[$persist] on $target"
  echo "ok: $protocol, $kind persist=[$persist] on $target leaves old or new beside a backup"
}

# catches PROTOCOL -- the same sweep over a protocol that is wrong, which the
# judge has to refuse at some cut. A check that passed it would pass anything:
# crash_model's own first three cases, asked of lazyfs.
catches() {
  local protocol="$1" out calls cut caught=""
  fresh
  mount_lazyfs
  out="$("$DRIVER" run "$MNT" 0 "$FIFO" "$DONE" "$protocol" 2>&1)" ||
    { echo "$out" >&2; fail "$protocol did not complete on lazyfs with no fault armed"; }
  calls="$(sed -n 's/^calls \([0-9][0-9]*\)$/\1/p' <<<"$out")"
  unmount
  for cut in $(seq 1 "${calls:-0}"); do
    fresh
    mount_lazyfs
    "$DRIVER" run "$MNT" "$cut" "$FIFO" "$DONE" "$protocol" >/dev/null 2>&1
    unmount
    if ! out="$("$DRIVER" judge "$ROOT" 2>&1)"; then
      caught="call $cut of $calls: $(head -1 <<<"$out")"
      break
    fi
  done
  [ -n "$caught" ] ||
    fail "$protocol survived every cut on lazyfs; a check that passes it would pass anything"
  echo "ok: $protocol is caught -- a power cut in front of $caught"
}

sweep overwrite
sweep download
catches unsynced

# Each tear twice: the first part landed and not the second, and the second and
# not the first -- a write that reached the card out of order. `torn-seq` needs a
# run of two writes or more, so it is aimed at what is written in pieces: the
# backup, copied 4 KiB at a time, and a download's staged body. The save's own
# staging file under `WriteAtomically` is one write, which only `torn-op` splits.
backup="config/rommsync/.backup/$("$DRIVER" backup).tmp"
for persist in 1 2; do
  torn overwrite "retroarch/saves/crash.srm.tmp" torn-op "$persist"
  torn overwrite "$backup" torn-op "$persist"
  torn overwrite "$backup" torn-seq "$persist"
  torn download "retroarch/saves/crash.srm.tmp.part" torn-op "$persist"
  torn download "retroarch/saves/crash.srm.tmp.part" torn-seq "$persist"
done

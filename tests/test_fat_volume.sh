#!/usr/bin/env bash
# A real FAT32 volume, for `test_card fat32` (M9-13, #202).
#
# The 4 GiB ceiling and a genuinely full card are the two things `faulty::Disk`
# can only pretend. This makes a 64 MiB FAT32 image -- the ceiling is reached by
# a seek, so nothing close to 4 GiB is ever written -- mounts it, and hands the mount to the test binary,
# which checks that the host backend reads it as FAT32, that a byte making a file
# 4 GiB is EFBIG, that filling it is ENOSPC, and that a download onto it is
# `kDiskFull`.
#
# Where it runs (docs/TESTING.md):
#   macOS  -- `hdiutil`, which needs no privileges. Always runs.
#   Linux  -- `mkfs.vfat` plus a loop mount, which needs root: it runs when
#             `sudo -n` is allowed (GitHub's runners allow it) and dosfstools is
#             installed (ci.yml does not install it yet), and otherwise SKIPS,
#             saying which was missing.
# A skip is exit 77, which ctest reports as Skipped rather than Passed -- a check
# that silently did not run is worse than one that says so.
set -euo pipefail

binary="${1:?usage: test_fat_volume.sh <test_card binary>}"
skip() { echo "SKIP: $*" >&2; exit 77; }

work="$(mktemp -d "${TMPDIR:-/tmp}/rommsync-fat.XXXXXX")"
mount_point="$work/mnt"
mkdir -p "$mount_point"
attached=""
cleanup() {
  if [[ -n "$attached" ]]; then
    case "$(uname -s)" in
      Darwin) hdiutil detach -quiet "$mount_point" || hdiutil detach -quiet -force "$mount_point" || true ;;
      Linux) sudo -n umount "$mount_point" || true ;;
    esac
  fi
  rm -rf "$work"
}
trap cleanup EXIT

case "$(uname -s)" in
  Darwin)
    command -v hdiutil >/dev/null || skip "hdiutil is not on PATH"
    # 64 MiB: big enough to be FAT32 rather than FAT16, small enough to fill in
    # a second.
    hdiutil create -quiet -size 64m -fs "MS-DOS FAT32" -volname RSXFAT -layout NONE \
      -type UDIF "$work/card" >/dev/null
    hdiutil attach -quiet -nobrowse -mountpoint "$mount_point" "$work/card.dmg" >/dev/null
    attached=1
    ;;
  Linux)
    command -v mkfs.vfat >/dev/null || skip "mkfs.vfat is not installed (dosfstools)"
    sudo -n true 2>/dev/null || skip "a loop mount needs root, and sudo -n is not allowed here"
    truncate -s 64M "$work/card.img"
    mkfs.vfat -F 32 -n RSXFAT "$work/card.img" >/dev/null
    sudo -n mount -o loop,uid="$(id -u)",gid="$(id -g)" "$work/card.img" "$mount_point" ||
      skip "the loop mount was refused"
    attached=1
    ;;
  *)
    skip "no way to mount a FAT32 image on $(uname -s)"
    ;;
esac

"$binary" fat32 "$mount_point"

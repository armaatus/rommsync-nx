---
name: save-safety
description: >-
  Apply whenever writing, overwriting, restoring, syncing, merging or deleting a
  Nintendo Switch save file, a save archive, or anything under a save directory --
  including download-to-file paths that land on an existing save, conflict
  resolution, and rollback. Triggers on save, savedata, save slot, restore,
  overwrite, sync conflict, backup.
---

# Never destroy a save

This is hard rule 2 in CLAUDE.md, and it is the one guarantee standing between a
bug in this repo and a player's destroyed progress. It is a control, not advice:
a reviewer will reject a path that breaks it.

## The rule

Every path that overwrites a save file **writes a backup first, and writes
atomically**. Both halves, in that order. There is no exception for "the file is
empty", "the server copy is newer", or "the user confirmed".

## What that means in code

1. **Back up before you touch the original.** The backup is complete and closed
   (fsync'd, renamed into place) before the write begins. A backup written
   concurrently with the overwrite protects nothing.
2. **Write atomically.** Write to a temporary file beside the target, fsync it,
   then `rename()` over the target. Never open the target for truncation. A
   process killed mid-write must leave either the old file or the new one, never
   a half of each.
3. **Fail closed.** If the backup cannot be written -- no space, no permission,
   no directory -- the overwrite does not happen. Report the reason; do not
   proceed "best effort".
4. **The server is the source of truth for conflicts** (hard rule 3), but that
   decides *which* bytes win, never whether the local copy is backed up first.

## Deleting a backup

A backup is deleted only when its bytes are provably somewhere else: a
`content_hash` RomM holds for the rom, a state row `state.db` proves RomM still
holds unmoved (#330), or a newer identical backup. The one exception is the state
budget (`[backup] max_state_mib`, #340). It deletes save-**state** backups with no
second copy, oldest first, beyond the newest `keep_states` of each state, and only
those `backup::StateBackupsOf` names from `state.db`. Never extend it to a save's
backup, never let it take a state's newest, and never decide "this is a state's"
from a file name alone.

## Proving it

An untested guarantee is not one. A change to any save-writing path needs a test
that fails without the change, in the shape docs/SYNC_PROTOCOL.md describes:

- the backup exists and matches the pre-write bytes,
- an interruption between the backup and the rename leaves the original intact,
- a failed backup aborts the overwrite.

See [docs/SYNC_PROTOCOL.md](../../../docs/SYNC_PROTOCOL.md) for the protocol this
sits inside.

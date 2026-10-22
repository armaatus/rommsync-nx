// What a negotiation is about (#241): the roms on the card, in chunks RomM
// accepts, and a rom just downloaded asked about on its own.
//
// An unscoped negotiation offers every server save this device has no history
// for, across the whole library -- so a console that holds ten games is offered
// the saves of a thousand, and places the ones it can (#237) beside roms it does
// not have. `rom_ids` is RomM's answer to that (5.3+): only the roms named, plus
// any rom the request sends a save for, are offered. It is read-only -- a rom
// left out is outside the negotiation, never a deletion -- so a scope that is
// too narrow costs a first-time restore, and never a save.
//
// Two things here are not what an obvious reading produces:
//
//   - **Every save goes in the chunk that names its rom.** RomM widens a chunk's
//     scope by the roms of the saves it carries. A save sent beside another
//     chunk's roms would leave its own rom's chunk without it, and that chunk
//     would offer the server's copy as a download over the very file the other
//     chunk is uploading. So the scope always includes the saves' roms, each
//     rom is in exactly one chunk, and its saves travel with it.
//
//   - **A freshly downloaded rom is a negotiation of its own**, with
//     `restore_unlisted`. RomM reads a save this device once synced and no
//     longer lists as one the user deleted, and offers nothing -- right for the
//     card at large, wrong for a rom whose save directory is empty because the
//     rom has only just arrived. The flag is per request, so it is sent only on
//     a request scoped to those roms (`SyncNegotiatePayload::restore_unlisted`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::sync {

/// One negotiation's worth of scope.
struct ScopeChunk {
  /// At most `kMaxNegotiateRomIds`, ascending, positive, no repeats. Empty is a
  /// real scope -- nothing on the card -- and is sent as `[]`, not left out.
  std::vector<std::int64_t> rom_ids;

  /// Indexes into the saves `ChunkScope` was given: the ones sent in this
  /// negotiation, in their original order.
  std::vector<std::size_t> saves;

  /// Negotiate with `restore_unlisted`: these are roms just downloaded.
  bool restore = false;
};

/// Split a tick's scope into negotiations RomM accepts.
///
/// The scope is `on_card` and every save's rom, less `restore`; it is chunked by
/// `kMaxNegotiateRomIds` in ascending order, and `restore` follows in chunks of
/// its own. Ids that are not positive are dropped -- RomM refuses the whole body
/// over one -- except that a save is never dropped: one whose rom id is not
/// positive rides in the first chunk, where `Validate` refuses it by name as it
/// always has.
///
/// Always at least one chunk, so a tick always negotiates: that is what tells a
/// console RomM switched sync off, and what the session accounting hangs on.
std::vector<ScopeChunk> ChunkScope(const std::vector<ClientSaveState>& saves,
                                   const std::vector<std::int64_t>& on_card,
                                   const std::vector<std::int64_t>& restore);

/// The roms the card holds, as far as the folder map can tell.
struct RomsOnCard {
  /// Ascending, no repeats.
  std::vector<std::int64_t> rom_ids;

  /// Folders that held more than `fs::kMaxDirectoryEntries` entries, so only the
  /// first ones by name were read. A rom past the bound is not in the scope: no
  /// save of it is lost, but none is restored for it either, which is worth a
  /// log line.
  std::vector<std::string> truncated;
};

/// The roms in `index` that have a file -- or, for a multi-file rom, a folder --
/// of their name in one of their platform's mapped `roms` folders.
///
/// The folder map is the definition, deliberately: it is where downloads land
/// and where the overlay already looks for "on the card" (`lists::Service`), so
/// a rom this client downloaded is always found, and a user who keeps roms
/// elsewhere maps that folder (docs/CONFIG.md). The name is matched as the save
/// scan matches one (`RomIndex::Find`, scoped to the platform the folder is
/// mapped for), and a name that is ambiguous or unknown counts for nothing.
///
/// One listing per distinct folder, never recursive. Nothing is hashed: being
/// wrong costs a first-time restore, not a save.
RomsOnCard FindRomsOnCard(const config::Config& config, const roms::RomIndex& index,
                          fs::FileSystem& files);

}  // namespace rommsync::sync

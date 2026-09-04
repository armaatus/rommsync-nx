// What the client *tells* RomM it has: `SyncNegotiatePayload` and the
// `ClientSaveState` entries under it.
//
// `auth.hpp` types the answers RomM sends; this types the one request body the
// whole sync loop hangs off. Every field name below is pinned to
// server/contract/romm-openapi-5.2.0.json (`ClientSaveState`) and was sent to a
// live 5.2.0 by server/probe_contract.py, whose replies are committed under
// server/contract/captures/. `sync.payload` re-reads that snapshot on every run,
// so a RomM that renames or re-types a field goes red here rather than on a
// console.
//
// Three of the seven fields are not what an obvious guess produces, and each
// wrong guess fails *quietly* -- the server takes the body, plans something
// reasonable-looking, and the client loses saves to it:
//
//   - `content_hash`, not `hash`, and an **MD5**: roms carry sha1/md5/crc, saves
//     are compared on MD5 alone. A SHA1 here matches nothing, so every
//     unchanged save negotiates as changed, forever.
//   - `file_size_bytes`, not `size`.
//   - `slot` is the pairing key with `rom_id`. A `null` slot means "archival,
//     manual upload" and is never paired with a slotted server save, so it
//     negotiates as `upload` on every tick even when the identical bytes are
//     already there.
//
// So nothing here is built by concatenation and nothing is sent unchecked:
// `EncodeNegotiateRequest` refuses a save it cannot express faithfully and
// names the field, on the same reasoning as `json::Reader` -- a body the server
// accepts but reads as a different save is a bug that surfaces hours later, as
// a save that came back wrong.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rommsync/json.hpp"

namespace rommsync::sync {

/// An instant as RomM compares it: UTC, seconds.
///
/// `std::chrono::system_clock::from_time_t` is how an mtime off `stat` becomes
/// one, on Horizon and on the host alike.
using Timestamp = std::chrono::system_clock::time_point;

/// `2026-09-04T11:36:27Z` -- RFC 3339, UTC, whole seconds.
///
/// Sub-second precision is dropped *downwards*, never rounded: RomM stores these
/// at second granularity and compares strictly greater-than, so rounding a
/// 11:36:27.9 mtime up to :28 would claim a file is newer than it is and hand it
/// an arbitration it should have lost.
///
/// The offset is spelled `Z` rather than `+00:00`. RomM sends the latter and
/// accepts both; `Z` is what the probe sent and what the docs quote.
///
/// Empty when `when` is outside `[kMinTimestampSeconds, kMaxTimestampSeconds]`,
/// which no valid save reaches -- `Validate` refuses those first, so an empty
/// string cannot become a field in a request body.
std::string FormatTimestamp(Timestamp when);

/// `when` as whole seconds since the Unix epoch, rounded towards the past.
///
/// The bounds below are seconds rather than `Timestamp`s because
/// `system_clock::time_point` cannot be relied on to *hold* the upper one: its
/// tick is implementation-defined, and a nanosecond tick runs out in 2262.
std::int64_t UnixSeconds(Timestamp when);

/// The earliest instant a save may claim, one second past the epoch.
///
/// The epoch itself is excluded on purpose. It is what a console with an unset
/// clock reports and what a default-constructed `ClientSaveState` holds, and
/// both mean "nobody knows when this file changed" -- which the server would
/// read as "very old" and answer with a `download` over a save that may be the
/// only copy. Refusing it costs one save one tick; accepting it costs the save.
inline constexpr std::int64_t kMinTimestampSeconds = 1;

/// `9999-12-31T23:59:59Z`, the last instant `FormatTimestamp` can spell.
inline constexpr std::int64_t kMaxTimestampSeconds = 253402300799;

/// Hex digits in a `content_hash`. MD5, so 32 -- a SHA1's 40 is the mistake this
/// number exists to catch.
inline constexpr std::size_t kContentHashDigits = 32;

/// One local save, as `POST /api/sync/negotiate` reads it.
///
/// Required by the snapshot: `rom_id`, `file_name`, `updated_at`,
/// `file_size_bytes`. The other three are `T | null`, and their `null` means
/// something in each case -- see the field comments. They are `optional` rather
/// than empty-string-means-absent because "" and `null` are different values to
/// the server, and only one of them is a value.
struct ClientSaveState {
  /// The rom this save belongs to, matched locally
  /// (SYNC_PROTOCOL.md#step-0--matching-local-files-to-roms). RomM's ids are
  /// positive; a `0` here is an unmatched save, which has nothing to negotiate.
  std::int64_t rom_id = 0;

  /// The save file's own name, `Game (USA).srm` -- a name, never a path. The
  /// server joins it into a storage path, so a separator in it is a client
  /// asking the server to write somewhere else.
  std::string file_name;

  /// The pairing key, with `rom_id`. Pick a stable one (`autosave`) and keep it:
  /// changing it between ticks makes the same file a new save every time.
  ///
  /// `null` is legitimate and means archival/manual-upload, which never pairs
  /// with a slotted server save -- so a null-slot save uploads on every tick.
  /// That is a deliberate choice, not a default to fall into, which is why an
  /// empty string is refused rather than quietly treated as one or the other.
  std::optional<std::string> slot;

  /// Which emulator wrote it (`retroarch`, `tico`), when that is known.
  std::optional<std::string> emulator;

  /// **MD5** of the file's bytes, 32 lowercase hex digits. `null` when it has
  /// not been hashed, which the server reads as "cannot compare content" and
  /// falls back to timestamps for.
  ///
  /// Lowercase because the server compares the string it stored, and RomM
  /// stores `hexdigest()`. An uppercase digest of the same bytes matches
  /// nothing and makes every unchanged save look changed -- the same failure a
  /// SHA1 produces, from a subtler cause, so `Validate` refuses both.
  std::optional<std::string> content_hash;

  /// The local file's mtime, in UTC. What the server arbitrates on when the
  /// hashes cannot settle it.
  Timestamp updated_at{};

  /// The file's size in bytes. Reported, not enforced: the server does not
  /// reject a mismatch, so this is a hint for its UI rather than a checksum.
  std::int64_t file_size_bytes = 0;
};

/// The whole `POST /api/sync/negotiate` body.
struct SyncNegotiatePayload {
  /// Which device is syncing. The snapshot marks it optional -- a device-bound
  /// client token identifies the device on its own -- but send it anyway: it is
  /// the id the token response already carried, and being explicit is what
  /// keeps a token that is *not* device-bound from negotiating as nobody.
  std::optional<std::string> device_id;

  /// One entry per local save. Legitimately empty: a negotiation that reports
  /// nothing is how a client asks "what am I missing?", and it is the read-only
  /// shape the probe uses (docs/API_CONTRACT.md).
  std::vector<ClientSaveState> saves;
};

/// A request body, or the reason there isn't one. `body` is empty on failure and
/// must not be sent -- check `ok()`.
struct Encoded {
  std::string body;
  json::Error error;
  bool ok() const { return error.ok(); }
};

/// Everything wrong with one save that would make its negotiation mean
/// something other than what the client meant.
///
/// `error.field` is the JSON field name (`content_hash`), so a caller can say
/// which one; `EncodeNegotiateRequest` prefixes it with the entry's index.
/// Values are never quoted back, matching `json::Error`.
json::Error Validate(const ClientSaveState& save);

/// The request body for `POST /api/sync/negotiate`, or a named error.
///
/// Every save is validated first and the first failure stops the encode: a body
/// that is missing the one save the tick was about is worse than no body, since
/// the plan that comes back looks complete.
Encoded EncodeNegotiateRequest(const SyncNegotiatePayload& payload);

}  // namespace rommsync::sync

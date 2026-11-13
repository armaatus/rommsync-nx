// Which RomM the records on the card were written against (M11-10, #294).
//
// `state.db`, `play.db` and `queue.json` are all keyed by RomM's rom ids, and a
// rom id means something only on the server that issued it: rom 12 on one RomM
// may be another game on the next, or nothing. So a record written against one
// server is never read against another. What a console does when its `[server]
// url` moves -- by the overlay, or by a hand edit while it was off -- is set the
// old records aside as `<file>.<old server>` and start again, which is a first
// sync: every save negotiated with no history, and every overwrite it plans
// backed up first, as any first sync's is (hard rule 2).
//
// ## What a server is
//
// Its normalised `[server] url`, scheme and host lower-cased. RomM 5.4 exposes
// no instance id -- `GET /api/heartbeat` carries a version and the server's
// configuration, nothing that names the installation -- so two RomMs behind one
// URL are one server to this client, and one RomM reached by two URLs is two.
// The second costs a first sync, which backs up before it overwrites; the first
// is the one case this cannot see.
//
// ## Two ways a record is bound
//
// `state.db` carries its server in its own header (state_db.hpp), because the
// tick that writes it can be running against the old server when the URL moves:
// a header written by that tick says which server its rows came from, and the
// next tick sets it aside. `play.db` and `queue.json` are resident -- read once
// at boot, then rewritten from memory -- so the engine decides their server at
// two moments it owns, and one marker beside them, `server.id`, records the
// answer (`BindRecords`). Neither file's format moved, so a downgrade still
// reads both.
//
// Hard rule 4: standard headers and `rommsync/` only.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::binding {

/// The marker `BindRecords` keeps beside the resident records, relative to
/// `sdmc:/config/rommsync/`.
inline constexpr const char* kMarkerFileName = "server.id";

/// The longest marker that will be read. A URL is bounded by `config.ini`'s own
/// line; past this the file is not one this build wrote.
inline constexpr std::size_t kMaxMarkerBytes = 1024;

/// The identity of the server `server_url` names: scheme and host lower-cased,
/// trailing slashes dropped, and any byte that is not printable ASCII or is a
/// space percent-encoded, so it is one token on a header line. Empty for an
/// empty URL -- an unconfigured console is bound to nothing.
std::string IdentityOf(std::string_view server_url);

/// The suffix a record bound to `identity` is set aside under: the identity
/// with everything but letters, digits, `.` and `-` turned to `-`, runs of `-`
/// collapsed, and at most 64 characters -- a FAT32 name a person can still read
/// (`http-192.168.1.5-8080`). `unknown` for an identity that leaves nothing.
std::string ArchiveSuffix(std::string_view identity);

/// A short, fixed-length name for `identity`, for a record that carries its
/// server on every row (`conflicts::Entry::server`, #317): the identity's MD5
/// in hex. Empty for an empty identity.
std::string TagOf(std::string_view identity);

/// What `Archive` did.
struct Archived {
  /// The file was moved. False when there was nothing to move, too.
  bool moved = false;

  /// Where it went, when it did.
  std::string to;

  /// Empty on success or when there was nothing to move; otherwise why the file
  /// stayed where it was, for a log.
  std::string message;

  bool ok() const { return message.empty(); }
};

/// Move `path` to the first free `<path>.<ArchiveSuffix(identity)>[.<n>]`, n
/// from 1 to 16 -- or, when `path` is not there, its `io::PreviousPathFor`,
/// which is where an interrupted commit leaves the record and where every
/// loader looks for it next.
///
/// A rename and never a copy-and-delete: the record's bytes are moved whole or
/// not at all, and nothing is ever removed. With every name taken, or the rename
/// refused, the file stays and `message` says so.
Archived Archive(const std::string& path, std::string_view identity);

/// What `BindRecords` did.
struct Rebound {
  /// The records were set aside because the marker named another server. The
  /// caller drops what it holds of them in memory.
  bool rebound = false;

  /// The marker names `identity` now. False when it could not be read or
  /// written, or a record could not be set aside -- the next call tries again.
  bool bound = false;

  /// With `rebound`: the identity the marker named, which the records were
  /// written against. Empty when it named none.
  std::string previous;

  /// One line per thing worth a log: what was set aside and where, and any
  /// file that could not be.
  std::vector<std::string> lines;
};

/// Hold `records` -- resolved paths -- to the server `identity` names, with the
/// marker at `marker`.
///
///   - **No marker** is a card from before #294, or a first boot: the records
///     are adopted as this server's and the marker written. A card upgraded from
///     rc2 has only ever had the server it is configured for, as far as anything
///     on it can show.
///   - **The same server**: nothing.
///   - **Another server** -- or a marker that names none -- sets every record
///     aside under that server's suffix (`Archive`), then writes the marker.
///     In that order, so a power cut between them is the same mismatch at the
///     next call, and a record already moved is simply not there to move again.
///     A record that could not be set aside leaves the marker as it was, for
///     the same reason: the next call tries again rather than read it.
///   - **A marker that is there and will not read** is a bad moment rather than
///     an answer: nothing is touched, and the next call asks again.
///
/// An empty `identity` -- no server configured -- touches nothing.
Rebound BindRecords(const std::string& marker, const std::vector<std::string>& records,
                    std::string_view identity);

}  // namespace rommsync::binding

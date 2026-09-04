// What pairing leaves behind: `sdmc:/config/rommsync/token.dat`.
//
// The device-code flow runs once. Everything after it -- every sync tick, every
// download, every boot -- depends on this file still holding a token that can be
// read back, so the two failure modes it has to rule out are a half-written file
// and a record that parses into something unusable.
//
// The write is atomic: the record goes to `<path>.tmp` and is renamed onto
// `path` only once it is complete, so a reader sees either the previous token or
// the new one and never a splice of the two. That is the same reasoning as the
// backup-before-overwrite rule for saves (docs/SYNC_PROTOCOL.md) and as
// `DownloadTarget`'s `.part` file, applied to the one file that cannot be
// re-fetched without a human at a browser.
//
// What it is *not*: hardened. Permissions, and whether the token should be at
// rest in plaintext at all on an SD card anything on the console can read, are
// M1-5's question (docs/SECURITY.md).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"

namespace rommsync::auth {

/// The record `token.dat` holds, one JSON object.
///
/// `server_url` is in here because a token is only meaningful against the RomM
/// that issued it: a user who repoints the sysmodule at a different server must
/// re-pair rather than send a stranger's server a bearer token.
///
/// There is no refresh token, because RomM issues none -- a 401 means revoked,
/// not expired (docs/AUTH.md).
struct StoredToken {
  std::string server_url;
  std::string access_token;
  std::string device_id;
  std::vector<std::string> scopes;

  /// `null` on every 5.2.0 response, meaning "does not expire on its own".
  /// Persisted anyway, so a RomM that starts setting it needs no format change.
  std::optional<std::string> expires_at;
};

/// The record pairing persists, given the server it paired against.
StoredToken StoredTokenFrom(std::string_view server_url, const DeviceTokenResponse& granted);

/// The record as one line of JSON.
///
/// One line, not pretty-printed: this is machine state, and the whole file is
/// written or none of it is.
std::string SerializeStoredToken(const StoredToken& token);

/// Read a record back. Held to exactly the bar `DeviceTokenResponse` is: a blank
/// or NUL-carrying token is refused rather than returned as a token-shaped
/// value that 401s on the next sync tick and looks like a server problem.
Parsed<StoredToken> ParseStoredToken(std::string_view text);

/// Why reading or writing the token file did not work.
enum class StoreError {
  kNone,
  kUnusableToken,  ///< the record would not have been readable back; nothing was written
  kOpenFailed,     ///< the temp file could not be created -- usually a missing directory
  kWriteFailed,    ///< the bytes did not all reach the disk; the destination is untouched
  kCommitFailed,   ///< the rename onto `path` failed; the destination is untouched
  kReadFailed,     ///< no such file, or it could not be read
  kMalformed,      ///< it was read, and it is not a token record
};

/// Stable, log-friendly name. Never null.
const char* ToString(StoreError error);

struct StoreResult {
  StoreError error = StoreError::kNone;

  /// For logs. Names the path and what went wrong; never the token.
  std::string message;

  bool ok() const { return error == StoreError::kNone; }
};

/// Write `token` to `path`, atomically.
///
/// The bytes go to `<path>.tmp`, are flushed and closed, and only then renamed
/// onto `path`. Every failure before that rename leaves whatever `path` already
/// held completely intact, which is the guarantee that matters: a failed write
/// costs the *new* token, never the working one.
///
/// The directory must already exist -- creating it is the platform layer's job,
/// since `sdmc:/config/rommsync/` is not a path the portable engine can mkdir
/// with only standard headers. A missing one is `kOpenFailed`, named.
///
/// What `rename` gives is atomicity, not durability: surviving a power cut mid
/// write needs an fsync the C++ standard library does not expose. The promise
/// here is that no reader ever sees a partial token.
StoreResult SaveToken(const std::string& path, const StoredToken& token);

struct LoadedToken {
  StoredToken value{};
  StoreError error = StoreError::kNone;
  std::string message;
  bool ok() const { return error == StoreError::kNone; }
};

/// Read `path` back. A missing file is `kReadFailed`, not an empty token: "not
/// paired yet" and "paired, and the file is gone" are the same recovery, but
/// only one of them should be reported as though nothing happened.
LoadedToken LoadToken(const std::string& path);

}  // namespace rommsync::auth

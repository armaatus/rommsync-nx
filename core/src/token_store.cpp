#include "rommsync/token_store.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/json.hpp"

namespace rommsync::auth {
namespace {

/// The bar every persisted string is held to, matching `json::Reader`: a blank
/// value is a record that lost something, and an embedded NUL is a value that
/// truncates the moment it reaches a C API -- so the token that would be sent
/// is not the token that was checked.
bool Usable(const std::string& value) {
  return !value.empty() && value.find('\0') == std::string::npos;
}

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

}  // namespace

const char* ToString(StoreError error) {
  switch (error) {
    case StoreError::kNone:
      return "none";
    case StoreError::kUnusableToken:
      return "unusable_token";
    case StoreError::kOpenFailed:
      return "open_failed";
    case StoreError::kWriteFailed:
      return "write_failed";
    case StoreError::kCommitFailed:
      return "commit_failed";
    case StoreError::kReadFailed:
      return "read_failed";
    case StoreError::kMalformed:
      return "malformed";
  }
  return "none";
}

StoredToken StoredTokenFrom(std::string_view server_url, const DeviceTokenResponse& granted) {
  StoredToken token;
  token.server_url = std::string(server_url);
  token.access_token = granted.access_token;
  token.device_id = granted.device_id;
  token.scopes = granted.scopes;
  token.expires_at = granted.expires_at;
  return token;
}

std::string SerializeStoredToken(const StoredToken& token) {
  std::string out("{\"server_url\":");
  out += json::Quote(token.server_url);
  out += ",\"access_token\":";
  out += json::Quote(token.access_token);
  out += ",\"device_id\":";
  out += json::Quote(token.device_id);
  out += ",\"scopes\":";
  out += json::QuoteArray(token.scopes);
  out += ",\"expires_at\":";
  // `null` rather than an omitted key: "does not expire" is an answer RomM
  // gives, and a reader that could not tell it from a missing field would have
  // to guess which one it was looking at.
  out += token.expires_at.has_value() ? json::Quote(*token.expires_at) : std::string("null");
  out += "}";
  return out;
}

Parsed<StoredToken> ParseStoredToken(std::string_view text) {
  Parsed<StoredToken> parsed;
  json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    parsed.error = std::move(document.error);
    return parsed;
  }

  StoredToken token;
  json::Reader reader(document.value, "token record");
  reader.Required("server_url", &token.server_url);
  reader.Required("access_token", &token.access_token);
  reader.Required("device_id", &token.device_id);
  reader.Required("scopes", &token.scopes);
  reader.RequiredNullable("expires_at", &token.expires_at);
  if (!reader.ok()) {
    parsed.error = reader.error();
    return parsed;
  }

  parsed.value = std::move(token);
  return parsed;
}

StoreResult SaveToken(const std::string& path, const StoredToken& token) {
  // Refused here rather than on the way back in: a record that cannot be read
  // is worse than no record at all, because the sysmodule would find a file,
  // believe it is paired, and 401 forever.
  if (!Usable(token.server_url) || !Usable(token.access_token) || !Usable(token.device_id) ||
      // `expires_at` is held to the same bar the reader holds it to: `null`
      // means "no expiry", but a *present* empty string is refused on the way
      // back in, so writing one produces a file that cannot be read.
      (token.expires_at.has_value() && !Usable(*token.expires_at))) {
    return {StoreError::kUnusableToken,
            Describe(path, "refusing to write a token with a blank or NUL-carrying field")};
  }

  // The atomicity, the two-rename commit and the reasoning behind both live in
  // `io::WriteAtomically` -- `device.dat` needs exactly the same guarantee, and
  // a second copy of that commit is the kind of thing that gets fixed in one
  // place and not the other.
  const io::WriteResult written = io::WriteAtomically(path, SerializeStoredToken(token) + "\n");
  switch (written.error) {
    case io::WriteError::kNone:
      return {};
    case io::WriteError::kOpenFailed:
      return {StoreError::kOpenFailed, written.message};
    case io::WriteError::kWriteFailed:
      return {StoreError::kWriteFailed, written.message};
    case io::WriteError::kCommitFailed:
      return {StoreError::kCommitFailed, written.message};
  }
  return {StoreError::kCommitFailed, written.message};
}

std::string DescribeStoredToken(const StoredToken& token) {
  // Everything here is either a server the user typed or an identifier RomM
  // shows in its own device list. The token itself is reduced to the two facts
  // a support thread ever needs -- that there is one, and how long it is --
  // because a log line is forever and an SD card is readable by anything.
  std::string out("server=");
  out += token.server_url;
  out += " device=";
  out += token.device_id;
  out += " scopes=[";
  for (std::size_t at = 0; at < token.scopes.size(); ++at) {
    if (at != 0) {
      out += " ";
    }
    out += token.scopes[at];
  }
  // **Spelled `credential=` and deliberately not `token=`.** A field literally
  // named `token` is what any redactor treats as a secret -- `log::Redact` does,
  // and so does a human running `grep -i token` over a log somebody pasted --
  // and this line's whole job is to be the one summary that survives being
  // logged. Redacted, it would read `token=<redacted>` for a console with no
  // credentials and `token=<redacted>(68 chars)` for one that has them, which
  // loses the single bit a support thread most needs (M7-3, #38).
  out += "] credential=";
  out += token.access_token.empty()
             ? std::string("absent")
             : "present(" + std::to_string(token.access_token.size()) + " chars)";
  out += " expires=";
  out += token.expires_at.value_or("never");
  return out;
}

bool DiscardToken(const std::string& path) {
  // Every one of the three, not just `token.dat`. "Re-pair" that unlinked the
  // record and left `token.dat.old` holding the same bearer token would have
  // discarded nothing -- and `.old` is exactly what an interrupted commit
  // leaves behind. What the overwrite inside `io::Shred` is and is not worth on
  // an SD card is in docs/SECURITY.md.
  return io::Shred(path);
}

namespace {

LoadedToken ReadRecord(const std::string& path, io::ReadError* how) {
  LoadedToken loaded;
  const io::ReadResult read = io::ReadFile(path);
  *how = read.error;
  if (!read.ok()) {
    loaded.error = StoreError::kReadFailed;
    loaded.message = read.message;
    return loaded;
  }

  Parsed<StoredToken> parsed = ParseStoredToken(read.contents);
  if (!parsed.ok()) {
    loaded.error = StoreError::kMalformed;
    loaded.message = Describe(path, parsed.error.Describe());
    return loaded;
  }
  loaded.value = std::move(parsed.value);
  return loaded;
}

}  // namespace

LoadedToken LoadToken(const std::string& path) {
  io::ReadError how = io::ReadError::kNone;
  LoadedToken loaded = ReadRecord(path, &how);
  if (loaded.ok()) {
    return loaded;
  }
  // A `token.dat` that *exists* and would not open is a bad moment, not a
  // commit window. Falling back then would answer a transient failure with the
  // previous token -- which may be the one the user just revoked, so the next
  // sync tick 401s and the overlay asks for a re-pair that was never needed.
  if (how == io::ReadError::kUnreadable) {
    return loaded;
  }
  // Missing or unreadable-as-a-record, on the other hand, is exactly what the
  // moment between `WriteAtomically`'s two renames looks like, and what is
  // sitting in `.old` then is the previous complete record. Reading it is the
  // difference between an interruption nobody notices and a re-pair at a
  // browser. The original error is kept when there is nothing to fall back to,
  // so a plain missing file still says so.
  io::ReadError ignored = io::ReadError::kNone;
  LoadedToken previous = ReadRecord(io::PreviousPathFor(path), &ignored);
  return previous.ok() ? previous : loaded;
}

}  // namespace rommsync::auth

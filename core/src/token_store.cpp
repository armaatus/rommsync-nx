#include "rommsync/token_store.hpp"

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/json.hpp"

namespace rommsync::auth {
namespace {

/// The suffix the record is staged under before it is renamed into place.
constexpr const char* kTempSuffix = ".tmp";

/// Where the record already in place is moved while the new one takes its
/// name. See `SaveToken` for why the move is unconditional.
constexpr const char* kPreviousSuffix = ".old";

bool Exists(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  std::fclose(file);
  return true;
}

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

  const std::string temp = path + kTempSuffix;
  const std::string text = SerializeStoredToken(token) + "\n";

  std::FILE* file = std::fopen(temp.c_str(), "wb");
  if (file == nullptr) {
    return {StoreError::kOpenFailed,
            Describe(temp, "could not be created (does the directory exist?)")};
  }

  const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
  // The flush has to happen while the handle is still open: fclose reports a
  // failed flush too, but by then there is nothing left to distinguish "the
  // bytes never left the buffer" from "the close itself failed".
  const bool flushed = written == text.size() && std::fflush(file) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!flushed || !closed) {
    std::remove(temp.c_str());
    return {StoreError::kWriteFailed, Describe(temp, "could not be written completely")};
  }

  // The commit. Everything above touched only the temp file, so a failure up to
  // this point leaves whatever `path` already held exactly as it was.
  //
  // `rename` replaces the destination on POSIX and does *not* on Horizon:
  // `fsFsRenameFile` refuses a destination that already exists, which is every
  // re-pair. So the record already in place is moved aside first -- on both
  // platforms, deliberately, because a fallback taken only on the console is a
  // path no host test ever runs and the v1 gate is the first thing that would
  // see it.
  const std::string previous = path + kPreviousSuffix;
  std::remove(previous.c_str());
  const bool replacing = Exists(path);
  if (replacing && std::rename(path.c_str(), previous.c_str()) != 0) {
    std::remove(temp.c_str());
    return {StoreError::kCommitFailed, Describe(path, "could not be moved aside to " + previous)};
  }
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    if (replacing) {
      std::rename(previous.c_str(), path.c_str());
    }
    std::remove(temp.c_str());
    return {StoreError::kCommitFailed, Describe(path, "could not be replaced by " + temp)};
  }
  std::remove(previous.c_str());
  return {};
}

namespace {

LoadedToken ReadRecord(const std::string& path) {
  LoadedToken loaded;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    loaded.error = StoreError::kReadFailed;
    loaded.message = Describe(path, "could not be opened");
    return loaded;
  }

  std::string text;
  char buffer[512];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, got);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed) {
    loaded.error = StoreError::kReadFailed;
    loaded.message = Describe(path, "could not be read");
    return loaded;
  }

  Parsed<StoredToken> parsed = ParseStoredToken(text);
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
  LoadedToken loaded = ReadRecord(path);
  if (loaded.ok()) {
    return loaded;
  }
  // The one moment `path` does not exist is between the two renames in
  // `SaveToken`, and what is sitting in `.old` then is the previous complete
  // record. Reading it is the difference between a re-pair nobody notices and
  // one the user has to do at a browser. The original error is kept when there
  // is nothing to fall back to, so a plain missing file still says so.
  LoadedToken previous = ReadRecord(path + kPreviousSuffix);
  return previous.ok() ? previous : loaded;
}

}  // namespace rommsync::auth

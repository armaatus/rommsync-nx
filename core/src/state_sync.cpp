// The states half of a tick: where the client has to arbitrate for itself.
//
// Read `state_sync.hpp` first -- the two contract facts and the policy they
// force are there. What is only here is the decision table, which is small
// enough to state whole. For one local state, keyed `(rom_id, file_name)`:
//
//   | baseline row | server row with that name | local file | -> |
//   |---|---|---|---|
//   | none         | none                      | -          | upload    |
//   | none         | present                   | -          | keep both |
//   | id X         | none                      | -          | upload    |
//   | id X         | id != X                   | -          | keep both |
//   | id X         | id X, unmoved             | unchanged  | no-op     |
//   | id X         | id X, unmoved             | changed    | upload    |
//   | id X         | id X, moved               | unchanged  | download  |
//   | id X         | id X, moved               | changed    | keep both |
//
// ...and one server row no local file claimed is placed, which overwrites
// nothing. **Every branch that is not an unambiguous match keeps both copies**,
// and no branch deletes anything.
//
// The overwrite order is `sync_execute.cpp`'s, unchanged and not re-spelled:
//
//   1. fetch to `io::TempPathFor(<state>)`, never to `<state>`
//   2. check those bytes against the length the server reported -- which is all
//      there is; see the header
//   3. `sync::BackUpFirst` the state's current bytes into `.backup/`
//   4. `io::CommitStaged` the bytes onto the state
#include "rommsync/state_sync.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/hash_file.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync_execute.hpp"

namespace rommsync::sync {
namespace {

constexpr const char* kStatesPath = "/api/states";

/// The largest `GET /api/states` body this client will work from.
///
/// `/api/states` takes no `limit` and returns every state the user has, so
/// unlike `GET /api/roms` there is no paging lever to pull -- the bound is all
/// there is. At the ~600 bytes a `StateSchema` row costs, this is around 220
/// states across the whole library, against `scan::kMaxStates` local ones.
///
/// **Over the bound the whole run stops rather than working from what fits.**
/// A partial listing is worse than none: a local state whose server row fell off
/// the end reads as "the server has no row with this name", and the answer to
/// that is an upload -- which, `POST /api/states` being an upsert, would replace
/// the row that was there. So the listing is all of it or none of it.
constexpr std::size_t kMaxStateListBytes = 128 * 1024;

/// Percent-encode one query-string value.
///
/// A state's `file_name` is user data off a FAT32 card and its `emulator` is a
/// folder name a human chose, so neither may be pasted into a URL: an `&` in one
/// would move the rest of the value into a parameter of its own.
std::string EncodeQuery(std::string_view value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (unreserved) {
      out.push_back(character);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }
  }
  return out;
}

/// How one state is named in a log line: the rom and the file. Never a token,
/// and never the file's contents.
std::string Describe(std::int64_t rom_id, std::string_view file_name) {
  return "rom " + std::to_string(rom_id) + ", state \"" + std::string(file_name) + "\"";
}

http::Request Authed(http::Method method, std::string url, const auth::StoredToken& token,
                     const StateSyncOptions& options) {
  http::Request request;
  request.method = method;
  request.url = std::move(url);
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.timeout = options.timeout;
  request.cancel = options.cancel;
  return request;
}

/// The reason an exchange did not produce a usable answer, or none when it did.
/// `sync_execute.cpp`'s `Refused`, over this file's error enum.
std::optional<std::pair<StateError, std::string>> Refused(const http::Result& result,
                                                          std::string_view what) {
  if (!result.ok()) {
    const StateError error =
        result.error == http::Error::kCanceled ? StateError::kCanceled : StateError::kTransferFailed;
    return std::make_pair(error,
                          std::string(what) + " did not complete: " + http::ToString(result.error) +
                              (result.message.empty() ? "" : " (" + result.message + ")"));
  }
  const int status = result.response.status;
  if (status == 401) {
    return std::make_pair(StateError::kUnauthorized,
                          std::string(what) + " was rejected: HTTP 401; the token has been revoked");
  }
  if (status == 403) {
    return std::make_pair(StateError::kForbidden,
                          std::string(what) +
                              " was rejected: HTTP 403; this pairing was not granted the "
                              "assets.read and assets.write scopes states need");
  }
  if (!result.successful()) {
    return std::make_pair(StateError::kRefused,
                          std::string(what) + " was refused: HTTP " + std::to_string(status));
  }
  return std::nullopt;
}

/// The staged download, removed unless something takes it. `sync_execute.cpp`'s
/// guard, for its reason: an unverified download is not a state.
class StagedFile {
 public:
  explicit StagedFile(std::string path) : path_(std::move(path)) {}
  ~StagedFile() {
    if (!path_.empty()) {
      std::remove(path_.c_str());
    }
  }

  StagedFile(const StagedFile&) = delete;
  StagedFile& operator=(const StagedFile&) = delete;

  const std::string& path() const { return path_; }
  void Release() { path_.clear(); }

 private:
  std::string path_;
};

StateOperationResult Fail(std::int64_t rom_id, std::string file_name, StateAction action,
                          StateError error, std::string message) {
  StateOperationResult result;
  result.action = action;
  result.rom_id = rom_id;
  result.message = Describe(rom_id, file_name) + ": " + std::move(message);
  result.file_name = std::move(file_name);
  result.outcome =
      error == StateError::kCanceled ? StateOutcome::kCanceled : StateOutcome::kFailed;
  result.error = error;
  return result;
}

}  // namespace

const char* ToString(StateAction action) {
  switch (action) {
    case StateAction::kNoOp:
      return "no_op";
    case StateAction::kUpload:
      return "upload";
    case StateAction::kDownload:
      return "download";
    case StateAction::kPlace:
      return "place";
    case StateAction::kKeepBoth:
      return "keep_both";
  }
  return "no_op";
}

const char* ToString(StateOutcome outcome) {
  switch (outcome) {
    case StateOutcome::kNoOp:
      return "no_op";
    case StateOutcome::kUploaded:
      return "uploaded";
    case StateOutcome::kDownloaded:
      return "downloaded";
    case StateOutcome::kKeptBoth:
      return "kept_both";
    case StateOutcome::kFailed:
      return "failed";
    case StateOutcome::kCanceled:
      return "canceled";
  }
  return "failed";
}

const char* ToString(StateError error) {
  switch (error) {
    case StateError::kNone:
      return "none";
    case StateError::kUnreadableCard:
      return "unreadable_card";
    case StateError::kBackupFailed:
      return "backup_failed";
    case StateError::kTransferFailed:
      return "transfer_failed";
    case StateError::kRefused:
      return "refused";
    case StateError::kUnauthorized:
      return "unauthorized";
    case StateError::kForbidden:
      return "forbidden";
    case StateError::kUnverified:
      return "unverified";
    case StateError::kCommitFailed:
      return "commit_failed";
    case StateError::kNoPlacement:
      return "no_placement";
    case StateError::kCanceled:
      return "canceled";
  }
  return "none";
}

auth::Answer AnswerOf(StateError error) {
  switch (error) {
    case StateError::kUnauthorized:
      return auth::Answer::kRejected;
    case StateError::kForbidden:
      return auth::Answer::kForbidden;
    // The one acceptance: an operation that did what it set out to did it with
    // this token.
    case StateError::kNone:
      return auth::Answer::kAccepted;
    case StateError::kUnreadableCard:
    case StateError::kBackupFailed:
    case StateError::kTransferFailed:
    case StateError::kRefused:
    case StateError::kUnverified:
    case StateError::kCommitFailed:
    case StateError::kNoPlacement:
    case StateError::kCanceled:
      break;
  }
  return auth::Answer::kSilent;
}

namespace {

/// One `StateSchema` object, read strictly. Shared by the list and the single
/// row `POST /api/states` answers with, so the two cannot drift apart.
bool ReadStateRow(const json::Value& element, ServerState* out, json::Error* error) {
  ServerState state;
  std::optional<std::string> emulator;
  std::string updated_at;
  json::Reader reader(element, "state row");
  reader.Required("id", &state.id);
  reader.Required("rom_id", &state.rom_id);
  reader.Required("file_name", &state.file_name);
  reader.RequiredNullable("emulator", &emulator);
  reader.Required("file_size_bytes", &state.file_size_bytes);
  reader.Required("updated_at", &updated_at);
  if (!reader.ok()) {
    *error = reader.error();
    return false;
  }
  const std::optional<Timestamp> when = ParseTimestamp(updated_at);
  if (!when.has_value()) {
    *error = json::Error{0, "state row", "updated_at is not a timestamp this client can read"};
    return false;
  }
  if (state.id <= 0 || state.rom_id <= 0) {
    *error = json::Error{0, "state row", "id and rom_id must be positive"};
    return false;
  }
  if (!IsSingleFileName(state.file_name)) {
    // The name is a pairing key that gets joined into a backup path and a
    // placement. A row whose name is a path is a row this client refuses to act
    // on rather than one it sanitises into something else.
    *error = json::Error{0, "state row", "file_name is not a single file name"};
    return false;
  }
  if (state.file_size_bytes < 0) {
    *error = json::Error{0, "state row", "file_size_bytes is negative"};
    return false;
  }
  state.emulator = emulator.value_or(std::string());
  state.updated_at = *when;
  *out = std::move(state);
  return true;
}

}  // namespace

auth::Parsed<ServerState> ParseState(std::string_view body) {
  auth::Parsed<ServerState> parsed;
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    parsed.error = document.error;
    return parsed;
  }
  ReadStateRow(document.value, &parsed.value, &parsed.error);
  return parsed;
}

auth::Parsed<std::vector<ServerState>> ParseStateList(std::string_view body) {
  auth::Parsed<std::vector<ServerState>> parsed;
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    parsed.error = document.error;
    return parsed;
  }
  if (!document.value.is_array()) {
    parsed.error = json::Error{0, "state list", "expected an array of StateSchema"};
    return parsed;
  }
  std::vector<ServerState> states;
  states.reserve(document.value.elements().size());
  for (const json::Value& element : document.value.elements()) {
    ServerState state;
    if (!ReadStateRow(element, &state, &parsed.error)) {
      return parsed;
    }
    states.push_back(std::move(state));
  }
  parsed.value = std::move(states);
  return parsed;
}

namespace {

/// `GET /api/states` -- every state this user holds, in one call.
///
/// One unfiltered request rather than one per rom: a console with a hundred roms
/// would otherwise spend a hundred round trips per tick to find out that nothing
/// changed, and the endpoint offers no way to ask about several roms at once.
/// The cost is the bound above.
bool ListServerStates(http::HttpClient& client, const auth::StoredToken& token,
                      const StateSyncOptions& options, std::vector<ServerState>* out,
                      StateError* error, std::string* message) {
  const http::Result result =
      client.Send(Authed(http::Method::kGet, http::JoinUrl(token.server_url, kStatesPath), token,
                         options));
  if (const auto refused = Refused(result, "the state listing")) {
    *error = refused->first;
    *message = refused->second;
    return false;
  }
  if (result.response.body.size() > kMaxStateListBytes) {
    *error = StateError::kRefused;
    *message = "the state listing is " + std::to_string(result.response.body.size()) +
               " bytes, more than the " + std::to_string(kMaxStateListBytes) +
               " this client can work from; no state was touched this tick";
    return false;
  }
  const auth::Parsed<std::vector<ServerState>> parsed = ParseStateList(result.response.body);
  if (!parsed.ok()) {
    *error = StateError::kRefused;
    *message = "the state listing did not parse (" + parsed.error.Describe() + ")";
    return false;
  }
  *out = parsed.value;
  return true;
}

/// The server's row for `(rom_id, file_name)`, or nullptr.
const ServerState* FindServerState(const std::vector<ServerState>& states, std::int64_t rom_id,
                                   std::string_view file_name) {
  for (const ServerState& state : states) {
    if (state.rom_id == rom_id && state.file_name == file_name) {
      return &state;
    }
  }
  return nullptr;
}

/// Whether the server row is still the one the baseline agreed on.
///
/// Id, timestamp and size together. The id alone would miss a replacement; the
/// timestamp alone misses a same-second rewrite, which is exactly what a second
/// console syncing in the same second produces.
bool ServerUnmoved(const state::StateRecord& row, const ServerState& state) {
  return state.id == row.server_state_id &&
         UnixSeconds(state.updated_at) == UnixSeconds(row.server_updated_at) &&
         state.file_size_bytes == row.server_file_size_bytes;
}

/// Whether the local file is still the one the baseline hashed.
bool LocalUnmoved(const state::StateRecord& row, const scan::StateFile& file) {
  return UnixSeconds(row.mtime) == file.modified_unix && row.file_size_bytes == file.size_bytes &&
         !file.content_hash.empty() && row.content_hash == file.content_hash;
}

/// The decision table from the file header, as one function.
StateAction Decide(const state::StateRecord* row, const ServerState* server,
                   const scan::StateFile& file, std::string* why) {
  if (row == nullptr) {
    if (server == nullptr) {
      return StateAction::kUpload;
    }
    *why = "the server already holds a state under this name that this console has never synced; "
           "RomM stores one state per name, so uploading would replace it -- both copies are kept "
           "and neither was touched";
    return StateAction::kKeepBoth;
  }
  if (server == nullptr) {
    // The row this console was paired with is gone from the server. Uploading
    // is safe -- there is nothing under that name to replace -- and it is the
    // only branch that puts a deleted state back, which is what "never delete"
    // means from the other side.
    return StateAction::kUpload;
  }
  if (server->id != row->server_state_id) {
    *why = "the server's state under this name is a different row from the one this console last "
           "synced; both copies are kept and neither was touched";
    return StateAction::kKeepBoth;
  }
  const bool server_moved = !ServerUnmoved(*row, *server);
  const bool local_moved = !LocalUnmoved(*row, file);
  if (!server_moved && !local_moved) {
    return StateAction::kNoOp;
  }
  if (!server_moved) {
    return StateAction::kUpload;
  }
  if (!local_moved) {
    return StateAction::kDownload;
  }
  *why = "both this console's copy and the server's have changed since the last sync, and RomM "
         "arbitrates neither; both copies are kept and neither was touched";
  return StateAction::kKeepBoth;
}

/// `POST /api/states?rom_id=&emulator=` with the local file as `stateFile`.
///
/// POST rather than `PUT /api/states/{id}` even when the row is known, because
/// the two do the same thing here -- POST upserts on `(rom_id, file_name)` -- and
/// POST is also the call that creates. One code path, and the caller has already
/// established that the name is one it may write.
StateOperationResult Upload(http::HttpClient& client, fs::FileSystem& files,
                            const auth::StoredToken& token, const scan::StateFile& file,
                            StateAction action, const StateSyncOptions& options) {
  const std::string source = files.Resolve(file.sd_path);
  if (source.empty() || !io::Exists(source)) {
    return Fail(file.rom_id, file.file_name, action, StateError::kUnreadableCard,
                "the local state " + file.sd_path + " could not be opened to upload");
  }

  std::string url = http::JoinUrl(token.server_url, kStatesPath) +
                    "?rom_id=" + std::to_string(file.rom_id);
  if (!file.emulator.empty()) {
    url += "&emulator=" + EncodeQuery(file.emulator);
  }

  http::Request request = Authed(http::Method::kPost, std::move(url), token, options);
  // No total timeout on the body: a state is tens of megabytes on a link that
  // may be a phone hotspot, so "no bytes moved for this long" is the ceiling.
  request.timeout = std::chrono::milliseconds{0};
  request.stall_timeout = options.stall_timeout;
  http::FormPart part;
  part.name = "stateFile";
  part.file_path = source;
  // The client's own name, and this time it is also the server's: RomM stores a
  // state under the name it was sent (state_sync.hpp).
  part.file_name = file.file_name;
  part.content_type = "application/octet-stream";
  request.form.push_back(std::move(part));

  const http::Result result = client.Send(request);
  if (const auto refused = Refused(result, "the state upload")) {
    return Fail(file.rom_id, file.file_name, action, refused->first, refused->second);
  }

  // **Unlike a save's upload, the response row IS the pairing.** There is no
  // negotiation to recover it from and no `device_syncs` on the server, so a
  // body this client cannot read leaves the state with no baseline row -- which
  // the next tick reads as "never synced" and answers with a keep-both. That is
  // safe, and it is also permanent until someone renames the file, so it is
  // reported as a failure rather than shrugged off.
  //
  // The row is taken from this response rather than re-read: RomM renders
  // `updated_at` with microseconds here and to whole seconds in the listing,
  // and `ParseTimestamp` drops the fraction downwards -- so both spellings are
  // the same `UnixSeconds`, which is what `ServerUnmoved` compares.
  const auth::Parsed<ServerState> row = ParseState(result.response.body);
  if (!row.ok()) {
    return Fail(file.rom_id, file.file_name, action, StateError::kRefused,
                "the upload landed and its response did not parse (" + row.error.Describe() +
                    "), so this state has no server row recorded");
  }

  StateOperationResult uploaded;
  uploaded.action = action;
  uploaded.rom_id = file.rom_id;
  uploaded.file_name = file.file_name;
  uploaded.outcome = StateOutcome::kUploaded;
  uploaded.sd_path = file.sd_path;
  uploaded.message = Describe(file.rom_id, file.file_name) + ": uploaded " + file.sd_path;
  uploaded.state_id = row.value.id;
  uploaded.server = row.value;
  return uploaded;
}

/// The download half of `kDownload` and `kPlace`: fetch, check the length, back
/// up, commit. `sync_execute.cpp`'s order, over the one check a state gets.
///
/// `sd_path` is where the bytes go and `previous` is the backup discriminator's
/// name; for a placement there is nothing at `sd_path`, so `BackUpFirst` finds
/// no source and there is nothing to protect.
StateOperationResult Fetch(http::HttpClient& client, fs::FileSystem& files,
                           const auth::StoredToken& token, const ServerState& server,
                           const std::string& sd_path, StateAction action,
                           const StateSyncOptions& options, std::vector<std::string>* warnings) {
  const std::string destination = files.Resolve(sd_path);
  if (destination.empty()) {
    return Fail(server.rom_id, server.file_name, action, StateError::kUnreadableCard,
                "the state path " + sd_path + " is not a path on this card");
  }

  // Staged beside the state rather than at it: `http::DownloadTarget` renames
  // its `.part` onto the destination the instant the body ends, which is before
  // anything has checked it. A stale one from an interrupted run goes first --
  // the bytes in it are a download that never completed and belong to nothing.
  StagedFile staged(io::TempPathFor(destination));
  std::remove(staged.path().c_str());

  http::Request request =
      Authed(http::Method::kGet,
             http::JoinUrl(token.server_url,
                           std::string(kStatesPath) + "/" + std::to_string(server.id) + "/content"),
             token, options);
  request.timeout = std::chrono::milliseconds{0};
  request.stall_timeout = options.stall_timeout;

  http::DownloadTarget into;
  into.path = staged.path();
  // Never resumed, for `sync_execute.cpp`'s reason: a resumed transfer of a file
  // whose server copy may have moved on is two different states spliced at a
  // byte offset.
  into.resume = false;
  // **The whole of the verification.** RomM computes no digest for a state, so
  // this is what catches the clean short body a `truncate` fault or a dropped
  // connection leaves -- and nothing else. It is not an integrity check, and the
  // warning below says so out loud rather than letting a reader assume the save
  // path's MD5 is in play here too.
  into.expected_size = server.file_size_bytes > 0
                           ? static_cast<std::uint64_t>(server.file_size_bytes)
                           : 0;

  const http::Result fetched = client.Download(request, into);
  if (const auto refused = Refused(fetched, "the state download")) {
    // The backend leaves its partial behind for a resume that is never coming,
    // so it is litter beside a state rather than progress towards one.
    std::remove(http::PartialPathFor(staged.path()).c_str());
    return Fail(server.rom_id, server.file_name, action, refused->first, refused->second);
  }
  StateOperationResult result;
  result.action = action;
  result.rom_id = server.rom_id;
  result.file_name = server.file_name;
  result.sd_path = sd_path;
  result.state_id = server.id;

  const std::int64_t stamp =
      UnixSeconds(options.now != nullptr ? options.now() : std::chrono::system_clock::now());
  std::string message;
  const OperationError backed_up =
      BackUpFirst(files, options.backup_dir, sd_path, server.rom_id,
                  StateBackupDiscriminator(server.file_name), server.file_name, stamp,
                  &result.backup_sd_path, &message);
  if (backed_up != OperationError::kNone) {
    const StateError error = backed_up == OperationError::kBackupFailed
                                 ? StateError::kBackupFailed
                                 : StateError::kUnreadableCard;
    StateOperationResult failed =
        Fail(server.rom_id, server.file_name, action, error, std::move(message));
    failed.sd_path = sd_path;
    return failed;
  }

  const std::string staged_path = staged.path();
  // Consumed by the commit whether it succeeds or fails, so the guard is done.
  staged.Release();
  const io::WriteResult committed = io::CommitStaged(staged_path, destination);
  if (!committed.ok()) {
    StateOperationResult failed =
        Fail(server.rom_id, server.file_name, action, StateError::kCommitFailed,
             "the downloaded bytes could not be put in place: " + committed.message);
    failed.sd_path = sd_path;
    failed.backup_sd_path = result.backup_sd_path;
    return failed;
  }

  // Nothing to confirm: there is no `/api/states/{id}/downloaded`, so this
  // device's having the state is recorded in `state.db` and nowhere else.
  // Said once the bytes are actually in place, so the sentence describes a state
  // that was written rather than one that might have been.
  if (into.expected_size == 0) {
    // A row that claims no length is a row that cannot catch a short body at
    // all. The bytes are still written -- refusing would leave the state
    // unsynced forever over a field RomM may simply not have filled in -- but
    // this is the one case where a caller has to be told the download went in
    // completely unchecked.
    warnings->push_back(Describe(server.rom_id, server.file_name) +
                        ": the server reports no length for this state, so the download was not "
                        "checked at all");
  } else {
    warnings->push_back(Describe(server.rom_id, server.file_name) +
                        ": checked against its length only -- RomM computes no digest for a state, "
                        "so this is not an integrity check");
  }

  result.outcome = StateOutcome::kDownloaded;
  result.message = Describe(server.rom_id, server.file_name) + ": wrote the server's copy to " +
                   sd_path +
                   (result.backup_sd_path.empty()
                        ? std::string(", replacing nothing")
                        : ", after backing up its previous bytes to " + result.backup_sd_path);
  return result;
}

StateOperationResult KeepBoth(std::int64_t rom_id, const std::string& file_name,
                              const std::string& sd_path, std::string why) {
  StateOperationResult result;
  result.action = StateAction::kKeepBoth;
  result.rom_id = rom_id;
  result.file_name = file_name;
  result.sd_path = sd_path;
  result.outcome = StateOutcome::kKeptBoth;
  result.message = Describe(rom_id, file_name) + ": " + std::move(why);
  return result;
}

/// Record what an operation did, or drop the row it invalidated.
///
/// The row is built from the card rather than from what the tick reported, for
/// `state::ReadBackFile`'s reason -- and here it applies to an *upload* as well
/// as to a download, because the row also has to carry the server's new
/// `updated_at`, which only the response knows.
void Advance(fs::FileSystem& files, const scan::StateFile* local, const ServerState& server,
             const std::string& sd_path, state::Baseline* baseline,
             std::vector<std::string>* warnings) {
  // **A listing per call, not one carried across the run.** `fs::Directories`
  // re-lists only when the directory changes, and here the writes and the
  // read-backs interleave: two states downloaded into one folder would have the
  // second read back out of the listing taken before it was written, which is a
  // row claiming a digest against the *previous* file's mtime and size. That row
  // then matches nothing forever. `sync_finish` can share one because every
  // write there has already happened by the time it reads.
  fs::Directories directories(files);
  state::FileFacts facts;
  std::string why;
  if (!state::ReadBackFile(files, directories, sd_path, &facts, &why)) {
    // The bytes on the card are not ones this client can describe, so there is
    // no honest row to write. Erasing rather than keeping: whatever the stored
    // row described, it is not what is at that path now.
    baseline->EraseState(server.rom_id, server.file_name);
    warnings->push_back(Describe(server.rom_id, server.file_name) +
                        ": its row could not be recorded (" + why +
                        "), so this state is compared from scratch next tick");
    return;
  }
  state::StateRecord row;
  row.rom_id = server.rom_id;
  row.file_name = server.file_name;
  row.emulator = local != nullptr ? local->emulator : server.emulator;
  row.content_hash = facts.content_hash;
  row.mtime = facts.mtime;
  row.file_size_bytes = facts.file_size_bytes;
  row.server_state_id = server.id;
  row.server_updated_at = server.updated_at;
  row.server_file_size_bytes = server.file_size_bytes;
  baseline->SetState(std::move(row));
}

/// Drop state rows until the baseline fits, and never a save row.
///
/// The ones this run knows nothing about go first -- almost always states that
/// are no longer on the card and no longer on the server -- and only then the
/// ones it does, newest key last. Saves are untouched at every step: a save is
/// what hard rule 2 protects, and a state that loses its row costs a re-hash and
/// a keep-both, never a file.
std::size_t TrimStates(const std::vector<StateOperationResult>& operations,
                       state::Baseline* baseline) {
  if (baseline->size() <= state::kMaxRecords) {
    return 0;
  }
  std::vector<std::pair<std::int64_t, std::string>> touched;
  touched.reserve(operations.size());
  for (const StateOperationResult& operation : operations) {
    touched.emplace_back(operation.rom_id, operation.file_name);
  }
  const auto known = [&touched](std::int64_t rom_id, const std::string& file_name) {
    return std::find(touched.begin(), touched.end(), std::make_pair(rom_id, file_name)) !=
           touched.end();
  };

  std::size_t dropped = 0;
  for (int pass = 0; pass < 2 && baseline->size() > state::kMaxRecords; ++pass) {
    std::vector<std::pair<std::int64_t, std::string>> droppable;
    for (const auto& [key, row] : baseline->state_rows()) {
      (void)row;
      if (pass == 0 && known(key.first, key.second)) {
        continue;
      }
      droppable.push_back(key);
    }
    for (const auto& key : droppable) {
      if (baseline->size() <= state::kMaxRecords) {
        break;
      }
      if (baseline->EraseState(key.first, key.second)) {
        ++dropped;
      }
    }
  }
  return dropped;
}

}  // namespace

StateSyncReport SyncStates(http::HttpClient& client, fs::FileSystem& files,
                           const auth::StoredToken& token, const config::Config& config,
                           const roms::RomIndex& index, state::Baseline* baseline,
                           const StateSyncOptions& options) {
  StateSyncReport report;
  if (!config.sync.states) {
    // Off by default means *silent*: no directory is listed and no request is
    // made. This `return` is the whole of that promise, and `ran` is how a
    // caller -- or a test -- can tell it was kept.
    return report;
  }
  report.ran = true;

  report.scan = scan::ScanStates(config, index, files, *baseline);

  std::vector<ServerState> server_states;
  StateError error = StateError::kNone;
  std::string message;
  if (!ListServerStates(client, token, options, &server_states, &error, &message)) {
    report.failed = 1;
    report.unauthorized =
        error == StateError::kUnauthorized || error == StateError::kForbidden;
    report.canceled = error == StateError::kCanceled;
    report.operations.push_back(Fail(0, std::string(), StateAction::kNoOp, error, message));
    // `Fail` prefixes the rom and the state, and there is neither: this failure
    // is about the listing.
    report.operations.back().message = message;
    report.warnings.push_back(message);
    return report;
  }


  // The server rows a local state claimed. What is left over is what this
  // console does not have yet.
  std::vector<bool> claimed(server_states.size(), false);

  const auto record = [&report](StateOperationResult result) {
    switch (result.outcome) {
      case StateOutcome::kFailed:
        ++report.failed;
        report.warnings.push_back(result.message);
        break;
      case StateOutcome::kKeptBoth:
        ++report.kept_both;
        report.warnings.push_back(result.message);
        break;
      case StateOutcome::kCanceled:
        report.canceled = true;
        break;
      case StateOutcome::kNoOp:
      case StateOutcome::kUploaded:
      case StateOutcome::kDownloaded:
        ++report.completed;
        break;
    }
    const bool refused_the_token =
        result.error == StateError::kUnauthorized || result.error == StateError::kForbidden;
    report.unauthorized = report.unauthorized || refused_the_token;
    const bool stop = result.outcome == StateOutcome::kCanceled || refused_the_token;
    report.operations.push_back(std::move(result));
    return !stop;
  };

  // A 401, a 403 or a cancellation stops the run where it stands -- everything
  // after it would end the same way -- but the trim below still happens: it is
  // local, and a baseline over the bound is one `SaveBaseline` refuses whole.
  bool stopped = false;
  for (const scan::StateFile& file : report.scan.states) {
    if (stopped) {
      break;
    }
    if (options.cancel != nullptr && options.cancel->canceled()) {
      // At a boundary, never mid-write.
      report.canceled = true;
      break;
    }
    const ServerState* server = FindServerState(server_states, file.rom_id, file.file_name);
    if (server != nullptr) {
      claimed[static_cast<std::size_t>(server - server_states.data())] = true;
    }
    const state::StateRecord* row = baseline->FindState(file.rom_id, file.file_name);

    std::string why;
    const StateAction action = Decide(row, server, file, &why);
    switch (action) {
      case StateAction::kNoOp: {
        StateOperationResult result;
        result.action = action;
        result.rom_id = file.rom_id;
        result.file_name = file.file_name;
        result.sd_path = file.sd_path;
        result.state_id = server->id;
        result.message = Describe(file.rom_id, file.file_name) + ": nothing to do";
        if (!record(std::move(result))) {
          stopped = true;
        }
        break;
      }
      case StateAction::kKeepBoth:
        if (!record(KeepBoth(file.rom_id, file.file_name, file.sd_path, std::move(why)))) {
          stopped = true;
        }
        break;
      case StateAction::kUpload: {
        StateOperationResult result = Upload(client, files, token, file, action, options);
        if (result.outcome == StateOutcome::kUploaded) {
          Advance(files, &file, result.server, file.sd_path, baseline, &report.warnings);
        }
        if (!record(std::move(result))) {
          stopped = true;
        }
        break;
      }
      case StateAction::kDownload: {
        StateOperationResult result =
            Fetch(client, files, token, *server, file.sd_path, action, options, &report.warnings);
        if (result.outcome == StateOutcome::kDownloaded) {
          Advance(files, &file, *server, file.sd_path, baseline, &report.warnings);
        }
        if (!record(std::move(result))) {
          stopped = true;
        }
        break;
      }
      case StateAction::kPlace:
        break;  // never decided for a local file
    }
  }

  // Whatever the server holds that no local file claimed. Placing one overwrites
  // nothing, so it is the one branch that needs no arbitration at all.
  for (std::size_t at = 0; at < server_states.size() && !stopped; ++at) {
    if (claimed[at]) {
      continue;
    }
    if (options.cancel != nullptr && options.cancel->canceled()) {
      report.canceled = true;
      break;
    }
    const ServerState& server = server_states[at];
    if (index.ById(server.rom_id) == nullptr) {
      // A state for a rom this console does not have. Silent: it is the normal
      // shape of a shared library, not something the user can act on.
      continue;
    }
    const std::string sd_path = options.place != nullptr ? options.place(server) : std::string();
    if (sd_path.empty()) {
      if (!record(Fail(server.rom_id, server.file_name, StateAction::kPlace,
                       StateError::kNoPlacement,
                       "the server holds a state this console does not, and nothing said where a "
                       "new one should go"))) {
        break;
      }
      continue;
    }
    StateOperationResult result =
        Fetch(client, files, token, server, sd_path, StateAction::kPlace, options,
              &report.warnings);
    if (result.outcome == StateOutcome::kDownloaded) {
      Advance(files, nullptr, server, sd_path, baseline, &report.warnings);
    }
    if (!record(std::move(result))) {
      break;
    }
  }

  report.rows_dropped = TrimStates(report.operations, baseline);
  if (report.rows_dropped > 0) {
    report.warnings.push_back(
        std::to_string(report.rows_dropped) +
        " state row(s) were dropped from the baseline to keep it inside the " +
        std::to_string(state::kMaxRecords) +
        " rows a state.db can be read back with; those states are compared from scratch next tick");
  }
  return report;
}

}  // namespace rommsync::sync

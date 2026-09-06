// Executing a plan: the file where a save can be destroyed, and the one place
// the order of two operations is the whole guarantee.
//
// Read `sync_execute.hpp` first -- the four traps are named there. What is only
// here is the order every overwrite goes through, which is the same in the
// download branch and the conflict branch because it is the same rule:
//
//   1. fetch to `io::TempPathFor(<save>)`, never to `<save>`
//   2. verify those bytes against the digest the server reported
//   3. copy the save's current bytes into `.backup/`
//   4. `io::CommitStaged` the verified bytes onto the save
//
// Every step before 4 can fail without the save changing at all, and step 4
// cannot start until step 3 has succeeded. That is docs/SYNC_PROTOCOL.md's hard
// rule expressed as a sequence rather than as a promise, and
// `harness::Sandbox`'s teardown audit is what holds it to it.
#include "rommsync/sync_execute.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/state_db.hpp"

namespace rommsync::sync {
namespace {

constexpr const char* kSavesPath = "/api/saves";

/// How many `-1`, `-2`, ... a backup name may try before giving up.
///
/// A bound rather than a loop, on the reasoning `fs::kMaxDirectoryEntries`
/// gives: this runs over a FAT32 card that gets yanked mid-write, and a
/// directory that answers "yes, that exists" to everything must cost a named
/// failure rather than a tick that never ends. Reaching it means a thousand
/// backups of one save in one second, which is not a card in a state worth
/// overwriting a save on.
constexpr int kMaxBackupAttempts = 1000;

/// Percent-encode one query-string value.
///
/// A `slot` is derived (`retroarch-srm`) but an `emulator` is a folder name a
/// human chose and a slot on an operation is whatever another client sent, so
/// neither may be pasted into a URL: a space makes a request line no server
/// parses, and an `&` moves the rest of the value into a parameter of its own --
/// which on this endpoint would mean uploading a save under someone else's
/// `rom_id`. Only RFC 3986's unreserved set survives.
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

/// How this operation is named in a log line: the rom and the slot, which are
/// the pair a user can go and look at. Never the save's own name, which is a
/// game title, and never a token.
std::string Describe(const SyncOperation& operation) {
  return std::string(ToString(operation.action)) + " rom " + std::to_string(operation.rom_id) +
         ", slot " + (operation.slot.has_value() ? *operation.slot : "<archival>");
}

OperationResult Fail(const SyncOperation& operation, OperationError error, std::string message) {
  OperationResult result;
  result.action = operation.action;
  result.rom_id = operation.rom_id;
  result.slot = operation.slot;
  // A cancellation is not a failure -- the caller stopped this, and nothing
  // about the save went wrong. It gets its own outcome so the accounting M2-6
  // reports cannot call it either completed or failed.
  result.outcome = error == OperationError::kCanceled ? OperationOutcome::kCanceled
                                                      : OperationOutcome::kFailed;
  result.error = error;
  result.message = Describe(operation) + ": " + std::move(message);
  return result;
}

/// The reason an exchange did not produce a usable answer, or empty when it did.
///
/// One place, because every call here reads a failure the same way: a transport
/// error is `kTransferFailed` and a status the operation cannot proceed from is
/// `kRefused`, with the status in the message and never the body -- a rejected
/// request may be carrying a bearer token in a header, and RomM's own `detail`
/// is worth having, so it is included only for the statuses that carry one.
std::optional<std::pair<OperationError, std::string>> Refused(const http::Result& result,
                                                              std::string_view what) {
  if (!result.ok()) {
    // A cancelled exchange is kept apart from a failed one. The caller stopped
    // it, so reporting it to `complete` as a failed operation (M2-6) would
    // describe work that went wrong rather than work that was not attempted.
    const OperationError error = result.error == http::Error::kCanceled
                                     ? OperationError::kCanceled
                                     : OperationError::kTransferFailed;
    return std::make_pair(error,
                          std::string(what) + " did not complete: " + http::ToString(result.error) +
                              (result.message.empty() ? "" : " (" + result.message + ")"));
  }
  const int status = result.response.status;
  if (status == 401) {
    // Not this operation's problem, and not the next one's either: the token is
    // gone, so `ExecutePlan` stops here rather than refusing the rest of the
    // plan one request at a time (`sync_execute.hpp`, `download::Drain`).
    return std::make_pair(OperationError::kUnauthorized,
                          std::string(what) +
                              " was rejected: HTTP 401; the token has been revoked");
  }
  if (status == 403) {
    // The same stop, a different sentence: a scope this pairing was not granted
    // rather than a pairing that is gone (docs/AUTH.md#scopes-to-request).
    return std::make_pair(OperationError::kForbidden,
                          std::string(what) +
                              " was rejected: HTTP 403; this pairing was not granted the scopes "
                              "sync needs");
  }
  if (!result.successful()) {
    return std::make_pair(OperationError::kRefused,
                          std::string(what) + " was refused: HTTP " + std::to_string(status));
  }
  return std::nullopt;
}

/// The `id` off a `SaveSchema` body, or empty.
std::optional<std::int64_t> SaveIdOf(const json::Value& save) {
  const json::Value* id = save.Find("id");
  if (id == nullptr || !id->is_integer() || id->integer() <= 0) {
    return std::nullopt;
  }
  return id->integer();
}

http::Request Authed(http::Method method, std::string url, const auth::StoredToken& token,
                     std::chrono::milliseconds timeout, const http::CancelToken* cancel) {
  http::Request request;
  request.method = method;
  request.url = std::move(url);
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.timeout = timeout;
  request.cancel = cancel;
  return request;
}

/// The staged download, removed unless something takes it.
///
/// `Fetch` has five ways out between staging a body and committing it, and each
/// one has to remove those bytes -- an unverified or unplaceable download is not
/// a save. One branch forgetting would leave a `<save>.tmp` for the next tick to
/// reason about, and a `.tmp` beside a save is supposed to mean "verified bytes
/// that never landed" (issue #16).
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

  /// `io::CommitStaged` consumes the file whether it succeeds or fails, so the
  /// guard has nothing left to remove either way.
  void Release() { path_.clear(); }

 private:
  std::string path_;
};

/// What the server currently holds for `save_id`: its size, for
/// `DownloadTarget::expected_size`, and its digest.
///
/// A negotiate operation carries neither -- `SyncOperationSchema` has nine
/// fields and `file_size_bytes` is not one of them -- and without a size a body
/// that ends cleanly and early is indistinguishable from a complete one
/// (`http::DownloadTarget::expected_size`, and the `truncate` mode in
/// server/testing/fault_proxy.py, which deliberately sends no length).
///
/// Only the *existence* of the row is required. A body this cannot read leaves
/// both fields empty and is a warning rather than a failure: the digest
/// comparison in step 2 is the gate that matters, and refusing a download
/// because RomM grew a field would be this client breaking itself on an upgrade.
struct ServerSave {
  std::uint64_t file_size_bytes = 0;
  std::optional<std::string> content_hash;
};

}  // namespace

const char* ToString(OperationOutcome outcome) {
  switch (outcome) {
    case OperationOutcome::kNoOp:
      return "no_op";
    case OperationOutcome::kUploaded:
      return "uploaded";
    case OperationOutcome::kDownloaded:
      return "downloaded";
    case OperationOutcome::kKeptBoth:
      return "kept_both";
    case OperationOutcome::kFailed:
      return "failed";
    case OperationOutcome::kNotUnderstood:
      return "not_understood";
    case OperationOutcome::kCanceled:
      return "canceled";
  }
  return "failed";
}

const char* ToString(OperationError error) {
  switch (error) {
    case OperationError::kNone:
      return "none";
    case OperationError::kNoLocalSave:
      return "no_local_save";
    case OperationError::kNoSaveId:
      return "no_save_id";
    case OperationError::kUnreadableCard:
      return "unreadable_card";
    case OperationError::kBackupFailed:
      return "backup_failed";
    case OperationError::kTransferFailed:
      return "transfer_failed";
    case OperationError::kUnauthorized:
      return "unauthorized";
    case OperationError::kForbidden:
      return "forbidden";
    case OperationError::kRefused:
      return "refused";
    case OperationError::kUnverified:
      return "unverified";
    case OperationError::kCommitFailed:
      return "commit_failed";
    case OperationError::kUnconfirmed:
      return "unconfirmed";
    case OperationError::kCanceled:
      return "canceled";
  }
  return "none";
}

auth::Answer AnswerOf(OperationError error) {
  switch (error) {
    case OperationError::kUnauthorized:
      return auth::Answer::kRejected;
    case OperationError::kForbidden:
      return auth::Answer::kForbidden;
    // The one acceptance: an operation that did what the plan asked did it with
    // this token (the rule is on `auth::AnswerOf(const http::Result&)`).
    case OperationError::kNone:
      return auth::Answer::kAccepted;
    // The rest say nothing, `kRefused` -- a bare 4xx -- included.
    case OperationError::kNoLocalSave:
    case OperationError::kNoSaveId:
    case OperationError::kUnreadableCard:
    case OperationError::kBackupFailed:
    case OperationError::kTransferFailed:
    case OperationError::kRefused:
    case OperationError::kUnverified:
    case OperationError::kCommitFailed:
    case OperationError::kUnconfirmed:
    case OperationError::kCanceled:
      break;
  }
  return auth::Answer::kSilent;
}

std::string BackupFileName(std::int64_t rom_id, const std::optional<std::string>& slot,
                           std::string_view file_name, std::int64_t unix_seconds,
                           int uniquifier) {
  std::string safe_slot;
  if (slot.has_value()) {
    for (const char character : *slot) {
      const unsigned char byte = static_cast<unsigned char>(character);
      const bool keep = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
      safe_slot.push_back(keep ? character : '_');
    }
  }
  if (!slot.has_value()) {
    // Archival, which pairs with nothing. It cannot collide with a derived slot
    // because `scan::SlotFor` always carries an emulator and an extension.
    safe_slot = "archival";
  } else if (safe_slot.empty() || safe_slot == "." || safe_slot == "..") {
    // A slot that survives sanitising as one of the two names a join treats as
    // a directory still needs a name that is a name -- and a *different* one
    // from `archival`, because a save with a `..` slot and a save with no slot
    // at all are two different saves and must not share a backup.
    for (char& character : safe_slot) {
      character = '_';
    }
    safe_slot = safe_slot.empty() ? "slot" : safe_slot;
  }
  std::string name = std::to_string(rom_id) + "-" + safe_slot + "-" + std::to_string(unix_seconds);
  if (uniquifier > 0) {
    name += "-" + std::to_string(uniquifier);
  }
  return name + std::string(ExtensionOf(file_name));
}

const SaveTarget* MatchTarget(const std::vector<SaveTarget>& targets,
                              const SyncOperation& operation) {
  for (const SaveTarget& target : targets) {
    if (target.rom_id == operation.rom_id && target.slot == operation.slot) {
      return &target;
    }
  }
  return nullptr;
}

namespace {

/// Copy whatever is at `sd_path` into `backup_dir`, and say where it went.
///
/// Streamed (`io::CopyAtomically`), because the file is a save and the
/// sysmodule heap is 512 KiB.
///
/// `kSourceMissing` is success with nothing to show for it: a download for a
/// save the client does not have yet overwrites nothing, so there is nothing to
/// protect. Every other failure is a refusal to go on -- including a missing
/// `.backup/`, which `core/` cannot create and which therefore stops the
/// overwrite instead of proceeding without a copy.
OperationError BackUp(fs::FileSystem& files, const ExecuteOptions& options,
                      const SyncOperation& operation, const SaveTarget& target,
                      std::string* backup_sd_path, std::string* message) {
  const std::string source = files.Resolve(target.sd_path);
  if (source.empty()) {
    *message = "the local save " + target.sd_path + " is not a path on this card";
    return OperationError::kUnreadableCard;
  }

  // Whether there is anything to protect is `io::CopyAtomically`'s answer and
  // not `io::Exists`'s. The two draw different lines on purpose: `Exists` is
  // `fopen` succeeding, so a save the card would not open *this once* -- a full
  // handle table, an emulator holding it, an EIO -- reads as absent, and
  // "absent" here would mean overwriting the only copy with no backup. The copy
  // reads `errno` and says `kSourceMissing` for ENOENT/ENOTDIR alone.
  const std::int64_t stamp =
      UnixSeconds(options.now != nullptr ? options.now() : std::chrono::system_clock::now());
  for (int attempt = 0; attempt < kMaxBackupAttempts; ++attempt) {
    const std::string sd_path =
        options.backup_dir + "/" +
        BackupFileName(operation.rom_id, operation.slot, target.file_name, stamp, attempt);
    const std::string destination = files.Resolve(sd_path);
    if (destination.empty()) {
      *message = "the backup directory " + options.backup_dir + " is not a path on this card";
      return OperationError::kUnreadableCard;
    }
    if (io::Exists(destination)) {
      // A name already taken is a backup of *something*. This module does not
      // get to decide it is worthless, so it steps past rather than over.
      // Best-effort, and it is the destination rather than the save: the worst
      // an unopenable-but-present backup here costs is that older backup, and
      // the standard library offers no create-if-absent to do better with.
      continue;
    }
    const io::CopyResult copied = io::CopyAtomically(source, destination);
    if (copied.error == io::CopyError::kSourceMissing) {
      // There is no save at that path -- a download for a save this client does
      // not have yet. It overwrites nothing, so there is nothing to preserve.
      return OperationError::kNone;
    }
    if (!copied.ok()) {
      *message = "the save's previous bytes could not be preserved: " + copied.message;
      return OperationError::kBackupFailed;
    }
    *backup_sd_path = sd_path;
    return OperationError::kNone;
  }
  *message = "there are already " + std::to_string(kMaxBackupAttempts) +
             " backups of this save under " + options.backup_dir + " for this second";
  return OperationError::kBackupFailed;
}

/// `POST /api/saves?...&overwrite=true` with the local file as `saveFile`.
OperationResult Upload(http::HttpClient& client, fs::FileSystem& files,
                       const auth::StoredToken& token, const SyncPlan& plan,
                       const SyncOperation& operation, const SaveTarget* target,
                       const ExecuteOptions& options) {
  if (target == nullptr) {
    return Fail(operation, OperationError::kNoLocalSave,
                "there is no local save for this (rom_id, slot) to upload");
  }
  const std::string source = files.Resolve(target->sd_path);
  if (source.empty() || !io::Exists(source)) {
    return Fail(operation, OperationError::kUnreadableCard,
                "the local save " + target->sd_path + " could not be opened to upload");
  }

  // `overwrite=true` is not optional; see `Refused`. `session_id` ties the
  // upload to this negotiation and `device_id` is what writes the `device_syncs`
  // row the *next* negotiation arbitrates against -- skip either and the device
  // stays in the no-history branch forever (docs/API_CONTRACT.md).
  std::string url = http::JoinUrl(token.server_url, kSavesPath) +
                    "?rom_id=" + std::to_string(operation.rom_id);
  if (operation.emulator.has_value()) {
    url += "&emulator=" + EncodeQuery(*operation.emulator);
  }
  if (operation.slot.has_value()) {
    url += "&slot=" + EncodeQuery(*operation.slot);
  }
  url += "&session_id=" + std::to_string(plan.session_id);
  url += "&device_id=" + EncodeQuery(token.device_id);
  url += "&overwrite=true";

  http::Request request =
      Authed(http::Method::kPost, std::move(url), token, options.timeout, options.cancel);
  http::FormPart part;
  part.name = "saveFile";
  part.file_path = source;
  // The client's own name, not the server's: RomM stamps its own datetime tag
  // on ingest either way, and the name it stamps is derived from this one.
  part.file_name = target->file_name;
  part.content_type = "application/octet-stream";
  request.form.push_back(std::move(part));

  const http::Result result = client.Send(request);
  if (const auto refused = Refused(result, "the upload")) {
    std::string message = refused->second;
    if (result.response.status == 409) {
      // The one status worth spelling out, and only here: RomM answers it when
      // this device has no sync row for the slot's current save, which is
      // exactly the state the plan calls `Client save is newer (no sync
      // history)`. A plan executed as issued must never draw one, so seeing it
      // means the request went out without `overwrite=true`.
      message += "; the slot has a newer save and the upload did not say overwrite=true";
    }
    return Fail(operation, refused->first, std::move(message));
  }

  OperationResult uploaded;
  uploaded.action = operation.action;
  uploaded.rom_id = operation.rom_id;
  uploaded.slot = operation.slot;
  uploaded.outcome = OperationOutcome::kUploaded;
  uploaded.sd_path = target->sd_path;
  uploaded.message = Describe(operation) + ": uploaded " + target->sd_path;
  const json::ParseResult document = json::Parse(result.response.body);
  if (document.ok()) {
    // Read for the report, not to decide anything: the save is on the server
    // whether or not this client can read the row back, and failing the
    // operation over an unreadable answer would have the next tick upload it a
    // second time -- which RomM stores as a second save row.
    uploaded.save_id = SaveIdOf(document.value);
  }
  return uploaded;
}

/// `GET /api/saves/{id}` -- the size and digest the download is judged against.
ServerSave DescribeServerSave(http::HttpClient& client, const auth::StoredToken& token,
                              std::int64_t save_id, const ExecuteOptions& options,
                              std::vector<std::string>* warnings, std::string_view where) {
  ServerSave described;
  const http::Result result =
      client.Send(Authed(http::Method::kGet,
                         http::JoinUrl(token.server_url, std::string(kSavesPath) + "/" +
                                                      std::to_string(save_id)),
                         token, options.timeout, options.cancel));
  if (!result.successful()) {
    warnings->push_back(std::string(where) +
                        ": the server's save row could not be read, so the download has no "
                        "expected size to catch a short body with");
    return described;
  }
  const json::ParseResult document = json::Parse(result.response.body);
  if (!document.ok()) {
    warnings->push_back(std::string(where) + ": the server's save row did not parse (" +
                        document.error.Describe() + ")");
    return described;
  }
  const json::Value* size = document.value.Find("file_size_bytes");
  if (size != nullptr && size->is_integer() && size->integer() > 0) {
    described.file_size_bytes = static_cast<std::uint64_t>(size->integer());
  }
  const json::Value* hash = document.value.Find("content_hash");
  if (hash != nullptr && hash->is_string() && !hash->string().empty()) {
    described.content_hash = hash->string();
  }
  return described;
}

/// The download half of `download` and of `conflict`: fetch, verify, back up,
/// commit, confirm. The two differ in what they are called and in nothing else
/// -- keeping both *is* writing the server's copy over a local file whose
/// previous bytes are safe (docs/SYNC_PROTOCOL.md#conflicts).
OperationResult Fetch(http::HttpClient& client, fs::FileSystem& files,
                      const auth::StoredToken& token, const SyncOperation& operation,
                      const SaveTarget* target, const ExecuteOptions& options,
                      OperationOutcome success, std::vector<std::string>* warnings) {
  if (!operation.save_id.has_value()) {
    return Fail(operation, OperationError::kNoSaveId,
                "the plan says to fetch a save the server named no id for");
  }
  const std::int64_t save_id = *operation.save_id;

  // Where it goes. A matched local file keeps its own path; a save the client
  // has never seen needs one invented, which the plan cannot do -- the server's
  // `file_name` is tagged and the directory depends on the rom's platform.
  SaveTarget placed;
  if (target == nullptr) {
    const std::string sd_path = options.place != nullptr ? options.place(operation) : std::string();
    if (sd_path.empty()) {
      return Fail(operation, OperationError::kNoLocalSave,
                  "there is no local save for this (rom_id, slot) and nothing said where a new "
                  "one should go");
    }
    placed.rom_id = operation.rom_id;
    placed.slot = operation.slot;
    placed.sd_path = sd_path;
    // The extension the backup would take, from the path the caller chose. The
    // server's `file_name` is never used for this: it carries RomM's ingest tag.
    const std::size_t leaf = sd_path.rfind('/');
    placed.file_name = leaf == std::string::npos ? sd_path : sd_path.substr(leaf + 1);
    target = &placed;
  }

  const std::string destination = files.Resolve(target->sd_path);
  if (destination.empty()) {
    return Fail(operation, OperationError::kUnreadableCard,
                "the save path " + target->sd_path + " is not a path on this card");
  }

  const ServerSave described =
      DescribeServerSave(client, token, save_id, options, warnings, Describe(operation));

  // Staged beside the save rather than at it. `http::DownloadTarget` renames
  // its `.part` onto the destination the instant the body ends, and that is
  // before anything has checked that the bytes are the save the plan meant.
  // A stale one from an interrupted run is a destination the Horizon rename
  // refuses -- it is not a replace -- so it goes first. The bytes in it are a
  // download that never completed and belong to nothing.
  StagedFile staged(io::TempPathFor(destination));
  std::remove(staged.path().c_str());

  http::Request request = Authed(http::Method::kGet,
                                 http::JoinUrl(token.server_url, std::string(kSavesPath) + "/" +
                                                              std::to_string(save_id) + "/content"),
                                 token, options.timeout, options.cancel);
  // A save state is tens of megabytes on a link that may be a phone hotspot, so
  // the ceiling on a content transfer is "no bytes moved for this long" rather
  // than a total (`http::Request::stall_timeout`).
  request.timeout = std::chrono::milliseconds{0};
  request.stall_timeout = options.stall_timeout;

  http::DownloadTarget into;
  into.path = staged.path();
  // Never resumed. A save is small enough to refetch, and a resumed transfer of
  // a file whose server copy may have moved on is two different saves spliced
  // at a byte offset.
  into.resume = false;
  into.expected_size = described.file_size_bytes;

  const http::Result fetched = client.Download(request, into);
  if (const auto refused = Refused(fetched, "the download")) {
    // The backend leaves its partial file behind for a resume, and there is
    // never going to be one (`into.resume` below), so it is litter beside a
    // save rather than progress towards one -- and nothing else would ever
    // remove it. `<path>.part` is `http::DownloadTarget`'s documented staging
    // name, not a guess. The staged file goes with it, by the guard above: an
    // incomplete download is not a save.
    std::remove((staged.path() + ".part").c_str());
    return Fail(operation, refused->first, refused->second);
  }

  // Verify before anything replaces a save. The plan's digest wins over the
  // row's: it is the copy the server arbitrated on, so bytes that match the row
  // but not the plan are a save that changed *since* the plan was made, and the
  // right answer to that is to fail and let the next tick negotiate again.
  const std::optional<std::string>& reported =
      operation.server_content_hash.has_value() ? operation.server_content_hash
                                                : described.content_hash;
  if (!reported.has_value() || !IsContentHash(*reported)) {
    // Nothing to compare against, so the bytes are unverified -- and unverified
    // bytes do not replace a save. RomM computes the digest on ingest, so this
    // is a save some other tool put a SHA1 or an uppercase digest against
    // (docs/API_CONTRACT.md); it costs that one save every tick until the digest
    // is fixed, and it says so, which is the cheaper of the two mistakes.
    return Fail(operation, OperationError::kUnverified,
                "the server reports no comparable MD5 for this save, so the download could not be "
                "verified; the local file was not touched");
  }
  const state::HashOutcome digest = state::HashFile(staged.path());
  if (!digest.ok()) {
    return Fail(operation, OperationError::kUnreadableCard,
                "the downloaded bytes could not be hashed to verify them: " + digest.message);
  }
  if (digest.content_hash != *reported) {
    return Fail(operation, OperationError::kUnverified,
                "the downloaded bytes are not the save the plan described; the local file was "
                "not touched");
  }

  OperationResult result;
  result.action = operation.action;
  result.rom_id = operation.rom_id;
  result.slot = operation.slot;
  result.sd_path = target->sd_path;
  result.save_id = save_id;

  std::string message;
  const OperationError backed_up =
      BackUp(files, options, operation, *target, &result.backup_sd_path, &message);
  if (backed_up != OperationError::kNone) {
    OperationResult failed = Fail(operation, backed_up, std::move(message));
    failed.sd_path = target->sd_path;
    return failed;
  }

  const std::string staged_path = staged.path();
  // Consumed by the commit whether it succeeds or fails, so the guard is done.
  staged.Release();
  const io::WriteResult committed = io::CommitStaged(staged_path, destination);
  if (!committed.ok()) {
    OperationResult failed =
        Fail(operation, OperationError::kCommitFailed,
             "the verified bytes could not be put in place: " + committed.message);
    failed.sd_path = target->sd_path;
    failed.backup_sd_path = result.backup_sd_path;
    return failed;
  }

  // The save is on the card. What is left is telling the server so -- without
  // it, this device has no sync row for the save and every later negotiation
  // falls into the no-history branch (docs/API_CONTRACT.md).
  http::Request confirm =
      Authed(http::Method::kPost,
             http::JoinUrl(token.server_url,
                    std::string(kSavesPath) + "/" + std::to_string(save_id) + "/downloaded"),
             token, options.timeout, options.cancel);
  confirm.headers.push_back({"Content-Type", "application/json"});
  confirm.body = "{\"device_id\":" + json::Quote(token.device_id) + "}";
  const http::Result confirmed = client.Send(confirm);
  if (const auto refused = Refused(confirmed, "the download confirmation")) {
    // Counted failed on purpose, even though the bytes are correct: as far as
    // arbitration is concerned this device still has not seen the save, and a
    // baseline advanced for it (M2-6) would record a sync that the server has
    // no record of.
    OperationResult failed = Fail(operation, OperationError::kUnconfirmed,
                                  refused->second + "; the save itself is correct on the card");
    failed.sd_path = target->sd_path;
    failed.backup_sd_path = result.backup_sd_path;
    failed.save_id = save_id;
    return failed;
  }

  result.outcome = success;
  result.message = Describe(operation) + ": wrote the server's copy to " + target->sd_path +
                   (result.backup_sd_path.empty()
                        ? std::string(", replacing nothing")
                        : ", after backing up its previous bytes to " + result.backup_sd_path);
  return result;
}

}  // namespace

ExecutionReport ExecutePlan(http::HttpClient& client, fs::FileSystem& files,
                            const auth::StoredToken& token, const SyncPlan& plan,
                            const std::vector<SaveTarget>& targets,
                            const ExecuteOptions& options) {
  ExecutionReport report;
  report.operations.reserve(plan.operations.size());

  for (const SyncOperation& operation : plan.operations) {
    if (options.cancel != nullptr && options.cancel->canceled()) {
      // At an operation boundary, never mid-write: everything above either
      // completed or left the save exactly as it was.
      report.canceled = true;
      break;
    }

    OperationResult result;
    if (!operation.known_action) {
      // M2-4 hands an action this build does not know over as a `no_op` so that
      // the default branch cannot overwrite a save. Counting it as an ordinary
      // completed no-op would report that the client did what was asked, and it
      // does not know what was asked.
      result.action = operation.action;
      result.rom_id = operation.rom_id;
      result.slot = operation.slot;
      result.outcome = OperationOutcome::kNotUnderstood;
      result.message = "rom " + std::to_string(operation.rom_id) + ", slot " +
                       (operation.slot.has_value() ? *operation.slot : "<archival>") +
                       ": the action \"" + operation.action_text +
                       "\" is not one this build knows; nothing was done";
    } else {
      const SaveTarget* target = MatchTarget(targets, operation);
      switch (operation.action) {
        case Action::kUpload:
          result = Upload(client, files, token, plan, operation, target, options);
          break;
        case Action::kDownload:
          result = Fetch(client, files, token, operation, target, options,
                         OperationOutcome::kDownloaded, &report.warnings);
          break;
        case Action::kConflict:
          // Keep both: the server's copy lands on the card and the local bytes
          // are left under `.backup/` for the overlay (M7-1). RomM sends no
          // resolution field and the client invents none.
          result = Fetch(client, files, token, operation, target, options,
                         OperationOutcome::kKeptBoth, &report.warnings);
          break;
        case Action::kNoOp:
          result.action = operation.action;
          result.rom_id = operation.rom_id;
          result.slot = operation.slot;
          result.outcome = OperationOutcome::kNoOp;
          result.message = Describe(operation) + ": nothing to do (" +
                           ToString(operation.reason) + ")";
          break;
      }
    }

    // Enumerated rather than defaulted: a `default` here would count whatever
    // outcome is added next as work the client completed, which is the one
    // direction this accounting must not be wrong in (M2-6 reports it).
    switch (result.outcome) {
      case OperationOutcome::kFailed:
        ++report.failed;
        report.warnings.push_back(result.message);
        break;
      case OperationOutcome::kNotUnderstood:
        ++report.not_understood;
        report.warnings.push_back(result.message);
        break;
      case OperationOutcome::kCanceled:
        // Counted nowhere: the caller stopped this one, and the rest are not
        // attempted at all.
        report.canceled = true;
        break;
      case OperationOutcome::kNoOp:
      case OperationOutcome::kUploaded:
      case OperationOutcome::kDownloaded:
      case OperationOutcome::kKeptBoth:
        ++report.completed;
        break;
    }
    // The two failures that are not about one operation. A cancellation was
    // never attempted; a 401 or a 403 was, and failed -- so it is counted above,
    // and only the operations *after* it go unattempted.
    const bool refused_the_token = result.error == OperationError::kUnauthorized ||
                                   result.error == OperationError::kForbidden;
    report.unauthorized = report.unauthorized || refused_the_token;
    const bool stop = result.outcome == OperationOutcome::kCanceled || refused_the_token;
    report.operations.push_back(std::move(result));
    if (stop) {
      break;
    }
  }

  return report;
}

}  // namespace rommsync::sync

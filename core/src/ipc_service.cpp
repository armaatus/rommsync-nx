// `ServiceCore` and the dispatch table -- the logic half of the IPC boundary.
//
// Split from ipc.cpp, which is the codecs, because the two are consumed by
// different halves: the overlay links the codecs and never this file, and the
// sysmodule links both. Keeping them apart makes that visible in the link map
// rather than only in a comment.
//
// Everything a command decides is here, so it is decided on the host under
// `ctest` rather than on a console with no debugger: which of the five
// `SyncNow` outcomes applies, what the *effective* enabled state is after a
// write, whether a folder map fits a payload, and what a page size is clamped
// to. `sysmodule/source/ipc/` is buffers and a `Result` on top of this.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/pairing.hpp"

namespace rommsync::ipc {
namespace {

config::Diagnostic Note(config::Severity severity, std::string section, std::string key,
                        std::string message) {
  config::Diagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.section = std::move(section);
  diagnostic.key = std::move(key);
  diagnostic.message = std::move(message);
  return diagnostic;
}

/// Cut `text` to `limit` bytes on a UTF-8 boundary, visibly.
///
/// The same job `ipc.cpp`'s `Shorten` does for a diagnostic, applied to the two
/// other places a value the client does not control reaches a payload: a rom's
/// `fs_name`, which comes off a RomM library, and a verification path, which
/// comes off a server response.
void Shorten(std::string* text, std::size_t limit) {
  if (text->size() <= limit) {
    return;
  }
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>((*text)[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  text->resize(cut);
  *text += "...";
}

/// `WriteOutcome` for what the engine reported.
///
/// `kInvalid` is the only failure with a distinct meaning to a user -- the edit
/// was refused and the file is untouched. Everything else, `kInternal` included,
/// is reported as a write that did not happen: that is the true and actionable
/// half of any of them, and inventing a fourth outcome for "the engine could not
/// say" would be a screen nobody can write a sentence for.
WriteOutcome ToOutcome(Error error) {
  switch (error) {
    case Error::kOk:
      return WriteOutcome::kApplied;
    case Error::kInvalid:
      return WriteOutcome::kInvalid;
    default:
      return WriteOutcome::kWriteFailed;
  }
}

}  // namespace

ServiceCore::ServiceCore(Engine& engine) : engine_(engine) {}

std::uint32_t ServiceCore::GetInterfaceVersion() const { return kVersion; }

Status ServiceCore::GetStatus() const {
  const EngineSnapshot snapshot = engine_.Snapshot();
  const config::Config& config = engine_.config();

  Status status;
  status.interface = kVersion;
  status.build = version();
  Shorten(&status.build, kMaxNameBytes);
  // Read off the config rather than off the snapshot: the switch the overlay
  // draws is the one on the card, and `SetEnabled` is what moves it.
  status.enabled = config.sync.enabled;
  status.auth = snapshot.auth;
  status.configured = config.configured();
  status.online = snapshot.online;
  status.last_sync_at = snapshot.last_sync_at;
  status.last_sync_result = snapshot.last_sync_result;
  status.sync_in_progress = snapshot.sync_in_progress;
  status.uploaded = snapshot.uploaded;
  status.downloaded = snapshot.downloaded;
  status.conflicts = snapshot.conflicts;
  status.failed = snapshot.failed;
  status.queue_depth = snapshot.queue_depth;
  // Counted here rather than carried on the snapshot: the diagnostics are the
  // engine's own copy of what it read (`Engine::config_diagnostics`), and a
  // second count kept beside them is a second thing to get out of step.
  status.config_error_count = 0;
  for (const config::Diagnostic& diagnostic : engine_.config_diagnostics()) {
    if (diagnostic.severity == config::Severity::kError) {
      ++status.config_error_count;
    }
  }
  status.download = snapshot.download;
  // A `fs_name` comes off a RomM library, so its length is not ours to assume,
  // and `GetStatus` is documented never to fail. A truncated label costs a few
  // characters; an unbounded one costs the whole status screen.
  Shorten(&status.download.fs_name, kMaxNameBytes);
  return status;
}

ConfigView ServiceCore::GetConfig() const {
  ConfigView view;
  view.config = engine_.config();

  std::vector<config::Diagnostic> diagnostics = engine_.config_diagnostics();

  // A URL is bounded by nothing in `config.ini` (config.hpp bounds the file, not
  // the line), and this command may not fail -- so an absurd one is withheld and
  // named instead of making the payload unsendable. `Status::configured` still
  // reports the truth, and the value itself is never quoted back: a URL is the
  // one configured field that can carry a credential (docs/SECURITY.md).
  if (view.config.server.url.size() > kMaxServerUrlBytes) {
    view.config.server.url.clear();
    diagnostics.push_back(Note(config::Severity::kError, "server", "url",
                               "the configured server URL is too long to send over IPC; "
                               "edit config.ini directly"));
  }

  view.diagnostics = TrimDiagnostics(diagnostics);

  // The folder map is the one section that grows with what the user wrote, so it
  // is the one that gets dropped -- whole, and flagged. An overlay that rendered
  // a silently empty map would tell the user they had configured no folders.
  if (!Fits(EncodeConfigView(view))) {
    view.config.platforms.clear();
    view.platforms_truncated = true;
    // At the *front*, so the trimming below cannot be what drops the sentence
    // explaining the gap -- which is exactly what appending it did.
    diagnostics.insert(diagnostics.begin(),
                       Note(config::Severity::kNotice, "", "",
                            "the folder map is too large to send over IPC; "
                            "read config.ini for it"));
    view.diagnostics = TrimDiagnostics(diagnostics);
  }

  // ...and now the last thing that can still overflow: the diagnostics
  // themselves. `TrimDiagnostics` bounds them in *characters*, and `json::Quote`
  // escapes -- a backslash doubles, a control character becomes six bytes -- and
  // `config.cpp` quotes the rejected path back, which may hold either. So the
  // encoded length is not a function of the trim constants, and this command may
  // not fail. Drop complaints until it fits, and say how many went.
  std::size_t dropped = 0;
  while (!Fits(EncodeConfigView(view)) && !view.diagnostics.empty()) {
    view.diagnostics.pop_back();
    ++dropped;
  }
  if (dropped > 0) {
    // Spend one more slot on the notice rather than adding to a list that only
    // just fits -- and if even that is too much, the count is lost but the
    // config is not, which is the trade this command exists to make.
    if (!view.diagnostics.empty()) {
      view.diagnostics.pop_back();
      ++dropped;
    }
    view.diagnostics.push_back(
        Note(config::Severity::kNotice, "", "",
             std::to_string(dropped) +
                 " diagnostics did not fit this payload; read config.ini for them"));
    if (!Fits(EncodeConfigView(view))) {
      view.diagnostics.clear();
    }
  }
  return view;
}

ConfigResult ServiceCore::SetConfig(const ConfigEdit& edit) {
  std::vector<config::Diagnostic> raw;
  ConfigResult result;
  result.outcome = ToOutcome(engine_.ApplyConfigEdit(edit, &raw));
  result.diagnostics = TrimDiagnostics(raw);
  // The same overflow `GetConfig` guards against, from the same source: these
  // diagnostics quote what the user just typed.
  while (!Fits(EncodeConfigResult(result)) && !result.diagnostics.empty()) {
    result.diagnostics.pop_back();
  }
  return result;
}

EnabledResult ServiceCore::SetEnabled(bool enabled) {
  EnabledResult result;
  result.outcome = ToOutcome(engine_.SetSyncEnabled(enabled));
  // Read back rather than assume. A bare success would have the overlay draw the
  // state it *asked* for rather than the one that took, which is the whole
  // reason this command answers with anything at all (#24).
  result.enabled = engine_.config().sync.enabled;
  return result;
}

SyncOutcome ServiceCore::SyncNow() {
  const config::Config& config = engine_.config();
  // In remedy order, which is not the order the fields happen to be in: a
  // console with no server cannot meaningfully be "unauthenticated", and one
  // that has never paired cannot be told its problem is a switch. Each answer is
  // the *first* thing the user has to fix.
  if (!config.configured()) {
    return SyncOutcome::kNotConfigured;
  }
  if (engine_.Snapshot().auth != AuthState::kPaired) {
    return SyncOutcome::kUnauthenticated;
  }
  if (!config.sync.enabled) {
    return SyncOutcome::kDisabled;
  }
  return engine_.RequestSync() ? SyncOutcome::kAccepted : SyncOutcome::kAlreadyRunning;
}

Error ServiceCore::StartPair(auth::PairingStatus* status) {
  if (!engine_.config().configured()) {
    return Error::kNotConfigured;
  }
  const Error error = engine_.StartPairing();
  if (error != Error::kOk) {
    return error;
  }
  // The attempt as it stands one instant later, which is `kStarting` -- the init
  // request has not come back, and this command does not wait for it.
  *status = Bounded(engine_.pairing_status());
  return Error::kOk;
}

auth::PairingStatus ServiceCore::GetPairState() const { return Bounded(engine_.pairing_status()); }

auth::PairingStatus ServiceCore::Bounded(auth::PairingStatus status) {
  // `auth.cpp` reads `verification_path` straight off the server's JSON with no
  // length limit, and this command is documented never to fail. A URL too long
  // to send is withheld and named rather than sent: a pairing screen that says
  // "your server answered something unusable" is a diagnosis, and one that
  // cannot answer at all is a hang.
  if (status.verification_url.size() > kMaxVerificationUrlBytes ||
      status.verification_url_complete.size() > kMaxVerificationUrlBytes) {
    status.verification_url.clear();
    status.verification_url_complete.clear();
    status.message = "the server answered a verification URL too long to show";
  }
  // The last resort. `message` is ours and is short, but it is the one field
  // left that could still be carrying something unexpected.
  if (!Fits(auth::SerializePairingStatus(status))) {
    status.message.clear();
  }
  return status;
}

Error ServiceCore::Unpair() { return engine_.Unpair(); }

Error ServiceCore::Enqueue(std::int64_t rom_id, std::int32_t* position) {
  // The decoder already refuses a non-positive id, so this is for a caller that
  // reached `ServiceCore` directly. `kUnknownRom` rather than `kMalformedRequest`
  // because from here it is indistinguishable from an id the library does not
  // have.
  if (rom_id <= 0) {
    return Error::kUnknownRom;
  }
  return engine_.Enqueue(rom_id, position);
}

Error ServiceCore::Dequeue(std::int64_t rom_id) {
  if (rom_id <= 0) {
    return Error::kNotQueued;
  }
  return engine_.Dequeue(rom_id);
}

Error ServiceCore::ListBegin(const ListRequest& request, Cursor* cursor) {
  ListRequest clamped = request;
  // Clamped here rather than in the engine, so every implementation of `Engine`
  // is held to the same cap and a client asking for ten thousand roms gets a
  // page rather than a refusal.
  if (clamped.page_size < 1) {
    clamped.page_size = 1;
  }
  if (clamped.page_size > kMaxPageSize) {
    clamped.page_size = kMaxPageSize;
  }
  // A filter that has no meaning for the kind is dropped rather than passed
  // through: an engine that honoured `platform_id` on the queue would be serving
  // a list nobody asked for, and only one of the two sides would know.
  if (clamped.kind != ListKind::kRoms) {
    clamped.platform_id = 0;
    clamped.search.clear();
  }
  return engine_.ListBegin(clamped, cursor);
}

Error ServiceCore::ListNext(Cursor cursor, ListPage* page) {
  if (cursor == 0) {
    return Error::kBadCursor;
  }
  return engine_.ListNext(cursor, page);
}

Error ServiceCore::ListEnd(Cursor cursor) {
  if (cursor == 0) {
    return Error::kBadCursor;
  }
  return engine_.ListEnd(cursor);
}

// --- the table ----------------------------------------------------------------

/// True for the commands that carry an argument. The rest send `{}`, and are
/// checked once below rather than in eight identical branches.
bool TakesRequest(Command command) {
  switch (command) {
    case Command::kSetConfig:
    case Command::kSetEnabled:
    case Command::kEnqueue:
    case Command::kDequeue:
    case Command::kListBegin:
    case Command::kListNext:
    case Command::kListEnd:
    case Command::kListConflicts:
    case Command::kRestoreBackup:
      return true;
    case Command::kGetInterfaceVersion:
    case Command::kGetStatus:
    case Command::kGetConfig:
    case Command::kSyncNow:
    case Command::kStartPair:
    case Command::kGetPairState:
    case Command::kUnpair:
      return false;
  }
  return false;
}

ConflictPage ServiceCore::ListConflicts(const ConflictQuery& query) {
  // Clamped here rather than in the engine, so every implementation is held to
  // the same cap -- `ListBegin`'s page size, for its reason.
  ConflictQuery clamped = query;
  if (clamped.limit < 1) {
    clamped.limit = 1;
  }
  if (clamped.limit > kMaxConflictPage) {
    clamped.limit = kMaxConflictPage;
  }
  if (clamped.offset < 0) {
    clamped.offset = 0;
  }
  ConflictPage page;
  page.offset = clamped.offset;
  // Never fails: a console that has never overwritten anything has an empty
  // history, and that is the page the screen most needs to draw.
  static_cast<void>(engine_.ListConflicts(clamped, &page));
  return page;
}

conflicts::RestoreReport ServiceCore::RestoreBackup(std::int64_t entry_id) {
  conflicts::RestoreReport report;
  const Error refused = engine_.RestoreBackup(entry_id, &report);
  if (refused != Error::kOk && report.outcome == conflicts::RestoreOutcome::kRestored) {
    // An engine that refused and reported success is a bug on that side; the
    // wire says what the file did, so the safe reading is the refusal.
    report.outcome = conflicts::RestoreOutcome::kWriteFailed;
    report.message = "the sysmodule could not carry out the restore";
  }
  // The two paths in the message are as long as the card lets them be.
  Shorten(&report.message, kMaxRestoreMessageBytes);
  return report;
}

Error Dispatch(ServiceCore& core, std::uint32_t command_id, std::string_view request,
               std::string* response) {
  response->clear();

  Command command = Command::kGetInterfaceVersion;
  if (!IsCommand(command_id, &command)) {
    // The two halves are different releases. `GetInterfaceVersion` is what turns
    // this into a sentence the user can act on.
    return Error::kUnknownCommand;
  }
  // A command that takes nothing is still sent an object, and is still held to
  // it: a caller sending something else has misunderstood which command it is
  // calling, and that is worth saying rather than ignoring.
  if (!TakesRequest(command) && !DecodeEmpty(request).ok()) {
    return Error::kMalformedRequest;
  }

  std::string payload;
  Error error = Error::kOk;

  switch (command) {
    case Command::kGetInterfaceVersion:
      payload = EncodeInterfaceVersion(core.GetInterfaceVersion());
      break;

    case Command::kGetStatus:
      payload = EncodeStatus(core.GetStatus());
      break;

    case Command::kGetConfig:
      payload = EncodeConfigView(core.GetConfig());
      break;

    case Command::kSetConfig: {
      const Decoded<ConfigEdit> edit = DecodeConfigEdit(request);
      if (!edit.ok()) {
        return Error::kMalformedRequest;
      }
      // Succeeds whatever the edit did: what happened is in the answer, because
      // a `Result` that says the call failed takes the answer with it.
      payload = EncodeConfigResult(core.SetConfig(edit.value));
      break;
    }

    case Command::kSetEnabled: {
      const Decoded<bool> wanted = DecodeEnabled(request);
      if (!wanted.ok()) {
        return Error::kMalformedRequest;
      }
      payload = EncodeEnabledResult(core.SetEnabled(wanted.value));
      break;
    }

    case Command::kSyncNow:
      payload = EncodeSyncOutcome(core.SyncNow());
      break;

    case Command::kStartPair: {
      auth::PairingStatus status;
      error = core.StartPair(&status);
      if (error == Error::kOk) {
        payload = auth::SerializePairingStatus(status);
      }
      break;
    }

    case Command::kGetPairState:
      payload = auth::SerializePairingStatus(core.GetPairState());
      break;

    case Command::kUnpair:
      error = core.Unpair();
      if (error == Error::kOk) {
        payload = EncodeEmpty();
      }
      break;

    case Command::kEnqueue: {
      const Decoded<std::int64_t> rom_id = DecodeRomId(request);
      if (!rom_id.ok()) {
        return Error::kMalformedRequest;
      }
      std::int32_t position = 0;
      error = core.Enqueue(rom_id.value, &position);
      if (error == Error::kOk) {
        payload = EncodeQueuePosition(position);
      }
      break;
    }

    case Command::kDequeue: {
      const Decoded<std::int64_t> rom_id = DecodeRomId(request);
      if (!rom_id.ok()) {
        return Error::kMalformedRequest;
      }
      error = core.Dequeue(rom_id.value);
      if (error == Error::kOk) {
        payload = EncodeEmpty();
      }
      break;
    }

    case Command::kListBegin: {
      const Decoded<ListRequest> list = DecodeListRequest(request);
      if (!list.ok()) {
        return Error::kMalformedRequest;
      }
      Cursor cursor = 0;
      error = core.ListBegin(list.value, &cursor);
      if (error == Error::kOk) {
        payload = EncodeCursor(cursor);
      }
      break;
    }

    case Command::kListNext: {
      const Decoded<Cursor> cursor = DecodeCursor(request);
      if (!cursor.ok()) {
        return Error::kMalformedRequest;
      }
      ListPage page;
      error = core.ListNext(cursor.value, &page);
      if (error == Error::kOk) {
        payload = EncodeListPage(page);
      }
      break;
    }

    case Command::kListEnd: {
      const Decoded<Cursor> cursor = DecodeCursor(request);
      if (!cursor.ok()) {
        return Error::kMalformedRequest;
      }
      error = core.ListEnd(cursor.value);
      if (error == Error::kOk) {
        payload = EncodeEmpty();
      }
      break;
    }

    case Command::kListConflicts: {
      const Decoded<ConflictQuery> query = DecodeConflictQuery(request);
      if (!query.ok()) {
        return Error::kMalformedRequest;
      }
      payload = EncodeConflictPage(core.ListConflicts(query.value));
      break;
    }

    case Command::kRestoreBackup: {
      const Decoded<std::int64_t> entry_id = DecodeEntryId(request);
      if (!entry_id.ok()) {
        return Error::kMalformedRequest;
      }
      // Succeeds whatever the restore did, for `SetConfig`'s reason: a `Result`
      // that says the call failed takes the answer with it, and "the backup is
      // gone" is an answer the screen has a sentence for.
      payload = EncodeRestoreReport(core.RestoreBackup(entry_id.value));
      break;
    }
  }

  if (error != Error::kOk) {
    return error;
  }
  // The size check is here rather than in each encoder, so no encoder has to
  // remember it and no response can be one command's oversight away from
  // overrunning a buffer the sysmodule was handed.
  if (!Fits(payload)) {
    return Error::kTooLarge;
  }
  *response = std::move(payload);
  return Error::kOk;
}

}  // namespace rommsync::ipc

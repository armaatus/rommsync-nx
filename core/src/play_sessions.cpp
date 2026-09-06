// Play sessions: where one comes from, where it waits, and the two ways it
// reaches RomM.
//
// See play_sessions.hpp for what this is for. What is worth saying here is what
// the file *is*: a header line carrying the magic, the format version, the next
// id and the moment the last tick looked, then one JSON object per unsent
// session, oldest first, written with `io::WriteAtomically`. It is
// `conflicts.db`'s shape on purpose, which is `state.db`'s -- the reader, the
// bounds and the `.old` recovery are all the same problem.
//
// The wire types and their validation are `sync_complete.cpp`'s, because they
// are fields of the completion call. Nothing here re-checks them; everything
// here goes through `sync::Validate` and `sync::EncodePlaySessions` so a body
// this module builds and a body the sync tick builds cannot differ.
#include "rommsync/play_sessions.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::play {
namespace {

json::Error Fail(std::string_view field, std::string message) {
  json::Error error;
  error.field = std::string(field);
  error.message = std::move(message);
  return error;
}

std::string Describe(const std::string& path, std::string_view what) {
  return path + ": " + std::string(what);
}

/// One diagnostic, unless there are already `kMaxDiagnostics` of them. The
/// count next door is always exact; only the spelling out is bounded, which is
/// `scan::ScanResult`'s rule and `state::kMaxDiagnostics`'s reason.
void AddWarning(std::vector<std::string>* into, std::string line) {
  if (into->size() < kMaxDiagnostics) {
    into->push_back(std::move(line));
  }
}

std::string_view TrimCarriageReturn(std::string_view line) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

/// A whole non-negative decimal, or -1. Used on the two numbers in the header
/// line, which is parsed by hand because it is not JSON.
std::int64_t ReadDecimal(std::string_view text) {
  if (text.empty() || text.size() > 18) {
    return -1;
  }
  std::int64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return -1;
    }
    value = value * 10 + (digit - '0');
  }
  return value;
}

std::string FormatHeader(std::int64_t next_id, sync::Timestamp last_seen) {
  const std::int64_t seen = sync::UnixSeconds(last_seen);
  return std::string(kFormatMagic) + " " + std::to_string(kFormatVersion) + " " +
         std::to_string(next_id) + " " + std::to_string(seen < 0 ? 0 : seen);
}

/// The pairing key a save is matched on between two ticks: `(rom_id, slot)`,
/// exactly the key `sync::ClientSaveState` is arbitrated on and the key
/// `state::Baseline` stores. A derivation keyed on the rom alone would read one
/// rom's two slots as a single save that keeps moving.
using ObservationKey = std::pair<std::int64_t, std::string>;

std::map<ObservationKey, sync::Timestamp> IndexObservations(
    const std::vector<SaveObservation>& observations) {
  std::map<ObservationKey, sync::Timestamp> index;
  for (const SaveObservation& observation : observations) {
    if (observation.rom_id <= 0) {
      continue;
    }
    // First writer wins, which matches the scanner: a duplicate `(rom_id, slot)`
    // is already refused there (`scan::SkipReason::kDuplicateSlot`), so reaching
    // here means a baseline written by an older build.
    index.emplace(ObservationKey{observation.rom_id, observation.slot}, observation.mtime);
  }
  return index;
}

bool Plausible(sync::Timestamp when) {
  const std::int64_t seconds = sync::UnixSeconds(when);
  return seconds >= kEarliestPlausibleSeconds && seconds <= sync::kMaxTimestampSeconds;
}

}  // namespace

// --- deriving -----------------------------------------------------------------

Derivation DeriveSessions(const std::vector<SaveObservation>& previous,
                          const std::vector<SaveObservation>& current,
                          sync::Timestamp last_seen, sync::Timestamp now,
                          const DeriveOptions& options) {
  Derivation derived;

  if (!Plausible(now)) {
    // The console clock has never been set, or has been set to something this
    // client will not carry. Nothing is recorded, and it says so once: the
    // alternative is a library whose play-time totals gained an hour in 1970
    // that nobody can find again.
    AddWarning(&derived.warnings,
               "the console clock reads " + std::to_string(sync::UnixSeconds(now)) +
                   ", which is not a time this client will stamp a play session with; no play "
                   "time was recorded for this tick");
    return derived;
  }
  if (!Plausible(last_seen)) {
    // The ordinary first tick: there is no window yet, so there is nothing to
    // derive and nothing worth a warning. `Buffer::Record` stamps `now` on the
    // way out and the *next* tick has one.
    return derived;
  }
  if (last_seen > now) {
    // The wall clock was corrected backwards between two ticks, which
    // `sched::Scheduler` already handles for the schedule and cannot fix here:
    // every window this tick could derive would run backwards. One tick's play
    // time, and the window closes again on the next.
    AddWarning(&derived.warnings,
               "the console clock has gone backwards since the last tick; no play time was "
               "recorded for this tick");
    return derived;
  }

  const std::map<ObservationKey, sync::Timestamp> before = IndexObservations(previous);

  for (const SaveObservation& observation : current) {
    if (observation.rom_id <= 0) {
      continue;
    }
    const auto found = before.find(ObservationKey{observation.rom_id, observation.slot});
    const bool seen_before = found != before.end();
    if (seen_before && observation.mtime <= found->second) {
      // Nothing was written to this save since the last tick, so nobody played
      // it. The common case, and deliberately not counted: an unchanged card
      // must produce no diagnostics at all.
      continue;
    }

    if (!Plausible(observation.mtime)) {
      derived.skipped++;
      AddWarning(&derived.warnings,
                 "rom " + std::to_string(observation.rom_id) +
                     " has a save whose mtime is not a time a play session can end at; it was "
                     "not recorded");
      continue;
    }
    if (observation.mtime > now) {
      // A save dated after the moment this tick read the clock. Believing it
      // would send a session that ends in the future; it is a card whose clock
      // was wrong when the emulator wrote, and one tick's play time is the
      // whole cost of refusing it.
      derived.skipped++;
      AddWarning(&derived.warnings,
                 "rom " + std::to_string(observation.rom_id) +
                     " has a save whose mtime is in the future; it was not recorded");
      continue;
    }

    // The tightest lower bound this client has: the later of "when the last
    // tick looked" and "when this save was last written". The second only wins
    // when a save was written after the last tick had already read it, which is
    // exactly the case where it is the better bound.
    const sync::Timestamp start =
        seen_before && found->second > last_seen ? found->second : last_seen;
    if (observation.mtime <= start) {
      // The write landed at or before the window opened. A restored file, or a
      // clock corrected between the two reads; either way there is no window to
      // report.
      derived.skipped++;
      continue;
    }

    if (derived.sessions.size() >= options.max_sessions) {
      // Counted for every remaining save, so `skipped` is honest -- but the line
      // is written once. This is one event, not one per save, and sixteen copies
      // of it would crowd out every other diagnostic the tick produced.
      if (derived.skipped == 0) {
        AddWarning(&derived.warnings,
                   "more than " + std::to_string(options.max_sessions) +
                       " saves changed in one tick; the rest were not recorded as play sessions");
      }
      derived.skipped++;
      continue;
    }

    sync::PlaySession session;
    session.rom_id = observation.rom_id;
    if (!observation.slot.empty()) {
      session.save_slot = observation.slot;
    }
    session.start_time = start;
    session.end_time = observation.mtime;
    // **An upper bound, not a measurement**: the whole window counts as play,
    // because nothing on this console can say which part of it was. See the
    // header note.
    session.duration_ms =
        (sync::UnixSeconds(observation.mtime) - sync::UnixSeconds(start)) * 1000;

    if (const json::Error error = sync::Validate(session); !error.ok()) {
      // Unreachable with the checks above, which is what makes it worth having:
      // a session that would be refused on the way out must never reach the
      // buffer, because it would wedge every flush behind a body that cannot be
      // built.
      derived.skipped++;
      AddWarning(&derived.warnings, "rom " + std::to_string(observation.rom_id) +
                                        ": a derived play session was not usable: " +
                                        error.Describe());
      continue;
    }
    derived.sessions.push_back(std::move(session));
  }

  return derived;
}

// --- the file -----------------------------------------------------------------

std::string SerializeRow(const BufferedSession& buffered) {
  const sync::PlaySession& session = buffered.session;
  std::string out("{\"id\":");
  out += std::to_string(buffered.id);
  out += ",\"rom_id\":";
  out += session.rom_id.has_value() ? std::to_string(*session.rom_id) : "null";
  out += ",\"save_slot\":";
  out += session.save_slot.has_value() ? json::Quote(*session.save_slot) : "null";
  out += ",\"start_time\":";
  out += std::to_string(sync::UnixSeconds(session.start_time));
  out += ",\"end_time\":";
  out += std::to_string(sync::UnixSeconds(session.end_time));
  out += ",\"duration_ms\":";
  out += std::to_string(session.duration_ms);
  out += '}';
  return out;
}

bool ParseRow(const json::Value& object, BufferedSession* out, std::string* why) {
  BufferedSession buffered;
  std::int64_t start = 0;
  std::int64_t end = 0;

  json::Reader reader(object, "play session");
  reader.Required("id", &buffered.id);
  reader.RequiredNullable("rom_id", &buffered.session.rom_id);
  reader.RequiredNullable("save_slot", &buffered.session.save_slot);
  // Whole seconds rather than the RFC 3339 the wire carries: this file is the
  // client's own record and a number cannot be spelled two ways, where a
  // timestamp can (`Z` and `+00:00`) and would then round-trip differently
  // depending on which build wrote it.
  reader.Required("start_time", &start);
  reader.Required("end_time", &end);
  reader.Required("duration_ms", &buffered.session.duration_ms);
  if (!reader.ok()) {
    *why = reader.error().Describe();
    return false;
  }
  if (buffered.id <= 0) {
    *why = "field id: is not a positive id";
    return false;
  }
  buffered.session.start_time = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(start));
  buffered.session.end_time = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(end));

  if (buffered.session.save_slot.has_value() &&
      buffered.session.save_slot->size() > kMaxSlotBytes) {
    *why = "field save_slot: is longer than a slot can be";
    return false;
  }
  if (const json::Error error = sync::Validate(buffered.session); !error.ok()) {
    *why = error.Describe();
    return false;
  }
  *out = std::move(buffered);
  return true;
}

std::string SerializeBuffer(const std::vector<BufferedSession>& sessions, std::int64_t next_id,
                            sync::Timestamp last_seen) {
  std::string out = FormatHeader(next_id, last_seen);
  out += "\n";
  for (const BufferedSession& buffered : sessions) {
    out += SerializeRow(buffered);
    out += "\n";
  }
  return out;
}

LoadedBuffer ParseBuffer(std::string_view text) {
  LoadedBuffer loaded;

  std::vector<std::string_view> lines;
  for (std::size_t at = 0; at <= text.size();) {
    const std::size_t end = text.find('\n', at);
    const std::size_t stop = end == std::string_view::npos ? text.size() : end;
    lines.push_back(TrimCarriageReturn(text.substr(at, stop - at)));
    if (end == std::string_view::npos) {
      break;
    }
    at = end + 1;
  }
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  const std::string prefix = std::string(kFormatMagic) + " " + std::to_string(kFormatVersion) + " ";
  if (lines.empty() || lines.front().size() <= prefix.size() ||
      lines.front().substr(0, prefix.size()) != prefix) {
    loaded.diagnostics.push_back("play.db does not begin with \"" + prefix +
                                 "<next id> <last seen>\"; the buffered play time is discarded");
    return loaded;
  }
  const std::string_view tail = lines.front().substr(prefix.size());
  const std::size_t space = tail.find(' ');
  const std::int64_t next_id = ReadDecimal(space == std::string_view::npos ? tail
                                                                          : tail.substr(0, space));
  const std::int64_t seen =
      space == std::string_view::npos ? -1 : ReadDecimal(tail.substr(space + 1));
  if (next_id <= 0 || seen < 0) {
    // Fatal rather than defaulted, which is where this file is stricter than
    // `conflicts.db`: a `last_seen` invented here would silently become the
    // start of every session the next tick derives, and a window that starts at
    // a guess is play time nobody can check.
    loaded.diagnostics.push_back(
        "play.db's header carries no usable next id and last-seen pair; the buffered play time "
        "is discarded");
    return loaded;
  }
  loaded.next_id = next_id;
  loaded.last_seen = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(seen));

  for (std::size_t at = 1; at < lines.size(); ++at) {
    if (loaded.sessions.size() >= kMaxSessions) {
      if (loaded.diagnostics.size() < kMaxDiagnostics) {
        loaded.diagnostics.push_back("play.db holds more than the " +
                                     std::to_string(kMaxSessions) +
                                     " sessions a buffer keeps; the newest are not read");
      }
      break;
    }
    const json::ParseResult document = json::Parse(lines[at]);
    BufferedSession buffered;
    std::string why;
    if (!document.ok()) {
      why = document.error.Describe();
    } else if (!ParseRow(document.value, &buffered, &why)) {
      // `why` is set.
    } else {
      if (buffered.id >= loaded.next_id) {
        // A counter behind a row it already wrote would hand two sessions the
        // same id, and `Reconcile` releases by id.
        loaded.next_id = buffered.id + 1;
      }
      loaded.sessions.push_back(std::move(buffered));
      continue;
    }
    if (loaded.diagnostics.size() < kMaxDiagnostics) {
      loaded.diagnostics.push_back("play.db row " + std::to_string(at) + ": " + why +
                                   "; that row was dropped and the rest were kept");
    }
  }

  return loaded;
}

LoadedBuffer LoadBuffer(const std::string& path) {
  std::string contents;
  io::BoundedRead outcome = io::ReadBounded(path, kMaxBufferBytes, &contents);
  if (outcome == io::BoundedRead::kMissing) {
    outcome = io::ReadBounded(io::PreviousPathFor(path), kMaxBufferBytes, &contents);
    if (outcome == io::BoundedRead::kMissing) {
      LoadedBuffer loaded;
      loaded.diagnostics.push_back(
          Describe(path, "there is no play-session buffer yet; this console has not recorded any "
                         "play time, and the first tick after this one is the first that can"));
      return loaded;
    }
  }
  if (outcome != io::BoundedRead::kOk) {
    LoadedBuffer loaded;
    loaded.diagnostics.push_back(
        Describe(path, std::string(io::ToString(outcome)) +
                           ": the play-session buffer is empty for this boot"));
    return loaded;
  }
  return ParseBuffer(contents);
}

const char* ToString(StoreError error) {
  switch (error) {
    case StoreError::kNone:
      return "none";
    case StoreError::kTooLarge:
      return "too_large";
    case StoreError::kOpenFailed:
      return "open_failed";
    case StoreError::kWriteFailed:
      return "write_failed";
    case StoreError::kCommitFailed:
      return "commit_failed";
  }
  return "none";
}

Buffer::Buffer(std::string path) : path_(std::move(path)) {}

std::vector<std::string> Buffer::Load() {
  LoadedBuffer loaded = LoadBuffer(path_);
  sessions_ = std::move(loaded.sessions);
  next_id_ = loaded.next_id;
  last_seen_ = loaded.last_seen;
  return std::move(loaded.diagnostics);
}

StoreResult Buffer::Persist() {
  StoreResult result;
  const std::string text = SerializeBuffer(sessions_, next_id_, last_seen_);
  if (text.size() > kMaxBufferBytes) {
    // Unreachable with `kMaxSessions` rows of bounded fields -- `play.store`
    // asserts exactly that -- so reaching it means a bound moved. Refuse rather
    // than write a file the reader would discard whole.
    result.error = StoreError::kTooLarge;
    result.message = Describe(path_, "would be " + std::to_string(text.size()) +
                                         " bytes, more than a play-session buffer can be read "
                                         "back with; nothing was written");
    return result;
  }
  const io::WriteResult written = io::WriteAtomically(path_, text);
  switch (written.error) {
    case io::WriteError::kNone:
      break;
    case io::WriteError::kOpenFailed:
      result.error = StoreError::kOpenFailed;
      break;
    case io::WriteError::kWriteFailed:
      result.error = StoreError::kWriteFailed;
      break;
    case io::WriteError::kCommitFailed:
      result.error = StoreError::kCommitFailed;
      break;
  }
  if (result.error != StoreError::kNone) {
    result.message = written.message;
  }
  return result;
}

StoreResult Buffer::Record(const std::vector<sync::PlaySession>& sessions,
                           sync::Timestamp seen_at) {
  StoreResult result;
  std::size_t unusable = 0;
  for (const sync::PlaySession& session : sessions) {
    if (session.save_slot.has_value() && session.save_slot->size() > kMaxSlotBytes) {
      unusable++;
      continue;
    }
    if (!sync::Validate(session).ok()) {
      unusable++;
      continue;
    }
    BufferedSession buffered;
    buffered.id = next_id_++;
    buffered.session = session;
    sessions_.push_back(std::move(buffered));
  }
  // The oldest fall off the front, which is the opposite end from
  // `conflicts::History` and for the opposite reason: a conflict list is read
  // newest-first by a person, and this is a queue the server drains in order.
  if (sessions_.size() > kMaxSessions) {
    result.dropped = sessions_.size() - kMaxSessions;
    sessions_.erase(sessions_.begin(),
                    sessions_.begin() + static_cast<std::ptrdiff_t>(result.dropped));
  }
  // Stamped whether or not anything was recorded: the tick looked, and a window
  // that only moved on productive ticks would over-report the next session.
  last_seen_ = seen_at;

  result.unusable = unusable;

  const StoreResult written = Persist();
  if (!written.ok()) {
    // The sessions stay in memory, `conflicts::History::Append`'s rule: a card
    // that would not write must not also cost the running console the play time
    // it just derived, and the next tick writes the whole file again anyway.
    result.error = written.error;
    result.message = written.message;
  }
  return result;
}

std::vector<BufferedSession> Buffer::Pending(std::size_t limit) const {
  const std::size_t take = std::min(limit, sessions_.size());
  return std::vector<BufferedSession>(sessions_.begin(),
                                      sessions_.begin() + static_cast<std::ptrdiff_t>(take));
}

StoreResult Buffer::Release(const std::vector<std::int64_t>& ids) {
  if (ids.empty()) {
    return {};
  }
  const auto released = [&ids](const BufferedSession& buffered) {
    return std::find(ids.begin(), ids.end(), buffered.id) != ids.end();
  };
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), released), sessions_.end());
  return Persist();
}

// --- reconciling ---------------------------------------------------------------

Reconciliation Reconcile(const std::vector<BufferedSession>& sent,
                         const sync::PlaySessionIngest& ingest) {
  Reconciliation reconciled;
  std::vector<bool> answered(sent.size(), false);

  for (const sync::PlaySessionIngestResult& result : ingest.results) {
    if (result.index < 0 || static_cast<std::size_t>(result.index) >= sent.size()) {
      // A result for an entry that was never sent. Nothing to release, and
      // worth a line: it means this client and RomM disagree about what the
      // request held, which is the state where a silent release would drop a
      // session that never landed.
      AddWarning(&reconciled.warnings,
                 "the server answered for play session " + std::to_string(result.index) +
                     ", which was not in the " + std::to_string(sent.size()) + " sent");
      continue;
    }
    const std::size_t at = static_cast<std::size_t>(result.index);
    if (answered[at]) {
      // Two results for one entry. The first is kept; a second that disagreed
      // would otherwise decide by arriving later.
      AddWarning(&reconciled.warnings, "the server answered twice for play session " +
                                           std::to_string(result.index));
      continue;
    }
    answered[at] = true;

    switch (result.status) {
      case sync::IngestStatus::kCreated:
        reconciled.created++;
        break;
      case sync::IngestStatus::kDuplicate:
        // Success. See `sync::Ingested`.
        reconciled.duplicate++;
        break;
      case sync::IngestStatus::kError:
        reconciled.refused++;
        AddWarning(&reconciled.warnings,
                   "the server refused a play session and it was dropped rather than retried: " +
                       (result.detail.has_value() ? *result.detail : std::string("no reason given")));
        break;
    }
    // Released for all three, including the refusal -- see `Reconciliation::refused`.
    reconciled.release.push_back(sent[at].id);
  }

  for (std::size_t at = 0; at < sent.size(); ++at) {
    if (!answered[at]) {
      reconciled.unanswered++;
    }
  }
  return reconciled;
}

// --- the standalone ingest ------------------------------------------------------

const char* ToString(IngestError error) {
  switch (error) {
    case IngestError::kNone:
      return "none";
    case IngestError::kNotRegistered:
      return "not_registered";
    case IngestError::kNothingToSend:
      return "nothing_to_send";
    case IngestError::kUnauthorized:
      return "unauthorized";
    case IngestError::kForbidden:
      return "forbidden";
    case IngestError::kRejected:
      return "rejected";
    case IngestError::kUnusablePayload:
      return "unusable_payload";
    case IngestError::kCanceled:
      return "canceled";
    case IngestError::kUnreachable:
      return "unreachable";
    case IngestError::kServerError:
      return "server_error";
    case IngestError::kMalformed:
      return "malformed";
  }
  return "none";
}

bool ShouldRetry(IngestError error) {
  return error == IngestError::kUnreachable || error == IngestError::kServerError;
}

auth::Answer AnswerOf(IngestError error) {
  switch (error) {
    case IngestError::kUnauthorized:
      return auth::Answer::kRejected;
    case IngestError::kForbidden:
      return auth::Answer::kForbidden;
    case IngestError::kNone:
      return auth::Answer::kAccepted;
    case IngestError::kNotRegistered:
    case IngestError::kNothingToSend:
    case IngestError::kRejected:
    case IngestError::kUnusablePayload:
    case IngestError::kCanceled:
    case IngestError::kUnreachable:
    case IngestError::kServerError:
    case IngestError::kMalformed:
      break;
  }
  return auth::Answer::kSilent;
}

sync::Encoded EncodeIngestRequest(std::string_view device_id,
                                  const std::vector<sync::PlaySession>& sessions) {
  sync::Encoded encoded;
  const sync::Encoded entries = sync::EncodePlaySessions(sessions);
  if (!entries.ok()) {
    // `EncodePlaySessions` names the field `play_sessions[N].x`, which is the
    // completion body's spelling of it; this body calls the array `sessions`.
    encoded.error = entries.error;
    const std::size_t index = entries.error.field.find('[');
    encoded.error.field =
        index == std::string::npos ? "sessions" : "sessions" + entries.error.field.substr(index);
    return encoded;
  }
  if (device_id.empty()) {
    // Nullable in the schema, and refused here anyway: the acceptance test reads
    // these back with `?device_id=`, and a row filed under the user rather than
    // the console is one this client can never find again.
    encoded.error = Fail("device_id", "is empty");
    return encoded;
  }
  encoded.body = "{\"device_id\":" + json::Quote(device_id) + ",\"sessions\":" + entries.body + "}";
  return encoded;
}

namespace {

IngestOutcome Refuse(IngestError error, std::string message) {
  IngestOutcome refused;
  refused.error = error;
  refused.message = std::move(message);
  return refused;
}

/// Everything that can go wrong between sending and holding a 2xx body,
/// classified once. `sync_complete.cpp`'s `Refused`, over this call's enum.
std::optional<IngestOutcome> Refused(const http::Result& result) {
  if (result.error == http::Error::kCanceled) {
    return Refuse(IngestError::kCanceled,
                  "the play-session flush was stopped: " +
                      (result.message.empty() ? std::string("the caller cancelled")
                                              : result.message));
  }
  if (!result.ok()) {
    return Refuse(IngestError::kUnreachable,
                  std::string("the play sessions were not sent: ") + http::ToString(result.error) +
                      (result.message.empty() ? "" : " (" + result.message + ")"));
  }
  const int status = result.response.status;
  if (status == 401) {
    return Refuse(IngestError::kUnauthorized,
                  "the play-session flush was rejected: HTTP 401; the token has been revoked");
  }
  if (status == 403) {
    return Refuse(IngestError::kForbidden,
                  "the play-session flush was rejected: HTTP 403; this pairing was not granted "
                  "roms.user.write");
  }
  if (status >= 500 || status == 429 || status == 408) {
    return Refuse(IngestError::kServerError,
                  "the play-session flush: HTTP " + std::to_string(status) +
                      (status == 429 ? "; the server is rate limiting" : ""));
  }
  if (!result.successful()) {
    return Refuse(IngestError::kRejected,
                  "the play-session flush was refused: HTTP " + std::to_string(status));
  }
  return std::nullopt;
}

}  // namespace

IngestOutcome IngestSessions(http::HttpClient& client, const auth::StoredToken& token,
                             const std::vector<sync::PlaySession>& sessions,
                             const IngestOptions& options) {
  if (token.server_url.empty() || token.access_token.empty()) {
    return Refuse(IngestError::kNotRegistered, "this console is not paired");
  }
  if (sessions.empty()) {
    // Not a failure and not a request: an empty ingest would be a round trip
    // that says nothing, on a console whose whole budget here is "spend no
    // extra request".
    return Refuse(IngestError::kNothingToSend, "there is no play time to send");
  }
  const sync::Encoded encoded = EncodeIngestRequest(token.device_id, sessions);
  if (!encoded.ok()) {
    return Refuse(IngestError::kUnusablePayload,
                  "the play-session body could not be built: " + encoded.error.Describe());
  }

  http::Request request;
  request.method = http::Method::kPost;
  request.url = http::JoinUrl(token.server_url, "/api/play-sessions");
  request.headers.push_back({"Accept", "application/json"});
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back({"Authorization", "Bearer " + token.access_token});
  request.body = encoded.body;
  request.timeout = options.timeout;
  request.cancel = options.cancel;

  if (options.cancel != nullptr && options.cancel->canceled()) {
    return Refuse(IngestError::kCanceled,
                  "the play sessions were not sent: the caller cancelled first");
  }

  const int attempts = options.max_attempts > 0 ? options.max_attempts : 1;
  std::chrono::milliseconds backoff =
      options.backoff > options.max_backoff ? options.max_backoff : options.backoff;
  std::chrono::milliseconds waited{0};

  for (int attempt = 1;; ++attempt) {
    const http::Result result = client.Send(request);
    IngestOutcome outcome;
    if (const std::optional<IngestOutcome> refused = Refused(result)) {
      outcome = *refused;
    } else {
      const json::ParseResult document = json::Parse(result.response.body);
      if (!document.ok()) {
        outcome = Refuse(IngestError::kMalformed,
                         "the play-session ingest could not be read: " +
                             document.error.Describe());
      } else {
        auth::Parsed<sync::PlaySessionIngest> parsed =
            sync::ParseIngestResponse(document.value, "play session ingest");
        if (parsed.ok()) {
          outcome.value = std::move(parsed.value);
        } else {
          outcome = Refuse(IngestError::kMalformed,
                           "the play-session ingest could not be read: " +
                               parsed.error.Describe());
        }
      }
    }
    outcome.attempts = attempt;
    outcome.waited = waited;

    if (outcome.ok() || !ShouldRetry(outcome.error) || attempt >= attempts) {
      return outcome;
    }
    if (options.cancel != nullptr && options.cancel->canceled()) {
      outcome = Refuse(IngestError::kCanceled,
                       "the play sessions were not re-sent: the caller cancelled");
      outcome.attempts = attempt;
      outcome.waited = waited;
      return outcome;
    }
    if (options.wait != nullptr) {
      options.wait(backoff);
    } else {
      std::this_thread::sleep_for(backoff);
    }
    waited += backoff;
    backoff = backoff * 2 > options.max_backoff ? options.max_backoff : backoff * 2;
  }
}

FlushReport Flush(http::HttpClient& client, const auth::StoredToken& token, Buffer* buffer,
                  const IngestOptions& options) {
  FlushReport report;
  if (buffer == nullptr) {
    report.sent = Refuse(IngestError::kNothingToSend, "there is no buffer to flush");
    return report;
  }

  const std::vector<BufferedSession> pending = buffer->Pending(kMaxSessions);
  report.attempted = pending.size();
  std::vector<sync::PlaySession> sessions;
  sessions.reserve(pending.size());
  for (const BufferedSession& buffered : pending) {
    sessions.push_back(buffered.session);
  }

  report.sent = IngestSessions(client, token, sessions, options);
  if (!report.sent.ok()) {
    // Nothing is released without an answer that names it. The buffer is
    // exactly as it was, and the next flush sends the same sessions -- which
    // RomM answers `duplicate` if this one secretly landed.
    return report;
  }
  report.reconciled = Reconcile(pending, report.sent.value);
  report.released = buffer->Release(report.reconciled.release);
  return report;
}

}  // namespace rommsync::play

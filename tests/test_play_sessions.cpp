// M7-4 (#39): the play time this console can infer, and the two ways it reaches
// RomM.
//
// The feature is optional in the strongest sense -- it writes no save and
// nothing depends on it -- so the thing most of these scenarios pin is what it
// does *not* do: it does not invent a session out of an unset clock, it does not
// grow a file forever, it does not re-send what the server already has, and it
// never fails a sync tick.
//
// Five scenarios need no server and must stay checked with docker stopped:
// `encode` is the wire body both endpoints carry, `derive` is the mtime
// arithmetic with an injected clock (the acceptance criterion that says nothing
// here is verified on hardware -- hard rule 1), `store` is the bounded file that
// survives a reboot, `reconcile` is what an answer releases, and `tick` is the
// promise that a session the encoder refuses costs the play time and not the
// completion.
//
// The rig scenarios are the acceptance list: an ingest that RomM accepts with
// the token the client already has, the same sessions re-sent and answered
// `duplicate`, a completion carrying `play_sessions[]` whose `created_count`
// matches, and a 500 on `/api/play-sessions` that leaves the buffer full and the
// next sync tick working.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include "harness.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/play_sessions.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync_execute.hpp"
#include "rommsync/sync_finish.hpp"

namespace {

namespace auth = rommsync::auth;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace json = rommsync::json;
namespace play = rommsync::play;
namespace sync = rommsync::sync;

using harness::Fixture;
using harness::Sandbox;
using harness::SavePath;

/// A wall clock that does not move, and a plausible one: everything here is
/// after `play::kEarliestPlausibleSeconds`, because the whole module refuses
/// anything before it.
constexpr std::int64_t kNoon = 1'788'000'000;  // 2026-08-29T10:40:00Z

sync::Timestamp At(std::int64_t unix_seconds) {
  return sync::Timestamp{} + std::chrono::seconds{unix_seconds};
}

auth::StoredToken TokenFor(const std::string& base, const Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

play::SaveObservation Seen(std::int64_t rom_id, const std::string& slot, std::int64_t when) {
  play::SaveObservation observation;
  observation.rom_id = rom_id;
  observation.slot = slot;
  observation.mtime = At(when);
  return observation;
}

sync::PlaySession SessionOf(std::int64_t rom_id, const std::string& slot, std::int64_t start,
                            std::int64_t end) {
  sync::PlaySession session;
  session.rom_id = rom_id;
  session.save_slot = slot;
  session.start_time = At(start);
  session.end_time = At(end);
  session.duration_ms = (end - start) * 1000;
  return session;
}

/// A window unique to this run and safely in the past.
///
/// **RomM refuses a session that ends in the future** -- `end_time is too far in
/// the future`, verified against the live 5.2.0 and documented in
/// docs/API_CONTRACT.md#play-sessions. It is not in the OpenAPI snapshot, so a
/// test that picked a fixed instant would go green today and start answering
/// `error` for every entry the moment the fixture outlived it. Anchored to the
/// clock and pushed a month back instead, with the pid spreading concurrent runs
/// apart so a `duplicate` means what the ingest scenario says it means.
std::int64_t RunWindow() {
  const std::int64_t now = sync::UnixSeconds(std::chrono::system_clock::now());
  return now - 30 * 86'400 + static_cast<std::int64_t>(::getpid() % 1'000'000);
}

/// The ingest options every scenario runs with: the backoff spent instantly, so
/// a test that proves there is a retry does not have to wait one out.
play::IngestOptions Instantly() {
  play::IngestOptions options;
  options.wait = [](std::chrono::milliseconds) {};
  return options;
}

// --- encode -------------------------------------------------------------------
//
// The entry both endpoints carry, spelled once (`sync::PlaySession`) because the
// snapshot makes `PlaySessionEntry` and `SyncPlaySessionEntry` field-for-field
// identical. What this pins is the refusals: a session that would reach RomM as
// play time that never happened is refused here rather than sent, and the array
// is refused *whole* because the answer is reconciled by index.

void Encode(rig::Checks& checks) {
  const sync::PlaySession session = SessionOf(4, "retroarch-srm", kNoon, kNoon + 1440);
  const sync::Encoded one = sync::EncodePlaySessions({session});
  checks.Expect(one.ok(), "a derived session encodes: " + one.error.Describe());
  checks.ExpectEq(one.body,
                  std::string(R"([{"rom_id":4,"save_slot":"retroarch-srm",)"
                              R"("start_time":"2026-08-29T10:40:00Z",)"
                              R"("end_time":"2026-08-29T11:04:00Z","duration_ms":1440000}])"),
                  "with the snapshot's field order and the Z spelling the client writes");

  // Both nullable fields are genuinely null on a session nothing could be
  // attributed to, and `null` is not `""` -- only one of them is a value.
  sync::PlaySession anonymous;
  anonymous.start_time = At(kNoon);
  anonymous.end_time = At(kNoon + 60);
  anonymous.duration_ms = 60'000;
  const sync::Encoded bare = sync::EncodePlaySessions({anonymous});
  checks.Expect(bare.ok(), "an unattributed session is still sendable: " + bare.error.Describe());
  checks.Expect(bare.body.find(R"("rom_id":null)") != std::string::npos,
                "with a null rom_id rather than a zero");
  checks.Expect(bare.body.find(R"("save_slot":null)") != std::string::npos,
                "and a null save_slot rather than an empty string");

  checks.ExpectEq(sync::EncodePlaySessions({}).body, std::string("[]"),
                  "a tick that recorded nothing encodes an explicit empty array");

  // Every refusal, and each one is a number a user would otherwise read as play
  // time that happened.
  struct Case {
    const char* what;
    const char* field;
    sync::PlaySession session;
  };
  sync::PlaySession epoch = session;
  epoch.start_time = sync::Timestamp{};
  sync::PlaySession backwards = session;
  backwards.end_time = At(kNoon - 1);
  sync::PlaySession negative = session;
  negative.duration_ms = -1;
  sync::PlaySession impossible = session;
  impossible.duration_ms = (1440 * 1000) + 1;
  sync::PlaySession no_rom = session;
  no_rom.rom_id = 0;
  sync::PlaySession blank_slot = session;
  blank_slot.save_slot = std::string();
  const Case refused[] = {
      {"an unset clock's epoch is not a start time", "start_time", epoch},
      {"an end before its start", "end_time", backwards},
      {"a negative duration", "duration_ms", negative},
      {"a duration longer than its own window", "duration_ms", impossible},
      {"a rom id of zero", "rom_id", no_rom},
      {"a present-and-empty slot", "save_slot", blank_slot},
  };
  for (const Case& one_case : refused) {
    const json::Error error = sync::Validate(one_case.session);
    checks.Expect(!error.ok(), std::string("refused: ") + one_case.what);
    checks.ExpectEq(error.field, std::string(one_case.field),
                    std::string("...and the field is named: ") + one_case.what);
  }

  // The array is refused whole, index-prefixed, because a caller that dropped
  // the bad one would reconcile the answer against a list the server never saw.
  const sync::Encoded mixed = sync::EncodePlaySessions({session, backwards});
  checks.Expect(!mixed.ok(), "one refused entry refuses the whole array");
  checks.ExpectEq(mixed.error.field, std::string("play_sessions[1].end_time"),
                  "...naming which entry and which field");

  // And the two bodies that carry it.
  const sync::CompletionCounts counts{2, 0};
  const sync::Encoded completion = sync::EncodeCompleteRequest(counts, {session});
  checks.Expect(completion.ok(), "the completion carries them: " + completion.error.Describe());
  checks.Expect(completion.body.find(R"("play_sessions":[{"rom_id":4)") != std::string::npos,
                "in play_sessions, beside the counts");
  checks.Expect(!sync::EncodeCompleteRequest(counts, {backwards}).ok(),
                "and refuses the body rather than sending a session it cannot express");

  const sync::Encoded ingest = play::EncodeIngestRequest("device-7", {session});
  checks.Expect(ingest.ok(), "the standalone ingest encodes: " + ingest.error.Describe());
  checks.Expect(ingest.body.find(R"({"device_id":"device-7","sessions":[{)") == 0,
                "with the device this console is, so the rows can be read back by it");
  checks.Expect(!play::EncodeIngestRequest("", {session}).ok(),
                "a body with no device would file the play time under the user, not the console");
  const sync::Encoded bad_entry = play::EncodeIngestRequest("device-7", {backwards});
  checks.Expect(!bad_entry.ok(), "and it refuses an entry the completion would refuse");
  checks.ExpectEq(bad_entry.error.field, std::string("sessions[0].end_time"),
                  "...renamed to this body's own spelling of the array");

  // The answer, which is the same shape from both endpoints.
  const json::ParseResult document = json::Parse(
      R"({"results":[{"index":0,"status":"created","id":9,"detail":null},)"
      R"({"index":1,"status":"duplicate","id":null,"detail":null}],)"
      R"("created_count":1,"skipped_count":1})");
  checks.Expect(document.ok(), "the ingest answer parses");
  const auth::Parsed<sync::PlaySessionIngest> parsed =
      sync::ParseIngestResponse(document.value, "play session ingest");
  checks.Expect(parsed.ok(), "and is read: " + parsed.error.Describe());
  if (parsed.ok()) {
    checks.ExpectEq(static_cast<int>(parsed.value.results.size()), 2, "both results");
    checks.ExpectEq(parsed.value.created_count, static_cast<std::int64_t>(1), "the created count");
    checks.ExpectEq(parsed.value.skipped_count, static_cast<std::int64_t>(1), "the skipped count");
  }
  checks.Expect(sync::Ingested(sync::IngestStatus::kCreated), "created is success");
  checks.Expect(sync::Ingested(sync::IngestStatus::kDuplicate),
                "and so is duplicate -- which is what makes a retried flush safe");
  checks.Expect(!sync::Ingested(sync::IngestStatus::kError), "an error is not");
}

// --- derive -------------------------------------------------------------------
//
// The mtime arithmetic, with an injected clock and no card at all. This is the
// acceptance criterion that says the honest fallback is tested host-side:
// nothing here needs a console, and hard rule 1 says nothing here may want one.

void Derive(rig::Checks& checks) {
  const std::vector<play::SaveObservation> before = {Seen(4, "retroarch-srm", kNoon - 3600),
                                                     Seen(5, "retroarch-srm", kNoon - 3600)};

  // The ordinary case: one save moved, the other did not.
  const std::vector<play::SaveObservation> after = {Seen(4, "retroarch-srm", kNoon - 60),
                                                    Seen(5, "retroarch-srm", kNoon - 3600)};
  const play::Derivation derived =
      play::DeriveSessions(before, after, At(kNoon - 1800), At(kNoon));
  checks.ExpectEq(static_cast<int>(derived.sessions.size()), 1,
                  "a save whose mtime moved is one session, and one that did not is none");
  checks.ExpectEq(derived.skipped, static_cast<std::size_t>(0),
                  "an unchanged save is not a skip, it is nothing at all");
  if (derived.sessions.size() == 1) {
    const sync::PlaySession& session = derived.sessions[0];
    checks.Expect(session.rom_id.has_value() && *session.rom_id == 4, "attributed to the rom");
    checks.Expect(session.save_slot.has_value() && *session.save_slot == "retroarch-srm",
                  "and to the slot the attribution came from");
    checks.ExpectEq(sync::UnixSeconds(session.start_time), kNoon - 1800,
                    "the window opens when the last tick looked");
    checks.ExpectEq(sync::UnixSeconds(session.end_time), kNoon - 60,
                    "and closes at the save's own mtime, which is a real observation");
    checks.ExpectEq(session.duration_ms, static_cast<std::int64_t>(1740 * 1000),
                    "the duration is the whole window -- an upper bound, not a measurement");
    checks.Expect(sync::Validate(session).ok(), "and it is a session the encoder will take");
  }

  // The previous mtime is the tighter bound when it is later than the last tick,
  // which is the case where the emulator wrote after the tick had already read.
  const play::Derivation tighter = play::DeriveSessions(
      {Seen(4, "retroarch-srm", kNoon - 900)}, {Seen(4, "retroarch-srm", kNoon - 60)},
      At(kNoon - 1800), At(kNoon));
  checks.ExpectEq(static_cast<int>(tighter.sessions.size()), 1, "still one session");
  if (tighter.sessions.size() == 1) {
    checks.ExpectEq(sync::UnixSeconds(tighter.sessions[0].start_time), kNoon - 900,
                    "bounded by the save's previous mtime, not by the older tick");
  }

  // A save this client has never seen before. There is no previous mtime, so the
  // window is the tick's, and that is the honest answer rather than a skip.
  const play::Derivation fresh = play::DeriveSessions({}, {Seen(9, "tico-srm", kNoon - 30)},
                                                      At(kNoon - 600), At(kNoon));
  checks.ExpectEq(static_cast<int>(fresh.sessions.size()), 1,
                  "a save that appeared since the last tick is a session too");
  if (fresh.sessions.size() == 1) {
    checks.ExpectEq(sync::UnixSeconds(fresh.sessions[0].start_time), kNoon - 600,
                    "...bounded by the tick, which is all there is to bound it with");
  }

  // The first tick after a boot with no buffer. Silent, and it must be: with no
  // window every save on the card would come back as one enormous session.
  const play::Derivation first = play::DeriveSessions(before, after, sync::Timestamp{}, At(kNoon));
  checks.Expect(first.sessions.empty(), "the first tick with no window derives nothing");
  checks.Expect(first.warnings.empty(), "...and says nothing, because it is not a problem");

  // A console whose clock was never set. Horizon's own unset clock is 2000-01-01,
  // which is why the bound here is not the epoch.
  const play::Derivation unset =
      play::DeriveSessions(before, after, At(kNoon - 1800), At(946'684'800));
  checks.Expect(unset.sessions.empty(), "an unset console clock records no play time");
  checks.Expect(!unset.warnings.empty(), "...and says so once");

  // And one corrected backwards between two ticks.
  const play::Derivation rewound =
      play::DeriveSessions(before, after, At(kNoon + 3600), At(kNoon));
  checks.Expect(rewound.sessions.empty(), "a clock that went backwards records no play time");
  checks.Expect(!rewound.warnings.empty(), "...and says so once");

  // A save dated after the moment the tick read the clock: believing it would
  // send a session that ends in the future.
  const play::Derivation future = play::DeriveSessions(
      before, {Seen(4, "retroarch-srm", kNoon + 3600)}, At(kNoon - 1800), At(kNoon));
  checks.Expect(future.sessions.empty(), "a save with an mtime in the future is not a session");
  checks.ExpectEq(future.skipped, static_cast<std::size_t>(1), "...it is a counted skip");

  // A save whose mtime is before this client will stamp anything.
  const play::Derivation ancient = play::DeriveSessions(
      {Seen(4, "retroarch-srm", 1000)}, {Seen(4, "retroarch-srm", 2000)}, At(kNoon - 1800),
      At(kNoon));
  checks.Expect(ancient.sessions.empty(), "a save with an implausible mtime is not a session");
  checks.ExpectEq(ancient.skipped, static_cast<std::size_t>(1), "...and is counted");

  // A card whose every save moved at once -- a restore, a card swap, a touch.
  // The bound is the buffer's, so a tick cannot push out sessions it already has
  // in favour of ones it cannot explain either.
  std::vector<play::SaveObservation> stampede;
  for (std::int64_t rom = 1; rom <= 60; ++rom) {
    stampede.push_back(Seen(rom, "retroarch-srm", kNoon - 60));
  }
  play::DeriveOptions bounded;
  bounded.max_sessions = 4;
  const play::Derivation capped =
      play::DeriveSessions({}, stampede, At(kNoon - 1800), At(kNoon), bounded);
  checks.ExpectEq(static_cast<int>(capped.sessions.size()), 4, "one tick's output is bounded");
  checks.ExpectEq(capped.skipped, static_cast<std::size_t>(56), "and the rest are counted exactly");
  checks.Expect(capped.warnings.size() <= play::kMaxDiagnostics,
                "the count is exact and only the spelling out is bounded");

  // The archival save: no slot on either side. A derivation that spelled the two
  // differently would report every archival save as played on every tick.
  const play::Derivation archival = play::DeriveSessions(
      {Seen(4, "", kNoon - 3600)}, {Seen(4, "", kNoon - 3600)}, At(kNoon - 1800), At(kNoon));
  checks.Expect(archival.sessions.empty(),
                "an unchanged archival save is matched on the empty slot, not treated as new");
}

// --- store --------------------------------------------------------------------
//
// The bounded file on the card, in the harness sandbox: it round-trips, it drops
// the oldest rather than growing, it survives a reboot, and one unreadable row
// costs one row.

void Store(rig::Checks& checks) {
  Sandbox sandbox(checks, "play-store");
  const std::string path = sandbox.Host(play::kBufferSdPath);

  {
    play::Buffer buffer(path);
    const std::vector<std::string> first = buffer.Load();
    checks.ExpectEq(static_cast<int>(first.size()), 1,
                    "a console with no play.db says so once and carries on");
    checks.Expect(buffer.empty(), "and holds nothing");
    checks.ExpectEq(sync::UnixSeconds(buffer.last_seen()), static_cast<std::int64_t>(0),
                    "with no window, which is what makes the next tick derive nothing");

    // A tick that found nothing still looked, and the stamp has to move: a
    // window that only advanced on productive ticks would over-report the next
    // session by hours.
    const play::StoreResult quiet = buffer.Record({}, At(kNoon - 3600));
    checks.Expect(quiet.ok(), "an empty tick still writes: " + quiet.message);
    checks.ExpectEq(sync::UnixSeconds(buffer.last_seen()), kNoon - 3600,
                    "and moves the window forward");

    const play::StoreResult stored =
        buffer.Record({SessionOf(4, "retroarch-srm", kNoon - 1800, kNoon - 60)}, At(kNoon));
    checks.Expect(stored.ok(), "a session is appended: " + stored.message);
    checks.ExpectEq(static_cast<int>(buffer.size()), 1, "and held");
  }

  // A reboot: a brand-new `Buffer` over the same path.
  {
    play::Buffer buffer(path);
    const std::vector<std::string> reloaded = buffer.Load();
    checks.Expect(reloaded.empty(), "a written buffer reads back clean: " +
                                        (reloaded.empty() ? std::string() : reloaded[0]));
    checks.ExpectEq(static_cast<int>(buffer.size()), 1, "with the session that survived the boot");
    checks.ExpectEq(sync::UnixSeconds(buffer.last_seen()), kNoon,
                    "and the window the tick that wrote it closed at");
    if (buffer.size() == 1) {
      const sync::PlaySession& session = buffer.sessions()[0].session;
      checks.Expect(session.rom_id.has_value() && *session.rom_id == 4, "the rom survived");
      checks.ExpectEq(sync::UnixSeconds(session.start_time), kNoon - 1800, "and the window");
      checks.ExpectEq(session.duration_ms, static_cast<std::int64_t>(1740 * 1000),
                      "and the duration");
      checks.Expect(buffer.sessions()[0].id > 0, "and it has an id a release can address");
    }
  }

  // The bound, and which end falls off. This is a queue the server drains in
  // order, so the *oldest* go -- the opposite end from `conflicts.db`.
  {
    play::Buffer buffer(path);
    static_cast<void>(buffer.Load());
    std::vector<sync::PlaySession> many;
    for (std::size_t at = 0; at < play::kMaxSessions + 10; ++at) {
      const std::int64_t start = kNoon + static_cast<std::int64_t>(at) * 100;
      many.push_back(SessionOf(7, "retroarch-srm", start, start + 60));
    }
    const play::StoreResult full = buffer.Record(many, At(kNoon + 100'000));
    checks.Expect(full.ok(), "an offline console keeps recording: " + full.message);
    checks.ExpectEq(buffer.size(), play::kMaxSessions, "and the file stops growing");
    checks.Expect(full.dropped > 0, "the oldest fell off the front and it says how many");
    if (!buffer.empty()) {
      checks.Expect(sync::UnixSeconds(buffer.sessions().front().session.end_time) >
                        sync::UnixSeconds(buffer.sessions().back().session.end_time) - 100'000,
                    "the newest are the ones that survived");
      checks.Expect(buffer.sessions().front().id < buffer.sessions().back().id,
                    "and they are still in order, oldest first");
    }

    // The bound on the *file* is what the reader enforces, so a full buffer has
    // to fit inside it -- otherwise the reader discards a file the writer was
    // happy with.
    const std::string text =
        play::SerializeBuffer(buffer.sessions(), 10'000, At(kNoon + 100'000));
    checks.Expect(text.size() <= play::kMaxBufferBytes,
                  "a full buffer fits inside the bound its own reader enforces: " +
                      std::to_string(text.size()) + " bytes");
  }

  // Releasing by id, which is what an ingest answer produces.
  {
    play::Buffer buffer(path);
    static_cast<void>(buffer.Load());
    const std::vector<play::BufferedSession> pending = buffer.Pending(3);
    checks.ExpectEq(static_cast<int>(pending.size()), 3, "a flush takes the oldest first");
    const std::size_t before = buffer.size();
    checks.Expect(buffer.Release({pending[0].id, pending[2].id}).ok(), "a release writes");
    checks.ExpectEq(buffer.size(), before - 2, "and drops exactly the ids it was given");
    // Repeating one is what a retry looks like, and it must be harmless.
    checks.Expect(buffer.Release({pending[0].id}).ok(), "an id that is not held is ignored");
    checks.ExpectEq(buffer.size(), before - 2, "...and changes nothing");
  }

  // One unreadable row costs one row. A baseline is discarded whole because its
  // rows are only meaningful together; these are independent, and each one is
  // play time that is nowhere else.
  {
    const std::string good =
        play::SerializeRow({7, SessionOf(4, "retroarch-srm", kNoon, kNoon + 60)});
    const std::string text = std::string(play::kFormatMagic) + " 1 99 " +
                             std::to_string(kNoon) + "\n" + good + "\n{\"id\":8,\n" + good + "\n";
    const play::LoadedBuffer loaded = play::ParseBuffer(text);
    checks.ExpectEq(static_cast<int>(loaded.sessions.size()), 2,
                    "a truncated row is dropped and the rest are kept");
    checks.Expect(!loaded.diagnostics.empty(), "and it is named");
    checks.ExpectEq(loaded.next_id, static_cast<std::int64_t>(99), "the counter comes off the header");

    // A row that would be refused on the way out is refused on the way in: it
    // would otherwise wedge every flush behind a body that cannot be built.
    sync::PlaySession bad = SessionOf(4, "retroarch-srm", kNoon, kNoon + 60);
    bad.duration_ms = 999'999'999;
    const std::string poisoned = std::string(play::kFormatMagic) + " 1 99 " +
                                 std::to_string(kNoon) + "\n" + play::SerializeRow({7, bad}) + "\n";
    checks.Expect(play::ParseBuffer(poisoned).sessions.empty(),
                  "a row the encoder would refuse never reaches the buffer");

    // A header that is not this file's is fatal, and a `last_seen` invented here
    // would silently bound every session the next tick derives.
    checks.Expect(play::ParseBuffer("rommsync-play-sessions 1 99\n").sessions.empty(),
                  "a header with no last-seen is not this file");
    checks.Expect(!play::ParseBuffer("rommsync-play-sessions 1 99\n").diagnostics.empty(),
                  "...and says so");
    checks.Expect(play::ParseBuffer("something else\n" + good + "\n").sessions.empty(),
                  "and neither are bytes that do not begin with the magic");
  }
}

// --- reconcile ----------------------------------------------------------------
//
// What an answer releases. The judgement call this pins is the refusal: an entry
// RomM answered `error` is dropped rather than retried forever, because the body
// was already validated on the way out and a poisoned head would hold a slot a
// good session could have had.

void Reconcile(rig::Checks& checks) {
  std::vector<play::BufferedSession> sent;
  for (std::int64_t at = 0; at < 4; ++at) {
    sent.push_back({10 + at, SessionOf(4, "retroarch-srm", kNoon + at * 100,
                                       kNoon + at * 100 + 60)});
  }

  sync::PlaySessionIngest ingest;
  ingest.created_count = 1;
  ingest.skipped_count = 2;
  ingest.results.push_back({0, sync::IngestStatus::kCreated, 900, std::nullopt});
  ingest.results.push_back({1, sync::IngestStatus::kDuplicate, std::nullopt, std::nullopt});
  ingest.results.push_back({2, sync::IngestStatus::kError, std::nullopt,
                            std::string("end_time before start_time")});
  // The fourth entry is simply not answered.

  const play::Reconciliation reconciled = play::Reconcile(sent, ingest);
  checks.ExpectEq(reconciled.created, static_cast<std::size_t>(1), "one row was created");
  checks.ExpectEq(reconciled.duplicate, static_cast<std::size_t>(1),
                  "one was already there, which is success");
  checks.ExpectEq(reconciled.refused, static_cast<std::size_t>(1), "and one was refused");
  checks.ExpectEq(reconciled.unanswered, static_cast<std::size_t>(1),
                  "the entry nothing said anything about stays buffered");
  checks.ExpectEq(static_cast<int>(reconciled.release.size()), 3,
                  "all three answered entries are released, the refusal included");
  checks.Expect(std::find(reconciled.release.begin(), reconciled.release.end(), 13) ==
                    reconciled.release.end(),
                "and the unanswered one is not");
  checks.Expect(!reconciled.warnings.empty(),
                "a session dropped rather than retried is never silent");

  // An answer that names an entry nothing sent: the two sides disagree about
  // what the request held, which is the state where a silent release would drop
  // a session that never landed.
  sync::PlaySessionIngest stray;
  stray.results.push_back({9, sync::IngestStatus::kCreated, 1, std::nullopt});
  const play::Reconciliation nothing = play::Reconcile(sent, stray);
  checks.Expect(nothing.release.empty(), "an out-of-range index releases nothing");
  checks.Expect(!nothing.warnings.empty(), "and is worth saying out loud");
  checks.ExpectEq(nothing.unanswered, static_cast<std::size_t>(4), "everything stays buffered");

  // Two results for one entry. The first is kept; a second that disagreed would
  // otherwise decide by arriving later.
  sync::PlaySessionIngest twice;
  twice.results.push_back({0, sync::IngestStatus::kCreated, 1, std::nullopt});
  twice.results.push_back({0, sync::IngestStatus::kError, std::nullopt, std::string("no")});
  const play::Reconciliation doubled = play::Reconcile(sent, twice);
  checks.ExpectEq(static_cast<int>(doubled.release.size()), 1, "one entry is released once");
  checks.ExpectEq(doubled.created, static_cast<std::size_t>(1), "and the first answer stands");
  checks.ExpectEq(doubled.refused, static_cast<std::size_t>(0), "the second is ignored");

  // Nothing is released without an answer at all -- which is what makes a flush
  // that never reached the server cost nothing.
  checks.Expect(play::Reconcile(sent, {}).release.empty(),
                "an ingest with no results releases nothing");
}

// --- tick ---------------------------------------------------------------------
//
// The promise the whole feature hangs off: **a play session may never cost a
// sync tick.** `FinishTick` encodes them itself so that a session the encoder
// refuses is dropped and the completion still goes out -- which no server is
// needed to prove, because what is under test is the body that gets built.

void Tick(rig::Checks& checks) {
  const sync::CompletionCounts counts{4, 1};
  sync::PlaySession broken = SessionOf(4, "retroarch-srm", kNoon, kNoon + 60);
  broken.end_time = At(kNoon - 1);

  const sync::Encoded refused = sync::EncodeCompleteRequest(counts, {broken});
  checks.Expect(!refused.ok(), "a completion body carrying a bad session cannot be built");

  // ...which is exactly why `FinishTick` does not hand it to `CompleteSession`.
  // The counts still have to reach the server: RomM is holding a session open
  // over play time nobody asked it to keep.
  const sync::Encoded without = sync::EncodeCompleteRequest(counts, {});
  checks.Expect(without.ok(), "and the same counts with no sessions do: " +
                                  without.error.Describe());
  checks.Expect(without.body.find(R"("operations_completed":4)") != std::string::npos,
                "carrying the tick's real counts");

  const sync::PlaySession fine = SessionOf(4, "retroarch-srm", kNoon, kNoon + 60);
  const sync::Encoded good = sync::EncodeCompleteRequest(counts, {fine, fine});
  checks.Expect(good.ok(), "two good sessions ride along: " + good.error.Describe());

  // And the buffer never releases without an answer. A completion that failed
  // outright carries no ingest, so nothing is dropped and the next tick sends
  // the same sessions -- answered `duplicate` if this one secretly landed.
  const sync::SyncCompletion empty;
  checks.Expect(!empty.play_session_ingest.has_value(),
                "a completion with no ingest says nothing about what was sent");
}

// --- ingest (rig) -------------------------------------------------------------
//
// `POST /api/play-sessions` against the real RomM, with the token the client
// already has: the acceptance criterion that no scope beyond `MinimumScopes()`
// is needed. Then the same sessions again, which must come back `duplicate`.

void Ingest(rig::Checks& checks, http::HttpClient& client, const std::string& base,
            const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "play-ingest");
  const std::string path = sandbox.Host(play::kBufferSdPath);
  const auth::StoredToken token = TokenFor(base, fixture);

  // A window unique to this run, so a re-run does not read another run's rows
  // back and a `duplicate` means what this scenario says it means.
  const std::int64_t base_at = RunWindow();
  const std::vector<sync::PlaySession> sessions = {
      SessionOf(rom.id, "retroarch-srm", base_at, base_at + 600),
      SessionOf(rom.id, "retroarch-srm", base_at + 1200, base_at + 1500)};

  play::Buffer buffer(path);
  static_cast<void>(buffer.Load());
  checks.Expect(buffer.Record(sessions, At(base_at + 1500)).ok(), "the sessions are buffered");

  const play::FlushReport flushed = play::Flush(client, token, &buffer, Instantly());
  checks.Expect(flushed.ok(), "the standalone ingest succeeds with the token the client has: " +
                                 flushed.sent.message);
  if (!flushed.ok()) {
    return;
  }
  checks.ExpectEq(flushed.attempted, static_cast<std::size_t>(2), "both sessions were sent");
  checks.ExpectEq(flushed.sent.value.created_count, static_cast<std::int64_t>(2),
                  "and RomM created a row for each");
  checks.ExpectEq(flushed.reconciled.created, static_cast<std::size_t>(2),
                  "which is what the results say, entry by entry");
  checks.Expect(buffer.empty(), "so the buffer is drained");
  checks.Expect(flushed.released.ok(), "and the drained buffer is on the card: " +
                                           flushed.released.message);

  // Read them back the way the acceptance criterion says: by device.
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet,
      base + "/api/play-sessions?device_id=" + harness::UrlEncode(fixture.device_id) +
          "&limit=50",
      fixture));
  checks.Expect(listed.successful(),
                "the sessions read back by device: HTTP " + std::to_string(listed.response.status));
  const json::ParseResult page = json::Parse(listed.response.body);
  checks.Expect(page.ok() && page.value.is_array(), "the listing is an array of sessions");
  int found = 0;
  if (page.ok() && page.value.is_array()) {
    for (const json::Value& row : page.value.elements()) {
      const json::Value* duration = row.Find("duration_ms");
      const json::Value* device = row.Find("device_id");
      if (duration != nullptr && device != nullptr && device->is_string() &&
          device->string() == fixture.device_id &&
          (duration->integer() == 600'000 || duration->integer() == 300'000)) {
        found++;
      }
    }
  }
  checks.Expect(found >= 2, "and both of this run's sessions are among them");

  // The same sessions again. This is what a retry looks like from the server's
  // side, and reading it as a failure is what would have the client re-send its
  // whole buffer forever.
  const play::IngestOutcome again = play::IngestSessions(client, token, sessions, Instantly());
  checks.Expect(again.ok(), "re-sending them is answered rather than refused: " + again.message);
  if (again.ok()) {
    checks.ExpectEq(again.value.created_count, static_cast<std::int64_t>(0),
                    "nothing was created the second time");
    checks.ExpectEq(again.value.skipped_count, static_cast<std::int64_t>(2),
                    "both were skipped as duplicates");
    for (const sync::PlaySessionIngestResult& result : again.value.results) {
      checks.Expect(result.status == sync::IngestStatus::kDuplicate,
                    std::string("...and each entry says so: ") + sync::ToString(result.status));
      checks.Expect(sync::Ingested(result.status), "which this client treats as success");
    }
  }
}

// --- carried (rig) ------------------------------------------------------------
//
// The route the client actually prefers: `play_sessions[]` on the completion the
// tick is making anyway. The acceptance criterion is that `created_count` matches
// what was sent and that `GET /api/play-sessions` reads them back.

void Carried(rig::Checks& checks, http::HttpClient& client, const std::string& base,
             const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "play-carried");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string slot = harness::UniqueSlot("m7-4-carried");
  const std::string name = "carried.srm";
  sandbox.SeedSave(SavePath(name), "played for a while\n");

  const fs::Listing listing = files->List(harness::kSavesDir);
  checks.Expect(listing.ok(), "the saves folder lists: " + listing.message);
  std::int64_t modified = 0;
  std::int64_t size_bytes = 0;
  for (const fs::Entry& entry : listing.entries) {
    if (!entry.is_directory && entry.name == name) {
      modified = entry.modified_unix;
      size_bytes = entry.size_bytes;
    }
  }
  if (modified == 0) {
    checks.Expect(false, "the seeded save is on the card");
    return;
  }

  sync::ClientSaveState local;
  local.rom_id = rom.id;
  local.file_name = name;
  local.slot = slot;
  local.emulator = "m7-4";
  local.content_hash = crypto::Md5Hex(sandbox.Read(SavePath(name)));
  local.updated_at = At(modified);
  local.file_size_bytes = size_bytes;

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);

  const http::Result negotiated = harness::Negotiate(checks, client, base, fixture, payload);
  if (negotiated.response.status != 200) {
    checks.Expect(false, "the negotiation is answered: HTTP " +
                             std::to_string(negotiated.response.status));
    return;
  }
  const auth::Parsed<sync::SyncPlan> parsed =
      sync::ParseNegotiateResponse(negotiated.response.body);
  checks.Expect(parsed.ok(), "the plan reads: " + parsed.error.Describe());
  if (!parsed.ok()) {
    return;
  }

  // Derived the way a tick derives them: the baseline's mtime against the card's,
  // bounded by the previous tick. Nothing here is invented.
  const std::vector<play::SaveObservation> before = {Seen(rom.id, slot, modified - 3600)};
  const std::vector<play::SaveObservation> after = {Seen(rom.id, slot, modified)};
  const play::Derivation derived =
      play::DeriveSessions(before, after, At(modified - 1800), At(modified + 60));
  checks.ExpectEq(static_cast<int>(derived.sessions.size()), 1,
                  "the save this run wrote derives one session");
  if (derived.sessions.empty()) {
    harness::Complete(client, base, fixture, parsed.value.session_id, 0, 0);
    return;
  }

  sync::FinishOptions finish;
  finish.complete.wait = [](std::chrono::milliseconds) {};
  finish.play_sessions = derived.sessions;

  sync::ExecutionReport report;
  report.completed = 0;
  const sync::TickCompletion tick =
      sync::FinishTick(client, *files, TokenFor(base, fixture), parsed.value, report,
                       payload.saves, /*previous=*/{}, finish);

  checks.Expect(tick.reported.ok(), "the session is completed: " + tick.reported.message);
  checks.ExpectEq(tick.play_sessions_sent, static_cast<std::size_t>(1),
                  "carrying the one play session this tick derived");
  if (!tick.reported.ok()) {
    return;
  }
  const std::optional<sync::PlaySessionIngest>& ingest = tick.reported.value.play_session_ingest;
  checks.Expect(ingest.has_value(),
                "a completion that sent play sessions is answered with a non-null ingest");
  if (!ingest.has_value()) {
    return;
  }
  checks.ExpectEq(ingest->created_count, static_cast<std::int64_t>(1),
                  "and created_count matches what was sent");
  checks.ExpectEq(static_cast<int>(ingest->results.size()), 1, "with one result");
  if (!ingest->results.empty()) {
    checks.Expect(sync::Ingested(ingest->results[0].status),
                  std::string("which is a success: ") + sync::ToString(ingest->results[0].status));
  }

  // Read back by device, which is the acceptance criterion's own check.
  const http::Result listed = client.Send(harness::Authed(
      http::Method::kGet,
      base + "/api/play-sessions?device_id=" + harness::UrlEncode(fixture.device_id) +
          "&limit=50",
      fixture));
  checks.Expect(listed.successful(), "the sessions read back: HTTP " +
                                         std::to_string(listed.response.status));
  const json::ParseResult page = json::Parse(listed.response.body);
  bool matched = false;
  if (page.ok() && page.value.is_array()) {
    for (const json::Value& row : page.value.elements()) {
      const json::Value* sync_session = row.Find("sync_session_id");
      if (sync_session != nullptr && sync_session->is_integer() &&
          sync_session->integer() == parsed.value.session_id) {
        matched = true;
      }
    }
  }
  checks.Expect(matched, "and the row RomM wrote is attached to the sync session that carried it");
}

// --- buffered (rig) -----------------------------------------------------------
//
// The failure path, which is the only one that matters for an optional feature:
// a 500 on `/api/play-sessions` must leave the sessions buffered, and the next
// sync tick must complete normally.

void Buffered(rig::Checks& checks, http::HttpClient& client, const std::string& base,
              const Fixture& fixture, const harness::Rom& rom) {
  Sandbox sandbox(checks, "play-buffered");
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(sandbox.root().string());
  const std::string path = sandbox.Host(play::kBufferSdPath);
  const auth::StoredToken token = TokenFor(base, fixture);

  const std::int64_t base_at = RunWindow();
  play::Buffer buffer(path);
  static_cast<void>(buffer.Load());
  checks.Expect(
      buffer.Record({SessionOf(rom.id, "retroarch-srm", base_at, base_at + 300)}, At(base_at + 300))
          .ok(),
      "a session is buffered");

  {
    // Every attempt, not just the first: `Flush` retries a 5xx, and a scenario
    // that armed one fault would have the retry succeed and prove nothing.
    harness::Fault fault(
        checks, client, base,
        R"({"mode":"status","status":500,"path":"/api/play-sessions","count":10})");
    const play::FlushReport flushed = play::Flush(client, token, &buffer, Instantly());
    checks.Expect(!flushed.ok(), "a 500 fails the flush");
    checks.Expect(flushed.sent.error == play::IngestError::kServerError,
                  std::string("as a server error: ") + play::ToString(flushed.sent.error));
    checks.Expect(play::ShouldRetry(flushed.sent.error), "which is retryable");
    checks.Expect(flushed.sent.attempts > 1, "so it was retried");
    checks.ExpectEq(static_cast<int>(buffer.size()), 1,
                    "and the session is still buffered -- nothing is released without an answer");
  }

  // It survives the failure on the card too, not just in memory.
  {
    play::Buffer reread(path);
    static_cast<void>(reread.Load());
    checks.ExpectEq(static_cast<int>(reread.size()), 1,
                    "a flush that failed left the file exactly as it was");
  }

  // And the sync tick, whose completion carries the same session, still works --
  // the fault was on the other endpoint and this one is untouched.
  const std::string slot = harness::UniqueSlot("m7-4-buffered");
  const std::string name = "buffered.srm";
  sandbox.SeedSave(SavePath(name), "still playable\n");
  sync::ClientSaveState local;
  local.rom_id = rom.id;
  local.file_name = name;
  local.slot = slot;
  local.emulator = "m7-4";
  local.content_hash = crypto::Md5Hex(sandbox.Read(SavePath(name)));
  local.updated_at = At(base_at + 300);
  local.file_size_bytes = static_cast<std::int64_t>(sandbox.SizeOf(SavePath(name)));

  sync::SyncNegotiatePayload payload;
  payload.device_id = fixture.device_id;
  payload.saves.push_back(local);
  const http::Result negotiated = harness::Negotiate(checks, client, base, fixture, payload);
  if (negotiated.response.status != 200) {
    checks.Expect(false, "the negotiation is answered: HTTP " +
                             std::to_string(negotiated.response.status));
    return;
  }
  const auth::Parsed<sync::SyncPlan> parsed =
      sync::ParseNegotiateResponse(negotiated.response.body);
  checks.Expect(parsed.ok(), "the plan reads: " + parsed.error.Describe());
  if (!parsed.ok()) {
    return;
  }

  const std::vector<play::BufferedSession> pending = buffer.Pending(play::kMaxSessions);
  sync::FinishOptions finish;
  finish.complete.wait = [](std::chrono::milliseconds) {};
  for (const play::BufferedSession& buffered : pending) {
    finish.play_sessions.push_back(buffered.session);
  }
  sync::ExecutionReport report;
  const sync::TickCompletion tick =
      sync::FinishTick(client, *files, TokenFor(base, fixture), parsed.value, report,
                       payload.saves, /*previous=*/{}, finish);
  checks.Expect(tick.reported.ok(),
                "the next sync tick completes normally: " + tick.reported.message);
  checks.Expect(tick.stored.ok(), "and its baseline is written: " + tick.stored.message);
  if (tick.reported.ok() && tick.reported.value.play_session_ingest.has_value()) {
    const play::Reconciliation reconciled =
        play::Reconcile(pending, *tick.reported.value.play_session_ingest);
    checks.Expect(buffer.Release(reconciled.release).ok(), "the released ids are written");
    checks.Expect(buffer.empty(),
                  "and the sessions the ingest failed to take reached RomM on the tick instead");
  }
}

}  // namespace

int main(int argc, char** argv) {
  rommsync::host::InstallPosixFileSync();

  const std::string scenario = argc > 1 ? argv[1] : "encode";
  const std::string base = rig::BaseUrl();

  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  // The tally lives above every scenario so a `Sandbox`'s teardown audit reports
  // into an object that outlives it -- see `harness::Sandbox`.
  rig::Checks checks;

  if (scenario == "encode" || scenario == "derive" || scenario == "store" ||
      scenario == "reconcile" || scenario == "tick") {
    if (scenario == "encode") {
      Encode(checks);
    } else if (scenario == "derive") {
      Derive(checks);
    } else if (scenario == "store") {
      Store(checks);
    } else if (scenario == "reconcile") {
      Reconcile(checks);
    } else {
      Tick(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "play." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  rig::DisarmFault(*client, base);

  Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }
  // A session an earlier scenario left open is one this scenario's negotiate has
  // to cancel, and that cancel races with the session it just created (#76).
  harness::CloseOpenSessions(*client, base, fixture);

  harness::Rom rom;
  if (!harness::FindRom(*client, base, fixture, "gb240p.gb", &rom)) {
    std::cerr << "the fixture library holds no roms\n"
                 "  scan it with: ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }

  if (scenario == "ingest") {
    Ingest(checks, *client, base, fixture, rom);
  } else if (scenario == "carried") {
    Carried(checks, *client, base, fixture, rom);
  } else if (scenario == "buffered") {
    Buffered(checks, *client, base, fixture, rom);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  rig::DisarmFault(*client, base);
  harness::ExpectDisarmed(checks, *client, base, "the scenario left the proxy disarmed");

  if (checks.failures() == 0) {
    std::cout << "play." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

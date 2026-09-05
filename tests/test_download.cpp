// M3-2: the download queue and the worker that drains it.
//
// It splits the way the risk does. Six scenarios need no server, because what
// they pin is what happens to a file a yanked SD card left behind -- a queue
// that is truncated, oversized, half-written, or written by another release --
// and a healthy RomM has nothing to do with any of it. Those must stay checked
// with docker stopped.
//
// The other five need the real RomM 5.2.0 and the fault proxy in front of it,
// because what they pin is a *transfer*: bytes that match the fixture on the
// card, a 500 that leaves the entry queued rather than dropped, a connection
// dropped 4 MiB in that resumes rather than restarts, and a disc set refused
// with a sentence. None of those is something a literal in this file can be
// right about.
//
//   roundtrip  -- every field survives queue.json, states and a null hash included
//   queue      -- Enqueue/Remove/Clear/Snapshot, and the fixed IPC error set
//   corrupt    -- truncated, garbage, another release's format: an empty queue and a reason
//   store      -- the write is atomic, leaves no .tmp/.old, and a failed one costs nothing
//   bounds     -- a full queue of worst-case entries still fits the byte bound
//   endpoint   -- the content URL's encoding, and the rom detail shape
//   drain      -- the seeded 240pee.nes, byte-for-byte at the mapped destination
//   retries    -- a 500 leaves the entry queued with attempts up and backoff spent
//   resume     -- a drop 4 MiB in leaves a resumable entry; a second drain finishes it
//   verify     -- a wrong sha1_hash fails the entry and leaves nothing behind
//   fallback   -- md5_hash when there is no sha1; unverified when there is neither
//   truncate   -- a body that ends early, caught by the caller's own expected size
//   stall      -- the client's own stall_timeout ends it, and the entry is retried
//   staging    -- what an interrupted attempt leaves behind: salvaged, or discarded
//   disabled   -- `[downloads] enabled = false` drains nothing and loses nothing
//   multifile  -- a disc set is refused with a reason and no archive reaches the card
//   nested     -- ...and the directory-shaped rom beside it is downloaded, not refused
//   missing    -- a rom the server's library no longer holds fails with a sentence
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/config.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/sha1.hpp"

namespace config = rommsync::config;
namespace download = rommsync::download;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace ipc = rommsync::ipc;
namespace json = rommsync::json;
namespace crypto = rommsync::crypto;
namespace roms = rommsync::roms;

namespace {

using download::QueueEntry;
using download::QueueState;

/// An entry with every field set to something distinguishable, so a round trip
/// that dropped one is a failed comparison rather than a value that happens to
/// match its default.
QueueEntry Populated() {
  QueueEntry entry;
  entry.rom_id = 4;
  entry.platform_fs_slug = "nes";
  entry.fs_name = "Where in Time (USA) [!].nes";
  entry.size_bytes = 65552;
  entry.sha1_hash = "ff66e33efc818b516f7994f3027a72f4bc629b30";
  entry.destination = "/tico/roms/nes/Where in Time (USA) [!].nes";
  entry.state = QueueState::kActive;
  entry.bytes_done = 4096;
  entry.attempts = 2;
  entry.message = "the transfer did not complete";
  entry.queued_at = 1'757'000'000;
  return entry;
}

bool Same(const QueueEntry& left, const QueueEntry& right) {
  return left.rom_id == right.rom_id && left.platform_fs_slug == right.platform_fs_slug &&
         left.fs_name == right.fs_name && left.size_bytes == right.size_bytes &&
         left.sha1_hash == right.sha1_hash && left.destination == right.destination &&
         left.state == right.state && left.bytes_done == right.bytes_done &&
         left.attempts == right.attempts && left.message == right.message &&
         left.queued_at == right.queued_at;
}

/// A config with a usable server and the built-in folder map, so no scenario is
/// also a test of the missing-server error.
config::Config Settings() {
  return config::ParseConfig("[server]\nurl = https://romm.example.com\n").value;
}

// --- the format ---------------------------------------------------------------

void Roundtrip(checks::Checks& c) {
  const QueueEntry entry = Populated();
  const download::LoadedQueue back = download::ParseQueue(download::SerializeQueue({entry}));
  c.Expect(back.diagnostics.empty(), "a queue this module wrote reads back with no complaint");
  c.ExpectEq(back.entries.size(), std::size_t{1}, "and holds the entry it was given");
  if (back.entries.size() == 1) {
    c.Expect(Same(back.entries.front(), entry), "with every field as it went in");
  }

  // Every state, because the state string is a *format* decision: a release that
  // spelled one differently would read the file and re-download a rom the last
  // one finished.
  for (const QueueState state :
       {QueueState::kQueued, QueueState::kActive, QueueState::kVerifying, QueueState::kDone,
        QueueState::kFailed, QueueState::kSkipped}) {
    QueueEntry one = Populated();
    one.state = state;
    const download::LoadedQueue parsed = download::ParseQueue(download::SerializeQueue({one}));
    c.ExpectEq(parsed.entries.size(), std::size_t{1},
               std::string("the state '") + download::ToString(state) + "' round-trips");
    if (parsed.entries.size() == 1) {
      c.Expect(parsed.entries.front().state == state,
               std::string("...as '") + download::ToString(state) + "'");
    }
    QueueState read{};
    c.Expect(download::ParseQueueState(download::ToString(state), &read) && read == state,
             std::string("ToString and ParseQueueState agree about ") + download::ToString(state));
  }
  QueueState unknown{};
  c.Expect(!download::ParseQueueState("paused", &unknown),
           "a state this build does not know is refused, not read as queued");

  // The library leaves `sha1_hash` null on an unscanned rom, so "there is no
  // hash" has to survive as an answer rather than becoming an empty string a
  // reader cannot tell from a missing field.
  QueueEntry unhashed = Populated();
  unhashed.sha1_hash.clear();
  const std::string text = download::SerializeQueue({unhashed});
  c.Expect(text.find("\"sha1_hash\":null") != std::string::npos,
           "a rom with no hash is written as null rather than as an empty string");
  const download::LoadedQueue reread = download::ParseQueue(text);
  c.ExpectEq(reread.entries.size(), std::size_t{1}, "and reads back");
  if (reread.entries.size() == 1) {
    c.Expect(reread.entries.front().sha1_hash.empty(), "...still with no hash");
  }

  // A name holding a quote and a backslash is a name RomM will happily serve.
  // The header says the body is built with `json::Quote` and never by
  // concatenation; this is what that buys.
  QueueEntry hostile = Populated();
  hostile.fs_name = "a\"b\\c\td.nes";
  hostile.message = "line\nbreak";
  const download::LoadedQueue quoted = download::ParseQueue(download::SerializeQueue({hostile}));
  c.ExpectEq(quoted.entries.size(), std::size_t{1}, "a name holding a quote survives the file");
  if (quoted.entries.size() == 1) {
    c.ExpectEq(quoted.entries.front().fs_name, hostile.fs_name, "...carried, not interpreted");
    c.ExpectEq(quoted.entries.front().message, hostile.message, "...and so does a message");
  }

  // The empty file is a real state: a queue that was drained is written as one.
  const download::LoadedQueue empty = download::ParseQueue(download::SerializeQueue({}));
  c.Expect(empty.diagnostics.empty(), "an empty queue is a queue, not a complaint");
  c.Expect(empty.entries.empty(), "and it holds nothing");
}

// --- the queue ----------------------------------------------------------------

void QueueApi(checks::Checks& c) {
  download::Queue queue;
  std::int32_t position = 0;

  c.Expect(queue.Enqueue(4, &position) == ipc::Error::kOk, "a rom is enqueued");
  c.ExpectEq(position, 1, "and lands first, 1-based");
  c.Expect(queue.Enqueue(5, &position) == ipc::Error::kOk, "a second rom too");
  c.ExpectEq(position, 2, "...behind the first");
  c.ExpectEq(queue.pending(), std::size_t{2}, "both are waiting for the worker");

  c.Expect(queue.Enqueue(4, &position) == ipc::Error::kDuplicate,
           "enqueueing the same rom twice is one entry");
  c.ExpectEq(queue.size(), std::size_t{2}, "and does not deepen the queue");

  c.Expect(queue.Enqueue(0, &position) == ipc::Error::kUnknownRom,
           "a rom id of zero names no rom");
  c.Expect(queue.Enqueue(-1, &position) == ipc::Error::kUnknownRom, "nor does a negative one");

  // A terminal entry is re-queued rather than refused: a user pressing download
  // on a rom that failed means "try again", and `kDuplicate` would leave them no
  // way to ask for one.
  QueueEntry failed = queue.Find(5);
  failed.state = QueueState::kFailed;
  failed.attempts = 7;
  failed.message = "the server answered 500";
  c.Expect(queue.Update(failed), "an entry can be written back");
  c.ExpectEq(queue.pending(), std::size_t{1}, "a terminal entry is not pending");
  c.ExpectEq(queue.size(), std::size_t{2}, "...but it is still there, for the queue screen");
  c.Expect(queue.Enqueue(5, &position) == ipc::Error::kOk, "and it can be queued again");
  c.ExpectEq(position, 2, "behind what was already waiting");
  c.ExpectEq(queue.size(), std::size_t{2}, "still one entry for that rom");
  c.ExpectEq(queue.Find(5).attempts, 0, "with its attempts reset");
  c.Expect(queue.Find(5).message.empty(), "and last time's reason gone");

  c.Expect(queue.Remove(4) == ipc::Error::kOk, "an entry is removed");
  c.Expect(queue.Remove(4) == ipc::Error::kNotQueued, "and removing it again says so");
  c.ExpectEq(queue.size(), std::size_t{1}, "leaving the other one");
  c.ExpectEq(queue.Snapshot().size(), std::size_t{1}, "which is what a snapshot shows");
  c.ExpectEq(queue.Find(4).rom_id, std::int64_t{0}, "and looking for the removed one finds none");

  // The cap is a refusal rather than a silent drop: an entry that vanished is a
  // download that never happens with nothing to say why.
  download::Queue full;
  for (std::size_t at = 0; at < download::kMaxQueueEntries; ++at) {
    full.Enqueue(static_cast<std::int64_t>(at) + 1, &position);
  }
  c.ExpectEq(full.size(), download::kMaxQueueEntries, "the queue takes its full depth");
  c.Expect(full.Enqueue(9'999, &position) == ipc::Error::kQueueFull, "and refuses one more");
  c.ExpectEq(full.size(), download::kMaxQueueEntries, "without dropping anything to make room");

  // ...but only while every row is work still to do. Finished rows stay for the
  // queue screen and nothing prunes them, so counting them against the cap
  // would mean a console that had downloaded `kMaxQueueEntries` roms over its
  // life could never queue another -- with an empty queue on the screen and
  // nothing saying why.
  QueueEntry finished = full.Find(1);
  finished.state = QueueState::kDone;
  full.Update(finished);
  c.Expect(full.Enqueue(9'999, &position) == ipc::Error::kOk,
           "a queue whose oldest row is finished has room for one more");
  c.ExpectEq(full.size(), download::kMaxQueueEntries, "the file stays at its cap");
  c.ExpectEq(full.Find(1).rom_id, std::int64_t{0}, "and it is the finished row that made way");
  c.ExpectEq(full.Find(2).rom_id, std::int64_t{2}, "not one that is still waiting");
  full.Clear();
  c.ExpectEq(full.size(), std::size_t{0}, "Clear empties it");

  // `Reset` is what a load does. Entries past the cap are dropped from the tail
  // rather than making a queue the writer would then refuse to write at all.
  std::vector<QueueEntry> too_many;
  for (std::size_t at = 0; at < download::kMaxQueueEntries + 5; ++at) {
    QueueEntry entry;
    entry.rom_id = static_cast<std::int64_t>(at) + 1;
    too_many.push_back(entry);
  }
  full.Reset(std::move(too_many));
  c.ExpectEq(full.size(), download::kMaxQueueEntries, "Reset clamps to the cap");

  // What the status screen draws. The byte-level half is #22's; this is the
  // projection `ipc::Status` already carries.
  download::Queue watched;
  c.Expect(watched.CurrentDownload().state == ipc::DownloadState::kIdle,
           "an empty queue reports an idle worker");
  watched.Enqueue(4, &position);
  c.Expect(watched.CurrentDownload().state == ipc::DownloadState::kQueued,
           "a queue with something waiting is not idle -- it is about to start");
  QueueEntry moving = watched.Find(4);
  moving.state = QueueState::kActive;
  moving.fs_name = "240pee.nes";
  moving.bytes_done = 4096;
  moving.size_bytes = 65552;
  watched.Update(moving);
  const ipc::DownloadSnapshot snapshot = watched.CurrentDownload();
  c.Expect(snapshot.state == ipc::DownloadState::kDownloading, "an active entry is downloading");
  c.ExpectEq(snapshot.rom_id, std::int64_t{4}, "and it says which rom");
  c.ExpectEq(snapshot.fs_name, std::string("240pee.nes"), "by name and never by path");
  c.ExpectEq(snapshot.bytes_done, std::int64_t{4096}, "with the bytes already on the card");
  c.ExpectEq(snapshot.bytes_total, std::int64_t{65552}, "and the total to expect");

  // `Update` on an entry that is gone is not an error: `Remove` is callable
  // while the worker is mid-transfer, and a transfer finishing must not put the
  // entry the user removed back into the queue.
  c.Expect(watched.Remove(4) == ipc::Error::kOk, "the entry is removed under the worker");
  c.Expect(!watched.Update(moving), "and writing it back says there is no such entry");
  c.ExpectEq(watched.size(), std::size_t{0}, "which is what keeps it removed");

  // The two refusals only the library can answer. Neither may touch the network
  // (ipc.hpp), so both are read off the index the engine already holds.
  roms::RomIndex library;
  library.Add({4, "240pee", "240pee", "nes", false});
  library.Add({6, "Synthetic Two Disc Game", "Synthetic Two Disc Game", "psx", true});
  download::Queue engine_queue;
  c.Expect(
      download::EnqueueRom(engine_queue, library, 999'999, &position) == ipc::Error::kUnknownRom,
      "a rom the library does not have is unknown, not queued");
  c.Expect(download::EnqueueRom(engine_queue, library, 6, &position) == ipc::Error::kMultiFile,
           "a disc set is refused at the door rather than queued and then skipped");
  c.ExpectEq(engine_queue.size(), std::size_t{0}, "neither left an entry behind");
  c.Expect(download::EnqueueRom(engine_queue, library, 4, &position) == ipc::Error::kOk,
           "and a single-file rom the library knows is queued");
  c.ExpectEq(position, 1, "at the head");
  c.Expect(download::EnqueueRom(engine_queue, library, 4, &position) == ipc::Error::kDuplicate,
           "with the queue's own refusals still in force behind the library's");
}

// --- a file the card left behind ----------------------------------------------

void Corrupt(checks::Checks& c) {
  const std::string good = download::SerializeQueue({Populated()});

  const auto discarded = [&c](std::string_view text, std::string_view what) {
    const download::LoadedQueue loaded = download::ParseQueue(text);
    c.Expect(loaded.entries.empty(), std::string(what) + " yields an empty queue");
    c.Expect(!loaded.diagnostics.empty(), std::string(what) + " says so in a diagnostic");
    c.Expect(!loaded.DescribeDiagnostics().empty(), std::string(what) + " renders for a log");
  };

  discarded("", "an empty file");
  discarded("\x01\x02\x03 not json at all", "a file of garbage");
  discarded(std::string_view(good).substr(0, good.size() / 2), "a file cut in half");
  discarded(std::string_view(good).substr(0, good.size() - 3), "a file cut three bytes short");
  discarded("[]", "a bare array where the envelope should be");
  discarded("{\"format\":\"rommsync-queue\",\"version\":2,\"entries\":[]}",
            "a file from another release");
  discarded("{\"format\":\"rommsync-state\",\"version\":1,\"entries\":[]}",
            "a file that is not a queue at all");
  discarded("{\"format\":\"rommsync-queue\",\"version\":1,\"entries\":{}}",
            "an entries field that is not an array");
  discarded("{\"format\":\"rommsync-queue\",\"version\":1}", "a file with no entries at all");
  discarded("{\"format\":\"rommsync-queue\",\"version\":1,\"entries\":[42]}",
            "an entry that is not an object");

  // One unreadable entry costs the whole file, not just itself: a truncation
  // leaves a prefix that is individually well-formed and collectively a lie.
  QueueEntry second = Populated();
  second.rom_id = 5;
  std::string pair = download::SerializeQueue({Populated(), second});
  const std::string active = "\"state\":\"active\"";
  const std::size_t state_at = pair.rfind(active);
  c.Expect(state_at != std::string::npos, "the second entry's state is findable");
  if (state_at != std::string::npos) {
    pair.replace(state_at, active.size(), "\"state\":\"paused\"");
    const download::LoadedQueue loaded = download::ParseQueue(pair);
    c.Expect(loaded.entries.empty(),
             "one entry this build cannot read discards the whole queue, not just that entry");
    c.Expect(!loaded.diagnostics.empty(), "and names what was wrong with it");
  }

  // Two entries for one rom is a file no `Enqueue` can produce, so it is a
  // corruption rather than a duplicate to merge.
  const download::LoadedQueue twice =
      download::ParseQueue(download::SerializeQueue({Populated(), Populated()}));
  c.Expect(twice.entries.empty(), "two entries for one rom is a file that is not intact");

  // More entries than a queue may hold, well inside the byte bound.
  std::vector<QueueEntry> crowd;
  for (std::size_t at = 0; at < download::kMaxQueueEntries + 1; ++at) {
    QueueEntry entry;
    entry.rom_id = static_cast<std::int64_t>(at) + 1;
    crowd.push_back(entry);
  }
  discarded(download::SerializeQueue(crowd), "a file holding more entries than a queue may");

  // The diagnostics are bounded: a corrupt card region must not answer with
  // thousands of strings on a sysmodule heap.
  std::string many("{\"format\":\"rommsync-queue\",\"version\":1,\"entries\":[");
  for (std::size_t at = 0; at < download::kMaxQueueEntries; ++at) {
    many += at == 0 ? "" : ",";
    many += "{\"rom_id\":\"nope\"}";
  }
  many += "]}";
  const download::LoadedQueue noisy = download::ParseQueue(many);
  c.Expect(noisy.entries.empty(), "a file of nothing but bad entries is discarded");
  c.Expect(noisy.diagnostics.size() <= download::kMaxDiagnostics + 1,
           "and complains a bounded number of times");
}

// --- the write ----------------------------------------------------------------

void Store(checks::Checks& c) {
  harness::Sandbox sandbox(c, "download-store");
  const std::string path = sandbox.Host("/config/rommsync/queue.json");

  download::Queue queue;
  std::int32_t position = 0;
  queue.Enqueue(4, &position);
  queue.Enqueue(5, &position);

  c.Expect(download::SaveQueue(path, queue).ok(), "the queue is written");
  c.Expect(sandbox.Exists("/config/rommsync/queue.json"), "and the file is there");
  c.Expect(!sandbox.Exists("/config/rommsync/queue.json.tmp"), "no .tmp is left beside it");
  c.Expect(!sandbox.Exists("/config/rommsync/queue.json.old"), "and no .old either");
  c.Expect(download::LoadQueue(path).diagnostics.empty(), "and it reads back clean");
  c.ExpectEq(download::LoadQueue(path).entries.size(), std::size_t{2}, "with both entries");

  // A second write over the first is the case Horizon's rename cannot do
  // directly (atomic_file.hpp), so it is the one that has to leave nothing
  // behind either.
  queue.Enqueue(6, &position);
  c.Expect(download::SaveQueue(path, queue).ok(), "the queue is written over itself");
  c.Expect(!sandbox.Exists("/config/rommsync/queue.json.tmp"), "still no .tmp");
  c.Expect(!sandbox.Exists("/config/rommsync/queue.json.old"), "still no .old");
  c.ExpectEq(download::LoadQueue(path).entries.size(), std::size_t{3}, "and it holds three");

  // A missing file is not a failure -- it is a console that has queued nothing.
  const download::LoadedQueue absent = download::LoadQueue(sandbox.Host("/config/rommsync/none"));
  c.Expect(absent.entries.empty(), "a queue that was never written is empty");
  c.Expect(!absent.diagnostics.empty(), "and says so once");

  // The one moment `queue.json` legitimately does not exist is the window
  // between `WriteAtomically`'s two renames, with the previous queue sitting
  // under `.old`. Reconstruct exactly that and check the recovery.
  const std::string previous = io::PreviousPathFor(path);
  const std::string kept = io::ReadFile(path).contents;
  c.Expect(io::WriteAtomically(previous, kept).ok(), "a .old is staged");
  c.Expect(std::remove(path.c_str()) == 0, "and queue.json is taken away mid-commit");
  const download::LoadedQueue recovered = download::LoadQueue(path);
  c.ExpectEq(recovered.entries.size(), std::size_t{3},
             "the queue is recovered from the interrupted commit");
  c.Expect(!recovered.diagnostics.empty(), "and the recovery is not silent");
  c.Expect(download::SaveQueue(path, queue).ok(), "and the file is put back for what follows");

  // A queue that was drained is legitimately written as no entries at all, so
  // recovering an *empty* `.old` is a recovery and not a corruption. Reporting
  // it as one would log the state the module calls a perfectly good queue as
  // the state it calls a lost one.
  const std::string spare = sandbox.Host("/config/rommsync/drained.json");
  download::Queue nothing_queued;
  c.Expect(download::SaveQueue(spare, nothing_queued).ok(), "an empty queue is written");
  c.Expect(io::WriteAtomically(io::PreviousPathFor(spare), io::ReadFile(spare).contents).ok(),
           "and staged as a .old");
  c.Expect(std::remove(spare.c_str()) == 0, "with the file taken away mid-commit");
  const download::LoadedQueue drained = download::LoadQueue(spare);
  c.Expect(drained.entries.empty(), "the recovered queue is empty, as it was");
  c.Expect(drained.DescribeDiagnostics().find("recovered") != std::string::npos,
           "and the diagnostic says it was recovered, not that it was unusable: " +
               drained.DescribeDiagnostics());

  // A write that cannot happen costs the new queue and never the working one.
  download::Queue elsewhere;
  elsewhere.Enqueue(9, &position);
  const download::StoreResult refused =
      download::SaveQueue(sandbox.Host("/config/rommsync/nowhere/queue.json"), elsewhere);
  c.Expect(!refused.ok(), "a write into a directory that is not there fails");
  c.Expect(refused.error == download::StoreError::kOpenFailed, "and names why");
  c.Expect(!refused.message.empty(), "in a sentence");

  // The writer refuses what the reader would discard, rather than writing a file
  // that costs the next boot its whole queue.
  download::Queue oversized;
  QueueEntry huge = Populated();
  huge.message = std::string(download::kMaxMessageChars + 1, 'x');
  oversized.Reset({huge});
  const download::StoreResult too_big = download::SaveQueue(path, oversized);
  c.Expect(!too_big.ok(), "an entry the reader would refuse is not written");
  c.Expect(too_big.error == download::StoreError::kUnusableEntry,
           "and it is named as a bad entry rather than as a file that is too big -- the two "
           "send whoever reads the log to different places");
  c.Expect(!too_big.message.empty(), "and the refusal says which bound it hit");
  c.Expect(too_big.message.find(huge.fs_name) == std::string::npos,
           "without quoting the rom's name back into the log");
  c.ExpectEq(download::LoadQueue(path).entries.size(), std::size_t{3},
             "and the queue already on the card is untouched");
}

void Bounds(checks::Checks& c) {
  // The writer's two bounds have to agree, or a full queue would serialise to a
  // file the reader discards -- which costs a console its whole queue at the
  // next boot, silently.
  std::vector<QueueEntry> worst;
  for (std::size_t at = 0; at < download::kMaxQueueEntries; ++at) {
    QueueEntry entry;
    entry.rom_id = static_cast<std::int64_t>(at) + 1;
    entry.platform_fs_slug = std::string(32, 'p');
    entry.fs_name = std::string(config::kMaxPathLength, 'n');
    entry.destination = std::string(config::kMaxPathLength, 'd');
    entry.sha1_hash = std::string(40, 'a');
    entry.message = std::string(download::kMaxMessageChars, 'm');
    entry.size_bytes = 9'000'000'000;
    entry.bytes_done = 9'000'000'000;
    entry.attempts = 999'999;
    entry.queued_at = 253'402'300'799;
    worst.push_back(entry);
  }
  const std::string text = download::SerializeQueue(worst);
  c.Expect(text.size() <= download::kMaxQueueBytes,
           "a full queue of worst-case entries fits the byte bound (" + std::to_string(text.size()) +
               " of " + std::to_string(download::kMaxQueueBytes) + ")");
  c.ExpectEq(download::ParseQueue(text).entries.size(), download::kMaxQueueEntries,
             "...and reads back as the queue it was");
}

// --- the endpoints ------------------------------------------------------------

void Endpoint(checks::Checks& c) {
  c.ExpectEq(download::ContentUrl("https://romm.example.com", 4, "240pee.nes"),
             std::string("https://romm.example.com/api/roms/4/content/240pee.nes"),
             "the content URL is the endpoint docs/API_CONTRACT.md publishes");

  // Why the encoding is not optional: a raw space produces a request line no
  // server parses, and this is a name the seeded library actually has.
  c.ExpectEq(download::ContentUrl("http://h", 6, "Synthetic Two Disc Game"),
             std::string("http://h/api/roms/6/content/Synthetic%20Two%20Disc%20Game"),
             "a name with spaces is percent-encoded, as one segment");
  c.ExpectEq(download::ContentUrl("http://h", 1, "a/b"),
             std::string("http://h/api/roms/1/content/a%2Fb"),
             "a separator is encoded rather than passed through as a path");
  c.ExpectEq(download::ContentUrl("http://h", 1, "Where in Time (USA) [!].nes"),
             std::string("http://h/api/roms/1/content/"
                         "Where%20in%20Time%20%28USA%29%20%5B%21%5D.nes"),
             "and so is the punctuation a No-Intro name is full of");
  c.ExpectEq(download::ContentUrl("http://h", 1, "a-b_c.d~e"),
             std::string("http://h/api/roms/1/content/a-b_c.d~e"),
             "while the unreserved set is left alone");

  // The detail shape, held to `json::Reader`'s bar: a field that moved is a
  // named error rather than a struct that looks parsed.
  const std::string body =
      "{\"id\":4,\"fs_name\":\"240pee.nes\",\"platform_fs_slug\":\"nes\","
      "\"platform_slug\":\"nes\",\"fs_size_bytes\":65552,"
      "\"sha1_hash\":\"ff66e33efc818b516f7994f3027a72f4bc629b30\","
      "\"md5_hash\":\"06b44b6cbb2ecfca4325537ccb4d32a7\","
      "\"has_multiple_files\":false,\"missing_from_fs\":false}";
  download::RomDetail detail;
  c.Expect(download::ParseRomDetail(body, &detail).ok(), "a rom detail body reads");
  c.ExpectEq(detail.id, std::int64_t{4}, "with its id");
  c.ExpectEq(detail.fs_name, std::string("240pee.nes"), "its name");
  c.ExpectEq(detail.platform_fs_slug, std::string("nes"), "its fs slug -- not platform_slug");
  c.ExpectEq(detail.size_bytes, std::int64_t{65552}, "its size");
  c.ExpectEq(detail.sha1_hash, std::string("ff66e33efc818b516f7994f3027a72f4bc629b30"),
             "and its hash");
  c.ExpectEq(detail.md5_hash, std::string("06b44b6cbb2ecfca4325537ccb4d32a7"),
             "and the fallback the worker uses when a library has no sha1_hash");

  // An unscanned library leaves most fields null, which is the worst case the
  // client must survive (docs/API_CONTRACT.md).
  download::RomDetail unhashed;
  const std::string null_hash =
      "{\"id\":4,\"fs_name\":\"a.nes\",\"platform_fs_slug\":\"nes\",\"fs_size_bytes\":1,"
      "\"sha1_hash\":null,\"md5_hash\":null,\"has_multiple_files\":false,"
      "\"missing_from_fs\":false}";
  c.Expect(download::ParseRomDetail(null_hash, &unhashed).ok(), "a null sha1_hash is still a rom");
  c.Expect(unhashed.sha1_hash.empty() && unhashed.md5_hash.empty(),
           "with nothing to check the bytes against, which the worker records as unverified "
           "rather than passing off as a check");

  // ...and one that has only the fallback, which is the shape the worker's MD5
  // path exists for.
  download::RomDetail md5_only;
  const std::string no_sha1 =
      "{\"id\":4,\"fs_name\":\"a.nes\",\"platform_fs_slug\":\"nes\",\"fs_size_bytes\":1,"
      "\"sha1_hash\":null,\"md5_hash\":\"06b44b6cbb2ecfca4325537ccb4d32a7\","
      "\"has_multiple_files\":false,\"missing_from_fs\":false}";
  c.Expect(download::ParseRomDetail(no_sha1, &md5_only).ok(), "a library with only an MD5 reads");
  c.ExpectEq(md5_only.md5_hash, std::string("06b44b6cbb2ecfca4325537ccb4d32a7"),
             "and the fallback digest is what comes back");

  const auto refused = [&c, &body](const std::string& from, const std::string& to,
                                   std::string_view what) {
    std::string broken = body;
    const std::size_t at = broken.find(from);
    if (at == std::string::npos) {
      c.Expect(false, std::string(what) + ": the field to break was not found");
      return;
    }
    broken.replace(at, from.size(), to);
    download::RomDetail out;
    const json::Error error = download::ParseRomDetail(broken, &out);
    c.Expect(!error.ok(), std::string(what) + " is a named refusal");
    c.Expect(!error.Describe().empty(), std::string(what) + " says which field");
  };
  refused("\"fs_name\":\"240pee.nes\"", "\"fs_name\":null", "a rom with no name");
  refused("\"platform_fs_slug\":\"nes\"", "\"platform_fs_slugs\":\"nes\"",
          "a renamed platform_fs_slug");
  refused("\"fs_size_bytes\":65552", "\"fs_size_bytes\":\"65552\"", "a size sent as a string");
  refused("\"has_multiple_files\":false", "\"has_multiple_files\":0",
          "a multi-file flag sent as a number");
  refused("\"missing_from_fs\":false", "\"missing\":false", "a renamed missing_from_fs");
  refused("\"sha1_hash\":\"ff66e33efc818b516f7994f3027a72f4bc629b30\"", "\"sha1\":\"x\"",
          "a sha1_hash that is not on the body at all");
  // The same bar as `sha1_hash`, and for the same reason: a body with no
  // `md5_hash` key is not this schema, and reading it as "no fallback" would
  // turn a client pointed at the wrong server into a library nothing checked.
  refused("\"md5_hash\":\"06b44b6cbb2ecfca4325537ccb4d32a7\"", "\"md5\":\"x\"",
          "an md5_hash that is not on the body at all");
  refused("\"id\":4", "\"id\":0", "a rom id of zero");
  refused("\"fs_size_bytes\":65552", "\"fs_size_bytes\":-1", "a negative size");

  download::RomDetail nothing;
  c.Expect(!download::ParseRomDetail("{\"id\":4,", &nothing).ok(), "a truncated body is refused");
  c.Expect(!download::ParseRomDetail("", &nothing).ok(), "and so is an empty one");
}

// --- against the real library -------------------------------------------------

/// The real client, with a note of what the worker's own transfers answered.
///
/// The acceptance criterion for a resume is a **206**, and nothing the worker
/// hands back carries a status -- `DrainResult` is about entries. Wrapping the
/// client is the only way to assert on the response the *worker* got rather than
/// on one this test made to look like it: everything below the wrapper is the
/// engine's own request, built by the engine's own code.
class Watched : public http::HttpClient {
 public:
  explicit Watched(http::HttpClient& inner) : inner_(inner) {}

  http::Result Send(const http::Request& request) override { return inner_.Send(request); }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    http::Result result = inner_.Download(request, target);
    statuses.push_back(result.response.status);
    received.push_back(result.response.bytes_received);
    return result;
  }

  std::vector<int> statuses;
  std::vector<std::uint64_t> received;

 private:
  http::HttpClient& inner_;
};

/// What every rig scenario sets up: a sandbox standing in for the card, with the
/// built-in folder map's directories on it, an SD-rooted filesystem, and worker
/// options pointed at this worktree's RomM through the fault proxy.
class Rig {
 public:
  Rig(checks::Checks& checks, std::string_view name, const std::string& base,
      const harness::Fixture& fixture)
      : sandbox(checks, name),
        filesystem(rommsync::host::MakeNativeFileSystem(sandbox.root().string())) {
    // Creating a mapped folder is the platform layer's job, not the engine's
    // (atomic_file.hpp). A test that skipped this would be checking the failure
    // path for a folder that is not there, not a download.
    for (const char* folder :
         {"/tico/roms/nes", "/tico/roms/gb", "/tico/roms/gba", "/tico/roms/psx"}) {
      sandbox.MakeDirs(folder);
    }
    settings = Settings();
    options.base_url = base;
    options.bearer_token = fixture.token;
    options.queue_path = sandbox.Host("/config/rommsync/queue.json");
    // Nothing waits for real: the backoff is asserted on rather than spent, so a
    // red run costs seconds instead of a minute (`sync::NegotiateOptions::wait`).
    options.wait = [this](std::chrono::milliseconds delay) { slept += delay; };
  }

  download::DrainResult Drain(http::HttpClient& client) {
    return download::Drain(client, *filesystem, settings, queue, options);
  }

  /// The queue as the *card* holds it, not as memory does. Every assertion about
  /// a state transition reads this, because "written after every transition" is
  /// the property under test, and an in-memory check would pass with no file.
  download::LoadedQueue OnCard() const { return download::LoadQueue(options.queue_path); }

  QueueEntry Persisted(std::int64_t rom_id) const {
    for (const QueueEntry& entry : OnCard().entries) {
      if (entry.rom_id == rom_id) {
        return entry;
      }
    }
    return {};
  }

  harness::Sandbox sandbox;
  std::unique_ptr<fs::FileSystem> filesystem;
  config::Config settings;
  download::WorkerOptions options;
  download::Queue queue;
  std::chrono::milliseconds slept{0};
};

/// Enqueue the seeded rom named `fs_name`, and answer its id or `0`.
std::int64_t Queued(checks::Checks& c, http::HttpClient& client, const std::string& base,
                    const harness::Fixture& fixture, download::Queue* queue, const char* fs_name,
                    harness::Rom* rom) {
  if (!harness::FindRom(client, base, fixture, fs_name, rom)) {
    c.Expect(false, std::string("the seeded library holds ") + fs_name +
                        " -- re-seed it with ./server/testing/seed.sh");
    return 0;
  }
  std::int32_t position = 0;
  c.Expect(queue->Enqueue(rom->id, &position) == ipc::Error::kOk,
           std::string("the overlay queues ") + fs_name + " by id alone");
  return rom->id;
}

/// The rom as it sits in the fixture library, for a byte-for-byte comparison
/// with what reached the card.
std::string FixtureRom(const char* relative) {
  return rig::ReadFile(std::string(ROMMSYNC_LIBRARY_DIR) + "/" + relative);
}

void Drain(checks::Checks& c, http::HttpClient& client, const std::string& base,
           const harness::Fixture& fixture) {
  Rig rig(c, "download-drain", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  if (rom_id == 0) {
    return;
  }

  // `Enqueue` records an id and nothing else -- no IPC command may block on the
  // network (ipc.hpp) -- so everything below is the worker's own resolution.
  c.Expect(rig.queue.Find(rom_id).fs_name.empty(), "the enqueued entry knows only the rom id");

  const download::DrainResult result = rig.Drain(client);
  c.Expect(result.outcome == download::DrainOutcome::kCompleted,
           std::string("the drain completed -- got ") + download::ToString(result.outcome) + " (" +
               result.message + ")");
  c.ExpectEq(result.downloaded, 1, "one rom came down");
  c.ExpectEq(result.failed, 0, "and none failed");
  c.ExpectEq(rig.slept.count(), std::int64_t{0}, "with no backoff spent on a clean run");

  const std::string expected = FixtureRom("roms/nes/240pee.nes");
  c.Expect(!expected.empty(), "the fixture rom is readable");
  c.Expect(rig.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "the rom is at the destination the folder map names");
  c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == expected,
           "and its bytes are the fixture's, exactly");

  const QueueEntry entry = rig.Persisted(rom_id);
  c.Expect(entry.state == QueueState::kDone, "the entry on the card is done");
  c.ExpectEq(entry.destination, std::string("/tico/roms/nes/240pee.nes"),
             "and records where it went, as an SD path");
  c.ExpectEq(entry.fs_name, std::string("240pee.nes"), "with the name RomM gave it");
  c.ExpectEq(entry.platform_fs_slug, std::string("nes"), "keyed on the fs slug");
  c.ExpectEq(entry.bytes_done, static_cast<std::int64_t>(expected.size()),
             "and every byte accounted for");
  c.ExpectEq(entry.size_bytes, static_cast<std::int64_t>(expected.size()),
             "matching the size the server declared");
  c.Expect(!entry.sha1_hash.empty(), "the hash M3-3 will check is recorded");
  c.ExpectEq(crypto::Sha1FileHex(rig.sandbox.Host("/tico/roms/nes/240pee.nes")), entry.sha1_hash,
             "and the file on the card hashes to it");
  c.ExpectEq(entry.attempts, 0, "and it took no failed attempts");

  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp.part"),
           "no .part is left beside the finished rom");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"),
           "nor the staging file the hash was taken over");
  c.ExpectEq(rig.queue.pending(), std::size_t{0}, "nothing is left to do");
  c.ExpectEq(rig.queue.size(), std::size_t{1},
             "and the finished entry stays, for the queue screen");
  c.Expect(rig.queue.CurrentDownload().state == ipc::DownloadState::kIdle,
           "the worker is idle again");

  // A second drain is a no-op rather than a second download.
  const download::DrainResult again = rig.Drain(client);
  c.Expect(again.outcome == download::DrainOutcome::kIdle, "a drained queue is idle");
  c.ExpectEq(again.attempts, 0, "and costs no requests");

  // Queued again over the rom already on the card. #19 could not answer this --
  // without a digest there is no telling "already there and correct" from
  // "already there and truncated", and skipping the second is how a user is left
  // with a broken rom and a queue that says done. With the SHA-1 the answer is
  // honest, so the entry finishes with no content request at all.
  {
    std::int32_t position = 0;
    c.Expect(rig.queue.Enqueue(rom_id, &position) == ipc::Error::kOk,
             "a finished rom can be asked for again");
    const download::DrainResult present = rig.Drain(client);
    c.Expect(present.outcome == download::DrainOutcome::kCompleted,
             std::string("...and the drain finishes -- got ") +
                 download::ToString(present.outcome) + " (" + present.message + ")");
    c.ExpectEq(present.attempts, 1,
               "having spent one request: the rom detail, and no transfer at all");
    const QueueEntry skipped = rig.Persisted(rom_id);
    c.Expect(skipped.state == QueueState::kDone, "the entry is done");
    c.Expect(skipped.message.find("already on the card") != std::string::npos,
             "and says the rom was already there rather than claiming a download");
    c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == expected,
             "with the rom on the card untouched");
    c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"),
             "and nothing staged beside it");
  }

  // The same rom again, with the file on the card no longer the rom. It is the
  // right *length*, so only the hash can tell -- and it does: the skip does not
  // fire, the worker goes to fetch it, and `bytes_done` is progress towards
  // *this* transfer rather than a count of the file it is about to replace,
  // which would show the overlay's bar at 100% for the whole download (#22).
  {
    c.Expect(rig.sandbox.Write("/tico/roms/nes/240pee.nes", std::string(expected.size(), 'Z')),
             "a file of the right length and the wrong bytes is on the card");
    std::int32_t position = 0;
    c.Expect(rig.queue.Enqueue(rom_id, &position) == ipc::Error::kOk,
             "and the rom is queued again");
    rig.options.max_attempts = 1;
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"status\",\"status\":500,\"count\":9,\"path\":\"/api/roms/" +
                             std::to_string(rom_id) + "/content\"}");
    const download::DrainResult requeued = rig.Drain(client);
    c.Expect(requeued.outcome == download::DrainOutcome::kRetryable,
             std::string("a corrupt file on the card is fetched again, not skipped -- got ") +
                 download::ToString(requeued.outcome));
    c.ExpectEq(rig.Persisted(rom_id).bytes_done, std::int64_t{0},
               "with no progress claimed for a transfer that has not moved a byte");
    c.ExpectEq(rig.sandbox.Read("/tico/roms/nes/240pee.nes"), std::string(expected.size(), 'Z'),
               "and the file already there is left alone until verified bytes replace it");
  }
}

void Retries(checks::Checks& c, http::HttpClient& client, const std::string& base,
             const harness::Fixture& fixture) {
  Rig rig(c, "download-retries", base, fixture);
  harness::Rom rom;
  harness::Rom behind;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  // A second, healthy rom behind the failing one. Without it this scenario says
  // nothing about the *queue*: a worker that stopped the whole drain at the
  // first retryable entry would starve the sixty-three behind it, and a
  // one-entry queue cannot tell that apart from correct behaviour.
  const std::int64_t behind_id =
      Queued(c, client, base, fixture, &rig.queue, "gb240p.gb", &behind);
  if (rom_id == 0 || behind_id == 0) {
    return;
  }

  {
    // Scoped to this rom's own path. The acceptance criterion writes
    // `"path":"/api/roms/"`, which is a prefix of `GET /api/roms/{id}` *and* of
    // the content request underneath it -- and the proxy consumes a scenario on
    // the first matching request whether or not it changed anything
    // (fault_proxy.py's `claim`). A count large enough to outlast the whole
    // retry budget is what actually forces the failure this is about.
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"status\",\"status\":500,\"count\":9,\"path\":\"/api/roms/" +
                             std::to_string(rom_id) + "\"}");
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kRetryable,
             std::string("a 500 is retryable, not fatal -- got ") +
                 download::ToString(result.outcome));
    c.Expect(rig.slept.count() > 0, "and the backoff was actually asked for");
    c.Expect(rig.slept >= rig.options.backoff, "at least the first delay");
    c.ExpectEq(result.failed, 0, "nothing was written off as failed");

    // The drain goes round the entry that cannot be served rather than stopping
    // at it. The failing rom is the *head* of the queue, so this is exactly the
    // arrangement a head-of-line stall would show up in.
    c.ExpectEq(result.downloaded, 1, "the healthy rom behind it still came down");
    c.Expect(rig.sandbox.Read("/tico/roms/gb/gb240p.gb") == FixtureRom("roms/gb/gb240p.gb"),
             "byte for byte");
    c.Expect(rig.Persisted(behind_id).state == QueueState::kDone, "and its entry is done");
  }

  const QueueEntry entry = rig.Persisted(rom_id);
  c.ExpectEq(entry.rom_id, rom_id, "the failing entry is still in the file on the card");
  c.Expect(entry.state == QueueState::kQueued, "still queued, never dropped");
  c.ExpectEq(entry.attempts, rig.options.max_attempts, "with its attempts counted");
  c.Expect(!entry.message.empty(), "and a reason a user can read");
  c.Expect(entry.message.find("500") != std::string::npos, "naming the status the server sent");
  c.ExpectEq(rig.queue.pending(), std::size_t{1}, "and it is still the worker's to do");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "nothing reached the destination for the rom that could not be resolved");

  // The fault is disarmed, so the same queue drains. That is what "never
  // dropped" is worth: the retry is the *next* drain, not a lost download.
  const download::DrainResult recovered = rig.Drain(client);
  c.Expect(recovered.outcome == download::DrainOutcome::kCompleted,
           std::string("the next drain finishes it -- got ") +
               download::ToString(recovered.outcome) + " (" + recovered.message + ")");
  c.ExpectEq(recovered.downloaded, 1, "the rom that was retried came down");
  c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
           "byte for byte");
  c.Expect(rig.Persisted(rom_id).state == QueueState::kDone, "and the entry is done");
  c.ExpectEq(rig.Persisted(rom_id).attempts, rig.options.max_attempts,
             "with what it cost still recorded");

  // The retry budget belongs to the *entry*, not to each half of its turn.
  //
  // This is the case the 500 above cannot see: there the detail call fails, so
  // only the first of the worker's two request loops ever runs. Here the detail
  // call succeeds and the transfer is what keeps failing, so a budget spent
  // separately by each half would issue one request more than `max_attempts`
  // and wait one more backoff for this single entry.
  {
    harness::Rom nova;
    const std::int64_t nova_id = Queued(c, client, base, fixture, &rig.queue, "nova.nes", &nova);
    if (nova_id == 0) {
      return;
    }
    harness::Fault fault(
        c, client, base,
        "{\"mode\":\"drop\",\"bytes\":1024,\"count\":9,\"path\":\"/api/roms/" +
            std::to_string(nova_id) + "/content\"}");
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kRetryable,
             std::string("a transfer that keeps dropping is retryable -- got ") +
                 download::ToString(result.outcome));
    c.ExpectEq(result.attempts, rig.options.max_attempts,
               "one entry's turn costs one entry's budget: the detail call and the transfers "
               "share it rather than each getting their own");
    c.ExpectEq(rig.Persisted(nova_id).attempts, rig.options.max_attempts - 1,
               "...so the transfer failed for every request left after the detail call");
    c.Expect(rig.Persisted(nova_id).state == QueueState::kQueued, "and the entry is still queued");
    c.Expect(!rig.sandbox.Exists("/tico/roms/nes/nova.nes"),
             "with no short file where a complete rom is expected");
    c.Expect(!rig.sandbox.Exists("/tico/roms/nes/nova.nes.tmp"),
             "and nothing staged, because no body ever completed");
  }
}

void Resume(checks::Checks& c, http::HttpClient& client, const std::string& base,
            const harness::Fixture& fixture) {
  Rig rig(c, "download-resume", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id =
      Queued(c, client, base, fixture, &rig.queue, "synthetic-large.gba", &rom);
  if (rom_id == 0) {
    return;
  }
  constexpr std::int64_t kDropAt = 4 * 1024 * 1024;

  {
    // One attempt, so the drop is the end of *this* drain rather than something
    // the retry inside it papers over. The acceptance criterion is that a second
    // drain finishes what the first one started.
    rig.options.max_attempts = 1;
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"drop\",\"bytes\":" + std::to_string(kDropAt) +
                             ",\"path\":\"/api/roms/" + std::to_string(rom_id) + "/content\"}");
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kRetryable,
             std::string("a dropped connection is retryable -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
  }

  c.Expect(!rig.sandbox.Exists("/tico/roms/gba/synthetic-large.gba"),
           "no short file is left where a complete rom is expected");
  // `<destination>.tmp.part`, not `<destination>.part`: the worker points the
  // backend at the staging file rather than at the destination, so that the
  // rename the backend does when the *body* completes cannot put unverified
  // bytes where a rom goes (download.hpp).
  c.Expect(rig.sandbox.Exists("/tico/roms/gba/synthetic-large.gba.tmp.part"),
           "the bytes that did arrive are kept in the .part under the staging name");
  c.Expect(!rig.sandbox.Exists("/tico/roms/gba/synthetic-large.gba.tmp"),
           "and nothing is staged, because no body completed");
  const std::uintmax_t partial =
      rig.sandbox.SizeOf("/tico/roms/gba/synthetic-large.gba.tmp.part");
  c.Expect(partial > 0, "which is not empty");
  c.Expect(partial < static_cast<std::uintmax_t>(rom.size), "and is not the whole rom either");

  const QueueEntry stopped = rig.Persisted(rom_id);
  c.Expect(stopped.state == QueueState::kQueued, "the entry is queued again, not failed");
  c.ExpectEq(stopped.bytes_done, static_cast<std::int64_t>(partial),
             "and its bytes_done is what is on the card, so a bar carries on rather than "
             "restarting at zero");
  c.ExpectEq(stopped.attempts, 1, "one failed attempt is recorded");

  // The second drain resumes from that `.part`, and this is where the **206** is
  // asserted: the request goes through `Watched`, so what is checked is the
  // response the worker's own transfer got, not one this test constructed. That
  // RomM honours `Range` at all is `harness.resume`'s; that the worker asks for
  // one and gets a partial answer rather than the whole resource is this issue's.
  rig.options.max_attempts = 3;
  Watched watched(client);
  const download::DrainResult finished = rig.Drain(watched);
  c.Expect(finished.outcome == download::DrainOutcome::kCompleted,
           std::string("the second drain finishes it -- got ") +
               download::ToString(finished.outcome) + " (" + finished.message + ")");
  c.ExpectEq(finished.downloaded, 1, "the interrupted rom came down");
  c.ExpectEq(watched.statuses.size(), std::size_t{1}, "in exactly one transfer");
  if (watched.statuses.size() == 1) {
    c.ExpectEq(watched.statuses.front(), 206,
               "which the server answered 206 -- a 200 would be the whole resource again, and "
               "the download restarted rather than resumed");
    c.ExpectEq(watched.received.front(), static_cast<std::uint64_t>(rom.size) - partial,
               "fetching exactly the bytes that were missing and no more");
  }
  c.Expect(!rig.sandbox.Exists("/tico/roms/gba/synthetic-large.gba.tmp.part"),
           "and the .part is consumed rather than left beside it");
  c.Expect(!rig.sandbox.Exists("/tico/roms/gba/synthetic-large.gba.tmp"),
           "as is the staging file the hash was taken over");
  c.Expect(rig.sandbox.Read("/tico/roms/gba/synthetic-large.gba") ==
               FixtureRom("roms/gba/synthetic-large.gba"),
           "with the whole rom byte-identical to the fixture");
  const QueueEntry done = rig.Persisted(rom_id);
  c.Expect(done.state == QueueState::kDone, "the entry on the card is done");
  c.Expect(done.message.find("verified") != std::string::npos,
           "and says the bytes were checked against the server's own digest");
  // The acceptance criterion in full: not "some SHA-1 matched" but "the SHA-1 of
  // the file on the card equals the one RomM recorded for this rom". Recomputed
  // here rather than trusted from the message, and compared against the value
  // the *server* gave -- the entry's own `sha1_hash`, resolved from
  // `GET /api/roms/{id}` and persisted.
  c.Expect(!done.sha1_hash.empty(), "the entry carries the hash the server recorded");
  c.ExpectEq(crypto::Sha1FileHex(rig.sandbox.Host("/tico/roms/gba/synthetic-large.gba")),
             done.sha1_hash,
             "and the 120 MiB rom on the card hashes to exactly that");

  // The assertions above are all satisfied by a second drain that *restarted*
  // from zero, so on their own they do not pin the property the issue names --
  // "an entry left `kActive` by a power cut is **resumed** from its `.part`, not
  // restarted". This does, by making the two outcomes different bytes: seed the
  // in-flight file with contents that are not the rom's and see what happens.
  //
  // Under M3-2 what happened was a file of the right *length* and the wrong
  // *bytes* at the destination, with the entry marked `kDone` -- the failing
  // case this issue was written down against. Now the resume still happens, the
  // SHA-1 catches it, and because the bytes it built on came off the card rather
  // than off the wire, the worker throws them away and fetches the rom clean.
  // The user ends up with the rom; what they never end up with is the wrong one.
  {
    harness::Rom nova;
    const std::int64_t nova_id = Queued(c, client, base, fixture, &rig.queue, "nova.nes", &nova);
    if (nova_id == 0) {
      return;
    }
    const std::string seeded(1024, 'X');
    c.Expect(rig.sandbox.Write("/tico/roms/nes/nova.nes.tmp.part", seeded),
             "an in-flight file from an interrupted transfer is on the card");

    Watched spliced(client);
    const download::DrainResult result = rig.Drain(spliced);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("the drain finishes -- got ") + download::ToString(result.outcome) + " (" +
                 result.message + ")");
    c.ExpectEq(result.downloaded, 1, "the rom does arrive");
    c.ExpectEq(result.failed, 0, "and the entry is not written off over a bad local file");

    // Two transfers, and what each one was is the whole point. The first is the
    // resume: 206, only the missing bytes, built on a prefix that is not this
    // rom's. The second is the clean fetch that follows the digest saying so.
    c.ExpectEq(spliced.statuses.size(), std::size_t{2},
               "in two transfers -- the spliced one, and the clean one after it");
    if (spliced.statuses.size() == 2) {
      c.ExpectEq(spliced.statuses.front(), 206,
                 "the transfer did resume -- the server answered 206 and sent only the rest");
      c.ExpectEq(spliced.received.front(),
                 static_cast<std::uint64_t>(nova.size) - seeded.size(),
                 "so the bytes that were already there were kept, and are the wrong bytes");
      c.ExpectEq(spliced.statuses.back(), 200,
                 "and the retry asked for the whole resource, having thrown the prefix away");
      c.ExpectEq(spliced.received.back(), static_cast<std::uint64_t>(nova.size),
                 "every byte of it");
    }

    const QueueEntry landed = rig.Persisted(nova_id);
    c.Expect(landed.state == QueueState::kDone, "the entry is done");
    c.ExpectEq(landed.attempts, 1,
               "with the spliced attempt counted against it -- the digest failing is a failed "
               "attempt, not a free one");
    c.Expect(rig.sandbox.Read("/tico/roms/nes/nova.nes") == FixtureRom("roms/nes/nova.nes"),
             "and the rom at the destination is the fixture, byte for byte -- under M3-2 this "
             "was the right length and the wrong bytes, marked done");
    c.Expect(!rig.sandbox.Exists("/tico/roms/nes/nova.nes.tmp"),
             "with nothing staged left over");
    c.Expect(!rig.sandbox.Exists("/tico/roms/nes/nova.nes.tmp.part"), "nor any .part");
  }
}

/// A `DetailedRomSchema` body for the fault proxy to answer a detail call with.
///
/// Only the six fields `ParseRomDetail` reads, and the *real* parser is what
/// reads them. This is how a library that is healthy on purpose is made to look
/// like one that recorded the wrong hash, or none at all, without touching the
/// fixture every other scenario shares -- the same trick `missing` uses for
/// `missing_from_fs`. An empty digest is written as JSON `null`, which is what
/// an unscanned RomM sends.
std::string DetailBody(const harness::Rom& rom, const char* slug, const std::string& sha1,
                       const std::string& md5) {
  const auto quoted = [](const std::string& value) {
    return value.empty() ? std::string("null") : json::Quote(value);
  };
  return "{\"id\":" + std::to_string(rom.id) + ",\"fs_name\":" + json::Quote(rom.fs_name) +
         ",\"platform_fs_slug\":\"" + slug + "\",\"fs_size_bytes\":" + std::to_string(rom.size) +
         ",\"sha1_hash\":" + quoted(sha1) + ",\"md5_hash\":" + quoted(md5) +
         ",\"has_multiple_files\":false,\"missing_from_fs\":false}";
}

/// Arm the proxy to answer this rom's *detail* call with `body`.
///
/// `count` stays at its default of one, deliberately: `/api/roms/{id}` is a
/// prefix of `/api/roms/{id}/content`, so a scenario that stayed armed would
/// also answer the transfer with this JSON. One is exactly right, because the
/// detail call is the first request an entry's turn makes.
std::string DetailFault(std::int64_t rom_id, const std::string& body) {
  return "{\"mode\":\"status\",\"status\":200,\"path\":\"/api/roms/" + std::to_string(rom_id) +
         "\",\"body\":" + json::Quote(body) + "}";
}

/// The hash is load-bearing, not decorative.
///
/// A rom whose recorded digest does not describe its bytes fails the entry and
/// leaves *nothing* at the destination -- not the bytes, not a staging file, not
/// a `.part`. This is the criterion the whole staging decision exists for: a
/// worker that hashed the destination would have to delete a rom the card had
/// already accepted, and a power cut in that window leaves it there.
void Verify(checks::Checks& c, http::HttpClient& client, const std::string& base,
            const harness::Fixture& fixture) {
  Rig rig(c, "download-verify", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  if (rom_id == 0) {
    return;
  }
  c.Expect(!rom.sha1_hash.empty(), "the seeded library recorded a sha1_hash for this rom");

  // 40 zeroes: a well-formed digest that describes nothing. The bytes RomM
  // serves are the real ones, so what fails is the comparison and nothing else.
  {
    harness::Fault fault(c, client, base,
                         DetailFault(rom_id, DetailBody(rom, "nes", std::string(40, '0'), "")));
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("the drain finishes rather than retrying a hash forever -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.ExpectEq(result.failed, 1, "the entry is written off as failed");
    c.ExpectEq(result.downloaded, 0, "and nothing is reported as a rom that arrived");
  }

  const QueueEntry entry = rig.Persisted(rom_id);
  c.Expect(entry.state == QueueState::kFailed, "the entry on the card is failed");
  c.Expect(entry.message.find("SHA-1") != std::string::npos,
           "with a sentence naming what did not match");
  c.Expect(entry.message.find(rom.fs_name) == std::string::npos,
           "that does not quote the rom's name back into the queue file");
  c.ExpectEq(entry.bytes_done, std::int64_t{0}, "and claims no bytes on the card");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "nothing reached the destination -- not even a file of exactly the right length");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"),
           "the staged bytes are discarded");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp.part"),
           "and so is the .part, so a later attempt does not resume bytes already known wrong");
  c.ExpectEq(rig.queue.pending(), std::size_t{0},
             "the entry is finished with: a server whose bytes disagree with its own hash "
             "answers the same way next time");

  // The same rom against the library's real digest. Without this the scenario
  // would pass just as well for a worker that failed every download.
  std::int32_t position = 0;
  c.Expect(rig.queue.Enqueue(rom_id, &position) == ipc::Error::kOk, "the rom is queued again");
  const download::DrainResult good = rig.Drain(client);
  c.Expect(good.outcome == download::DrainOutcome::kCompleted,
           std::string("and with the real hash it completes -- got ") +
               download::ToString(good.outcome) + " (" + good.message + ")");
  c.ExpectEq(good.downloaded, 1, "the rom came down");
  c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
           "byte for byte");
  const QueueEntry verified = rig.Persisted(rom_id);
  c.Expect(verified.state == QueueState::kDone, "the entry is done");
  c.Expect(verified.message.find("verified") != std::string::npos,
           "and says the bytes were checked, which is the difference between this and M3-2");
}

/// What a download is worth when there is no SHA-1 to check it against.
///
/// Three answers, and none of them is "call it verified": the `md5_hash`
/// fallback for a library that recorded one and not the other, an honest
/// "nothing checked this" for a library that recorded neither, and the same for
/// `verify_hash = false`.
void Fallback(checks::Checks& c, http::HttpClient& client, const std::string& base,
              const harness::Fixture& fixture) {
  Rig rig(c, "download-fallback", base, fixture);

  // 1. `sha1_hash` null, `md5_hash` the library's own. An unscanned RomM leaves
  // most fields null, so this is not a hypothetical shape.
  {
    harness::Rom rom;
    const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
    if (rom_id == 0) {
      return;
    }
    c.Expect(!rom.md5_hash.empty(), "the seeded library recorded an md5_hash for this rom");
    harness::Fault fault(c, client, base,
                         DetailFault(rom_id, DetailBody(rom, "nes", "", rom.md5_hash)));
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("a library with only an MD5 still downloads -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
             "byte for byte");
    const QueueEntry entry = rig.Persisted(rom_id);
    c.Expect(entry.state == QueueState::kDone, "the entry is done");
    c.Expect(entry.message.find("MD5") != std::string::npos,
             "and names the digest it fell back to, so a log says which check was made");
  }

  // 2. Neither digest. Recorded as unverified rather than passed off as a check:
  // "we could not tell" and "we checked" are different things to say to someone
  // whose rom will not boot.
  {
    harness::Rom rom;
    const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "gb240p.gb", &rom);
    if (rom_id == 0) {
      return;
    }
    harness::Fault fault(c, client, base, DetailFault(rom_id, DetailBody(rom, "gb", "", "")));
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("a rom with no hash at all still downloads -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.Expect(rig.sandbox.Read("/tico/roms/gb/gb240p.gb") == FixtureRom("roms/gb/gb240p.gb"),
             "byte for byte");
    const QueueEntry entry = rig.Persisted(rom_id);
    c.Expect(entry.state == QueueState::kDone, "the entry is done -- the user asked for the rom");
    c.Expect(entry.message.find("no hash") != std::string::npos,
             "with a sentence saying nothing could check it");
    c.Expect(entry.message.find("verified") == std::string::npos,
             "and it never claims the bytes were verified");
  }

  // 3. `verify_hash = false`, against a hash that is deliberately wrong. The
  // rom lands, which is the only way to show the check was genuinely skipped
  // rather than skipped-and-passed by accident.
  {
    rig.settings.downloads.verify_hash = false;
    harness::Rom rom;
    const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "nova.nes", &rom);
    if (rom_id == 0) {
      return;
    }
    harness::Fault fault(c, client, base,
                         DetailFault(rom_id, DetailBody(rom, "nes", std::string(40, '0'), "")));
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("with verify_hash off the wrong hash is never consulted -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.Expect(rig.sandbox.Read("/tico/roms/nes/nova.nes") == FixtureRom("roms/nes/nova.nes"),
             "and the rom arrives, byte for byte");
    const QueueEntry entry = rig.Persisted(rom_id);
    c.Expect(entry.state == QueueState::kDone, "the entry is done");
    c.Expect(entry.message.find("verify_hash") != std::string::npos,
             "and says on the entry that the check was switched off");
    c.Expect(entry.message.find("verified") == std::string::npos,
             "never reporting a rom nothing looked at as verified");
  }
}

/// A body that ends early with no `Content-Length` to catch it.
///
/// `truncate` sends no length at all, so the server's own declaration cannot
/// find it: only `DownloadTarget::expected_size`, which the worker always sets
/// from `fs_size_bytes`, turns a clean short response into `Error::kTruncated`.
/// Without it a 1 KiB prefix of a 64 KiB rom is a perfectly plausible download.
void Truncate(checks::Checks& c, http::HttpClient& client, const std::string& base,
              const harness::Fixture& fixture) {
  Rig rig(c, "download-truncate", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  if (rom_id == 0) {
    return;
  }
  constexpr std::int64_t kCutAt = 1024;

  {
    // One attempt, so the cut is the end of *this* drain rather than something a
    // retry inside it papers over.
    rig.options.max_attempts = 1;
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"truncate\",\"bytes\":" + std::to_string(kCutAt) +
                             ",\"count\":9,\"path\":\"/api/roms/" + std::to_string(rom_id) +
                             "/content\"}");
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kRetryable,
             std::string("a short body is retryable, not a finished download -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.ExpectEq(result.downloaded, 0, "and nothing is reported as a rom that arrived");
  }

  const QueueEntry entry = rig.Persisted(rom_id);
  c.Expect(entry.state == QueueState::kQueued, "the entry is queued again, not failed");
  c.ExpectEq(entry.attempts, 1, "with one failed attempt recorded");
  c.Expect(entry.message.find("body ended early") != std::string::npos,
           "and the reason is the truncation the caller's own expected size caught");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "nothing reached the destination");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"),
           "and nothing was staged, because no body ever completed");
  c.ExpectEq(rig.sandbox.SizeOf("/tico/roms/nes/240pee.nes.tmp.part"),
             static_cast<std::uintmax_t>(kCutAt),
             "the prefix that did arrive is kept, because a resume can build on it");

  // ...which the next drain does, the fault having disarmed itself.
  rig.options.max_attempts = 3;
  const download::DrainResult recovered = rig.Drain(client);
  c.Expect(recovered.outcome == download::DrainOutcome::kCompleted,
           std::string("the next drain finishes it -- got ") +
               download::ToString(recovered.outcome) + " (" + recovered.message + ")");
  c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
           "with the whole rom byte-identical to the fixture");
  c.Expect(rig.Persisted(rom_id).message.find("verified") != std::string::npos,
           "and the seam between the two halves checked against the server's own SHA-1");
}

/// A server that accepts the request and then says nothing.
///
/// The transfer has no total timeout on purpose -- a 120 MiB rom on hotel Wi-Fi
/// is slow rather than dead -- so `stall_timeout` is the only thing that ends
/// this, and it has to be the *client's* own. The proxy's 8 seconds is the
/// control: giving up before then is the client deciding, and giving up at 8 is
/// the server having answered.
void Stall(checks::Checks& c, http::HttpClient& client, const std::string& base,
           const harness::Fixture& fixture) {
  Rig rig(c, "download-stall", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  if (rom_id == 0) {
    return;
  }
  constexpr int kStallSeconds = 8;

  {
    rig.options.max_attempts = 1;
    rig.options.stall_timeout = std::chrono::milliseconds{2'000};
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"stall\",\"seconds\":" + std::to_string(kStallSeconds) +
                             ",\"path\":\"/api/roms/" + std::to_string(rom_id) + "/content\"}");
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    const download::DrainResult result = rig.Drain(client);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    c.Expect(result.outcome == download::DrainOutcome::kRetryable,
             std::string("a stalled transfer is retryable, not fatal -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.Expect(elapsed < std::chrono::seconds{kStallSeconds},
             "and it gave up on its own stall_timeout rather than waiting out the server -- "
             "took " + std::to_string(elapsed.count()) + " ms");
  }

  const QueueEntry entry = rig.Persisted(rom_id);
  c.Expect(entry.state == QueueState::kQueued, "the entry is queued again, never dropped");
  c.ExpectEq(entry.attempts, 1, "with one failed attempt recorded");
  c.Expect(entry.message.find("timed out") != std::string::npos,
           "and a reason a user can read");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "nothing reached the destination");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"), "and nothing was staged");

  // The retry is the *next* drain, which is what "the entry is retried" is worth.
  rig.options.max_attempts = 3;
  rig.options.stall_timeout = rommsync::http::kDefaultStallTimeout;
  const download::DrainResult recovered = rig.Drain(client);
  c.Expect(recovered.outcome == download::DrainOutcome::kCompleted,
           std::string("the next drain finishes it -- got ") +
               download::ToString(recovered.outcome) + " (" + recovered.message + ")");
  c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
           "and the rom arrives, byte for byte");
}

/// What the worker does with files an interrupted attempt left on the card.
///
/// Three of them, and the rule is the same each time: **bytes this turn did not
/// fetch are never trusted, only checked.** A staging file the digest recognises
/// is worth a whole transfer and is committed; one it does not is discarded; and
/// an in-flight file that cannot be a prefix of the rom is discarded before a
/// request is even made. Without these the first is a needless 120 MiB, and the
/// third is a request spent learning the server's 416.
void Staging(checks::Checks& c, http::HttpClient& client, const std::string& base,
             const harness::Fixture& fixture) {
  Rig rig(c, "download-staging", base, fixture);

  // 1. An in-flight file longer than the rom. It is another rom's, or another
  // release's; resuming from it would ask for a range past the end.
  {
    harness::Rom rom;
    const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
    if (rom_id == 0) {
      return;
    }
    c.Expect(rig.sandbox.Write("/tico/roms/nes/240pee.nes.tmp.part",
                               std::string(static_cast<std::size_t>(rom.size) + 4096, 'Q')),
             "an over-long .part is on the card");
    Watched watched(client);
    const download::DrainResult result = rig.Drain(watched);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("the drain completes -- got ") + download::ToString(result.outcome) +
                 " (" + result.message + ")");
    c.ExpectEq(watched.statuses.size(), std::size_t{1}, "in one transfer");
    if (watched.statuses.size() == 1) {
      c.ExpectEq(watched.statuses.front(), 200,
                 "which asked for the whole resource -- a 206 would mean the over-long file was "
                 "resumed from, and the request spent finding out it could not be");
    }
    c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
             "and the rom arrives, byte for byte");
  }

  // 2. A complete staging file holding the rom -- what a power cut in the window
  // between the backend's rename and the hash leaves behind. The digest can say
  // those bytes are the rom, and saying so is worth the whole transfer.
  {
    harness::Rom rom;
    const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "gb240p.gb", &rom);
    if (rom_id == 0) {
      return;
    }
    c.Expect(rig.sandbox.Write("/tico/roms/gb/gb240p.gb.tmp", FixtureRom("roms/gb/gb240p.gb")),
             "a complete body is staged on the card");
    Watched watched(client);
    const download::DrainResult result = rig.Drain(watched);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("the drain completes -- got ") + download::ToString(result.outcome) +
                 " (" + result.message + ")");
    c.ExpectEq(watched.statuses.size(), std::size_t{0},
               "with no transfer at all -- the bytes were already there and the digest said so");
    c.ExpectEq(result.attempts, 1, "one request was spent, and it was the rom detail");
    c.Expect(rig.sandbox.Read("/tico/roms/gb/gb240p.gb") == FixtureRom("roms/gb/gb240p.gb"),
             "and the staged bytes were committed to the destination");
    c.Expect(!rig.sandbox.Exists("/tico/roms/gb/gb240p.gb.tmp"),
             "with the staging file consumed rather than left beside it");
    const QueueEntry entry = rig.Persisted(rom_id);
    c.Expect(entry.state == QueueState::kDone, "the entry is done");
    c.Expect(entry.message.find("verified") != std::string::npos,
             "and says so because it was checked, not because it was assumed");
  }

  // 3. A complete staging file of the right *length* holding something else --
  // an interrupted transfer of a rom the server has since replaced. Discarded,
  // and the rom fetched.
  {
    harness::Rom rom;
    const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "nova.nes", &rom);
    if (rom_id == 0) {
      return;
    }
    c.Expect(rig.sandbox.Write("/tico/roms/nes/nova.nes.tmp",
                               std::string(static_cast<std::size_t>(rom.size), 'W')),
             "a complete body of the right length and the wrong bytes is staged");
    Watched watched(client);
    const download::DrainResult result = rig.Drain(watched);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("the drain completes -- got ") + download::ToString(result.outcome) +
                 " (" + result.message + ")");
    c.ExpectEq(watched.statuses.size(), std::size_t{1},
               "having fetched the rom rather than committing what was staged");
    c.Expect(rig.sandbox.Read("/tico/roms/nes/nova.nes") == FixtureRom("roms/nes/nova.nes"),
             "and the destination holds the fixture, byte for byte");
    c.ExpectEq(rig.Persisted(rom_id).attempts, 0,
               "with nothing counted against the entry -- a bad file on the card is not a "
               "failed request");
  }
}

void Disabled(checks::Checks& c, http::HttpClient& client, const std::string& base,
              const harness::Fixture& fixture) {
  Rig rig(c, "download-disabled", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  if (rom_id == 0) {
    return;
  }
  c.Expect(download::SaveQueue(rig.options.queue_path, rig.queue).ok(),
           "the queue is on the card before downloads are switched off");

  rig.settings.downloads.enabled = false;
  const download::DrainResult idle = rig.Drain(client);
  c.Expect(idle.outcome == download::DrainOutcome::kDisabled,
           std::string("the worker idles -- got ") + download::ToString(idle.outcome));
  c.ExpectEq(idle.attempts, 0, "having sent nothing");
  c.ExpectEq(idle.downloaded, 0, "and downloaded nothing");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes"), "nothing reached the card");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp.part"), "not even a partial one");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"), "nor a staged one");

  // The point of the criterion: it loses nothing. Both the queue in memory and
  // the file on the card still hold the entry, unchanged.
  c.ExpectEq(rig.queue.pending(), std::size_t{1}, "the queue still holds what was asked for");
  const QueueEntry kept = rig.Persisted(rom_id);
  c.ExpectEq(kept.rom_id, rom_id, "and so does the file on the card");
  c.Expect(kept.state == QueueState::kQueued, "still queued");
  c.ExpectEq(kept.attempts, 0, "with nothing counted against it");
  c.Expect(kept.message.empty(), "and no failure recorded, because there was none");

  // Switching it back on resumes exactly what was there.
  rig.settings.downloads.enabled = true;
  const download::DrainResult resumed = rig.Drain(client);
  c.Expect(resumed.outcome == download::DrainOutcome::kCompleted,
           std::string("switching downloads back on drains it -- got ") +
               download::ToString(resumed.outcome) + " (" + resumed.message + ")");
  c.Expect(rig.sandbox.Read("/tico/roms/nes/240pee.nes") == FixtureRom("roms/nes/240pee.nes"),
           "and the rom arrives, byte for byte");
}

void Multifile(checks::Checks& c, http::HttpClient& client, const std::string& base,
               const harness::Fixture& fixture) {
  Rig rig(c, "download-multifile", base, fixture);
  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "Synthetic Two Disc Game", &rom)) {
    c.Expect(false, "the seeded library holds the two-disc rom");
    return;
  }
  c.Expect(rom.has_multiple_files, "which RomM reports as a multi-file rom");

  // The door: an engine holding the library refuses before anything is queued,
  // so the overlay can say so while the user is still looking at the rom. The
  // index comes off the live server rather than a literal, because the field
  // that decides this is RomM's.
  roms::FetchOptions fetch;
  fetch.base_url = base;
  fetch.bearer_token = fixture.token;
  const roms::FetchResult library = roms::FetchRomIndex(client, fetch);
  c.Expect(library.ok(), "the rom index fetches: " + library.message);
  std::int32_t position = 0;
  download::Queue guarded;
  c.Expect(download::EnqueueRom(guarded, library.index, rom.id, &position) ==
               ipc::Error::kMultiFile,
           "a disc set is refused at the door, from the library the engine already holds");
  c.ExpectEq(guarded.size(), std::size_t{0}, "leaving no entry behind");

  // The backstop: an entry that reached the queue anyway -- an id queued by a
  // build with no index, or one whose library changed underneath it -- is
  // skipped by the worker with a reason rather than turned into an archive.
  c.Expect(rig.queue.Enqueue(rom.id, &position) == ipc::Error::kOk,
           "the queue itself takes any id, because it has no library to check against");
  const download::DrainResult result = rig.Drain(client);
  c.Expect(result.outcome == download::DrainOutcome::kCompleted,
           std::string("the drain finishes -- got ") + download::ToString(result.outcome) + " (" +
               result.message + ")");
  c.ExpectEq(result.skipped, 1, "having skipped one rom");
  c.ExpectEq(result.downloaded, 0, "and downloaded none");

  const QueueEntry entry = rig.Persisted(rom.id);
  c.Expect(entry.state == QueueState::kSkipped, "the entry is skipped, not failed");
  c.Expect(!entry.message.empty(), "with a sentence saying why");
  c.Expect(entry.message.find("disc") != std::string::npos, "that names what it is");

  // `GET /content` on a multi-file rom serves a zip RomM builds on the fly, with
  // no Content-Length at all (docs/API_CONTRACT.md). The whole reason to refuse
  // is that a client which did not would write that archive to the card under
  // the rom's name.
  c.Expect(!rig.sandbox.Exists("/tico/roms/psx/Synthetic Two Disc Game"),
           "and no archive reaches the card under the rom's name");
  c.Expect(!rig.sandbox.Exists("/tico/roms/psx/Synthetic Two Disc Game.tmp.part"),
           "nor a partial one");
  c.Expect(!rig.sandbox.Exists("/tico/roms/psx/Synthetic Two Disc Game.tmp"),
           "nor a staged one");
  c.ExpectEq(rig.queue.pending(), std::size_t{0}, "the worker has nothing left to do");
}

/// The other half of M3-4's decision: the rom the skip must NOT fire on.
///
/// `Synthetic Nested Game` is a *directory* holding exactly one file, which RomM
/// reports as `has_nested_single_file: true` with `has_multiple_files: false`.
/// Everything that makes a disc set unsafe is absent -- the whole-rom endpoint
/// serves the file itself with a length, and the rom-level `sha1_hash` is the
/// digest of exactly those bytes (`harness.multifile` pins both against the live
/// server). So it downloads like any other rom, and this is what says so: a
/// check written against "is a directory" rather than `has_multiple_files` would
/// refuse a rom this client handles perfectly well, and refuse it silently.
void Nested(checks::Checks& c, http::HttpClient& client, const std::string& base,
            const harness::Fixture& fixture) {
  Rig rig(c, "download-nested", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id =
      Queued(c, client, base, fixture, &rig.queue, "Synthetic Nested Game", &rom);
  if (rom_id == 0) {
    return;
  }

  // The list item this decision is made from, straight off the live server.
  c.Expect(rom.has_nested_single_file, "GET /api/roms reports the fixture as a nested single file");
  c.Expect(!rom.has_multiple_files, "and not as a multi-file rom");

  // The door. The same index that refuses the two-disc rom with `kMultiFile`
  // accepts this one -- so the refusal is keyed on `has_multiple_files` and not
  // on anything the two roms share, which is everything else: both are psx, both
  // are directories on the server, and neither has a file extension.
  roms::FetchOptions fetch;
  fetch.base_url = base;
  fetch.bearer_token = fixture.token;
  const roms::FetchResult library = roms::FetchRomIndex(client, fetch);
  c.Expect(library.ok(), "the rom index fetches: " + library.message);
  std::int32_t position = 0;
  download::Queue guarded;
  c.Expect(download::EnqueueRom(guarded, library.index, rom_id, &position) == ipc::Error::kOk,
           "a nested single-file rom is accepted at the door, not refused as a disc set");
  c.ExpectEq(guarded.size(), std::size_t{1}, "and is queued");

  const download::DrainResult result = rig.Drain(client);
  c.Expect(result.outcome == download::DrainOutcome::kCompleted,
           std::string("the drain completed -- got ") + download::ToString(result.outcome) + " (" +
               result.message + ")");
  c.ExpectEq(result.downloaded, 1, "the rom came down");
  c.ExpectEq(result.skipped, 0, "and nothing was skipped");

  // `fs_name` is the *directory's* name, so that is what the rom is called on
  // the card -- the inner file's `.bin` is not part of it. Pinned rather than
  // corrected: v1 writes what RomM names the rom, and a rename would have to be
  // the same rename on every code path that later looks for the file.
  const std::string destination = "/tico/roms/psx/Synthetic Nested Game";
  const std::string expected =
      FixtureRom("roms/psx/Synthetic Nested Game/Synthetic Nested Game.bin");
  c.Expect(!expected.empty(), "the fixture's nested file is readable");
  c.Expect(rig.sandbox.Exists(destination), "the rom is at the destination the folder map names");
  c.Expect(rig.sandbox.Read(destination) == expected,
           "and its bytes are the one file inside the rom's directory, exactly");

  const QueueEntry entry = rig.Persisted(rom_id);
  c.Expect(entry.state == QueueState::kDone, "the entry is done, not skipped");
  c.Expect(entry.message.find("disc") == std::string::npos,
           "with no disc-set refusal recorded against it");
  c.ExpectEq(entry.destination, destination, "and records where it went");
  c.ExpectEq(entry.fs_name, std::string("Synthetic Nested Game"),
             "under the name RomM gave the rom, which is the directory's");
  c.ExpectEq(entry.platform_fs_slug, std::string("psx"), "keyed on the fs slug");
  c.ExpectEq(entry.size_bytes, static_cast<std::int64_t>(expected.size()),
             "for the size the server declared -- the file's, not a directory's");

  // The hash M3-3 checks is the rom's own, and for a nested single-file rom it
  // is the digest of the bytes that arrived. This is exactly what a disc set
  // does not have, and the reason the two are treated differently.
  c.Expect(!entry.sha1_hash.empty(), "the rom-level hash is recorded");
  c.ExpectEq(crypto::Sha1FileHex(rig.sandbox.Host(destination)), entry.sha1_hash,
             "and the file on the card hashes to it");

  c.Expect(!rig.sandbox.Exists(destination + ".tmp"), "nothing is left staged beside it");
  c.ExpectEq(rig.queue.pending(), std::size_t{0}, "and the worker has nothing left to do");
}

/// `missing_from_fs: true` -- RomM knows the rom and its file is gone from the
/// server's own filesystem.
///
/// A download would 404 a third of the way in, so the entry fails with a
/// sentence instead. The fixture library is healthy and nothing here may make it
/// otherwise, so the flag is forced the only way that does not touch it: the
/// proxy answers the *detail* call with a body of this test's own, and the real
/// worker reads it with the real parser. `count` is 1 and the path is the detail
/// endpoint, so the content request underneath is untouched -- and never made.
void Missing(checks::Checks& c, http::HttpClient& client, const std::string& base,
             const harness::Fixture& fixture) {
  Rig rig(c, "download-missing", base, fixture);
  harness::Rom rom;
  const std::int64_t rom_id = Queued(c, client, base, fixture, &rig.queue, "240pee.nes", &rom);
  if (rom_id == 0) {
    return;
  }

  const std::string gone =
      "{\"id\":" + std::to_string(rom_id) +
      ",\"fs_name\":\"240pee.nes\",\"platform_fs_slug\":\"nes\",\"fs_size_bytes\":65552,"
      "\"sha1_hash\":null,\"md5_hash\":null,\"has_multiple_files\":false,"
      "\"missing_from_fs\":true}";
  {
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"status\",\"status\":200,\"path\":\"/api/roms/" +
                             std::to_string(rom_id) + "\",\"body\":" + json::Quote(gone) + "}");
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("the drain finishes rather than retrying forever -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.ExpectEq(result.failed, 1, "the entry is written off as failed");
    c.ExpectEq(result.downloaded, 0, "and nothing came down");
  }

  const QueueEntry entry = rig.Persisted(rom_id);
  c.Expect(entry.state == QueueState::kFailed,
           "a rom the server cannot serve is failed, not skipped -- a skip is a decision this "
           "client made, and this is the server's");
  c.Expect(!entry.message.empty(), "with a sentence saying why");
  c.Expect(entry.message.find(rom.fs_name) == std::string::npos,
           "that does not quote the rom's name back into the queue file");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes"),
           "and nothing was written where a rom would have gone");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp.part"), "not even a partial one");
  c.Expect(!rig.sandbox.Exists("/tico/roms/nes/240pee.nes.tmp"), "nor a staged one");
  c.ExpectEq(rig.queue.pending(), std::size_t{0}, "the worker has nothing left to do");

  // A rom whose `fs_name` is longer than a path may be. The fields on a
  // `RomDetail` come off someone else's filesystem and become fields in a file
  // this client has to be able to write *again*, so an entry recorded and only
  // then found unwritable would make every later write of the queue fail the
  // same way -- for every other rom, until the console rebooted. That is the
  // failure this phase exists for, and the last two assertions are the ones
  // that see it.
  harness::Rom second;
  const std::int64_t long_id = Queued(c, client, base, fixture, &rig.queue, "nova.nes", &second);
  if (long_id == 0) {
    return;
  }
  const std::string enormous =
      "{\"id\":" + std::to_string(long_id) + ",\"fs_name\":\"" + std::string(900, 'n') +
      "\",\"platform_fs_slug\":\"nes\",\"fs_size_bytes\":1,\"sha1_hash\":null,"
      "\"md5_hash\":null,\"has_multiple_files\":false,\"missing_from_fs\":false}";
  {
    harness::Fault fault(c, client, base,
                         "{\"mode\":\"status\",\"status\":200,\"path\":\"/api/roms/" +
                             std::to_string(long_id) + "\",\"body\":" + json::Quote(enormous) +
                             "}");
    const download::DrainResult result = rig.Drain(client);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted,
             std::string("a rom the queue cannot record does not stop the drain -- got ") +
                 download::ToString(result.outcome) + " (" + result.message + ")");
    c.ExpectEq(result.skipped, 1, "it is skipped");
  }
  const QueueEntry refused = rig.Persisted(long_id);
  c.Expect(refused.state == QueueState::kSkipped, "with a skip on the card");
  c.Expect(refused.fs_name.empty(),
           "and the name that could not be recorded was not recorded -- which is the whole point");
  c.Expect(!refused.message.empty(), "though it says why");

  // The queue is still writable, which it would not be if the over-long name
  // had reached it: `SaveQueue` refuses a file the reader would discard, so one
  // bad row makes every later write fail.
  harness::Rom healthy;
  const std::int64_t healthy_id =
      Queued(c, client, base, fixture, &rig.queue, "gb240p.gb", &healthy);
  const download::DrainResult after = rig.Drain(client);
  c.Expect(after.outcome == download::DrainOutcome::kCompleted,
           std::string("and the next rom still drains -- got ") +
               download::ToString(after.outcome) + " (" + after.message + ")");
  c.Expect(rig.Persisted(healthy_id).state == QueueState::kDone, "all the way to done");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "roundtrip";
  const std::string base = rig::BaseUrl();
  checks::Checks checks;

  // The scenarios that need no server. They must keep passing with docker
  // stopped: what they pin is what a yanked card leaves behind.
  bool offline = true;
  if (scenario == "roundtrip") {
    Roundtrip(checks);
  } else if (scenario == "queue") {
    QueueApi(checks);
  } else if (scenario == "corrupt") {
    Corrupt(checks);
  } else if (scenario == "store") {
    Store(checks);
  } else if (scenario == "bounds") {
    Bounds(checks);
  } else if (scenario == "endpoint") {
    Endpoint(checks);
  } else {
    offline = false;
  }
  if (offline) {
    if (checks.failures() == 0) {
      std::cout << "download." << scenario << " ok\n";
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

  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }

  if (scenario == "drain") {
    Drain(checks, *client, base, fixture);
  } else if (scenario == "retries") {
    Retries(checks, *client, base, fixture);
  } else if (scenario == "resume") {
    Resume(checks, *client, base, fixture);
  } else if (scenario == "verify") {
    Verify(checks, *client, base, fixture);
  } else if (scenario == "fallback") {
    Fallback(checks, *client, base, fixture);
  } else if (scenario == "truncate") {
    Truncate(checks, *client, base, fixture);
  } else if (scenario == "stall") {
    Stall(checks, *client, base, fixture);
  } else if (scenario == "staging") {
    Staging(checks, *client, base, fixture);
  } else if (scenario == "disabled") {
    Disabled(checks, *client, base, fixture);
  } else if (scenario == "multifile") {
    Multifile(checks, *client, base, fixture);
  } else if (scenario == "nested") {
    Nested(checks, *client, base, fixture);
  } else if (scenario == "missing") {
    Missing(checks, *client, base, fixture);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  // Every rig scenario arms the proxy through `harness::Fault`, which disarms on
  // the way out -- including out of an early return. This is the belt to that
  // brace: a scenario that armed one another way leaves the next test's first
  // request damaged, and the red run then names a file nobody touched.
  harness::ExpectDisarmed(checks, *client, base, "download." + scenario +
                          " left the fault proxy disarmed");

  if (checks.failures() == 0) {
    std::cout << "download." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

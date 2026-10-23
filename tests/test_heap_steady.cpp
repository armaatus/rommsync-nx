// What the sysmodule holds between ticks, measured, and held flat (M9-15, #204).
//
// The process is meant to run for weeks without restarting, on a 1.5 MiB heap
// where `bad_alloc` is `std::terminate` (`-fno-exceptions`, switch.mk). The
// heap's table in sysmodule/source/main.cpp is a bound added up from constants;
// nothing measured what a tick actually allocates, and nothing would have
// noticed the engine keeping a little more after every one. This does both,
// with every `operator new` in the process counted (`count_new.hpp`):
//
//   alloc.counts   the counter itself: every signature family is seen, a bare
//                  `delete new int` is not elided away, and outstanding bytes
//                  come back to where they were.
//   alloc.judge    the steady-state rule, on series whose answer is known: a
//                  flat one passes, one that keeps a block a tick fails.
//   alloc.log_ring the log's in-memory tail, full and written through: it holds
//                  the same blocks however many lines pass, which a `deque`
//                  under it did not (found by `soak`, see `LogRing`).
//   alloc.page_parse
//                  a page of roms or platforms is read a row at a time: its
//                  peak grows with the rows it becomes, not with their parsed
//                  documents, which weigh ~11x their text (M9-19, see
//                  `PageParse`).
//   alloc.response_parse
//                  the same for the negotiate plan, the state listing, the device
//                  list and queue.json (M9-20, see `ResponseParse`).
//   alloc.wrong_shape
//                  a body of the wrong shape is refused before it is parsed, not
//                  after (M9-20, see `WrongShape`).
//   alloc.queue_write
//                  writing the download queue, rolling a write back, and listing
//                  it cost a few entries however full it is, not a copy of it
//                  (M9-21, see `QueueWrite`).
//   alloc.steady   the real `SdEngine` -- its worker, its scheduler, the fixture
//                  RomM -- run tick after tick through the IPC table, with what
//                  is outstanding sampled between ticks. After a warm-up that
//                  fills the bounded buffers (the log tail above all), it must
//                  not grow with the number of ticks. Prints the peak a tick
//                  reaches over that floor, which is the measurement main.cpp's
//                  heap table cites.
//   soak           the same, for many rounds of a tick *and* a rom download,
//                  sized by its argument. `ctest` runs a few rounds as
//                  `soak.smoke`, so the nightly is never the first time this
//                  code runs; .github/workflows/soak.yml runs hundreds.
//   schedule       the engine on a clock four thousand times fast, with a real
//                  thirty-minute interval and nobody asking for a tick: online,
//                  offline under a backoff, the wall clock jumped a year either
//                  way, and a token the server stops accepting -- each stretch
//                  as many hours of that clock as its argument (#250).
//                  `soak.schedule` runs six; the nightly runs two days.
//
// **Host numbers, not console numbers.** These are counted on the host's
// standard library, where a `std::string` or a node is not exactly the size
// newlib's libstdc++ makes it on aarch64, and they count C++ allocations only:
// libcurl's `malloc`s are the host transport's and the console's transport is a
// different one. What transfers to the console is the *shape* -- flat, or
// growing -- and the order of magnitude of a tick.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "count_new.hpp"
#include "engine.hpp"
#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/device_registration.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/log.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/server_capabilities.hpp"
#include "rommsync/state_sync.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_tick.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace ipc = rommsync::ipc;
namespace rlog = rommsync::log;
namespace sysmodule = rommsync::sysmodule;

namespace {

// --- the steady-state rule ----------------------------------------------------

/// What was outstanding at one quiet moment between two ticks.
struct Sample {
  std::int64_t blocks = 0;
  std::int64_t bytes = 0;
};

/// How far a quiet moment may wander without it being growth.
///
/// Not zero, because a flat process still moves a little: a log line that is a
/// digit longer than the one it pushed out of the ring, a timestamp string that
/// crosses a width. **Blocks are the strict measure** -- a line a digit longer
/// is the same one block -- and bytes the loose one.
struct Slack {
  std::int64_t blocks = 0;
  std::int64_t bytes = 0;
};

/// Flat, or growing with the number of ticks?
///
/// The series is split in two and the *least* of each half compared, which is
/// the question in its strict form: a process that keeps one block per tick
/// raises its floor every tick, and no amount of transient noise -- which only
/// ever adds to a sample -- can lower the second half's floor back down. Then
/// the whole series' spread is bounded too, so a process that grows in one
/// jump mid-run and then holds still is caught as well.
///
/// Empty string when steady; otherwise the reason, for the failure message.
std::string Judge(const std::vector<Sample>& samples, Slack slack) {
  if (samples.size() < 4) {
    return "only " + std::to_string(samples.size()) + " samples; a trend needs at least four";
  }
  const auto mid = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2);
  for (const auto& [field, allowed, unit] :
       {std::tuple{&Sample::blocks, slack.blocks, "blocks"},
        std::tuple{&Sample::bytes, slack.bytes, "bytes"}}) {
    const auto by_field = [field = field](const Sample& a, const Sample& b) {
      return a.*field < b.*field;
    };
    const auto least = [&](auto first, auto last) {
      return (*std::min_element(first, last, by_field)).*field;
    };
    const std::int64_t early = least(samples.begin(), mid);
    const std::int64_t late = least(mid, samples.end());
    if (late - early > allowed) {
      return std::string("the floor rose by ") + std::to_string(late - early) + " " + unit +
             " between the first and second half of the run (" + std::to_string(early) +
             " -> " + std::to_string(late) + "), past the " + std::to_string(allowed) +
             " allowed";
    }
    // The spread may be wider than the floor's rise: a quiet reading is the
    // least of five, and an unlucky one still catches the worker mid-loop.
    // Four slacks and a little over is a reading that caught something
    // transient, not one that found a different floor.
    const auto [low, high] = std::minmax_element(samples.begin(), samples.end(), by_field);
    if ((*high).*field - (*low).*field > 4 * allowed + 64) {
      return std::string("outstanding ") + unit + " spread " + std::to_string((*low).*field) +
             " .. " + std::to_string((*high).*field) + " across the run";
    }
  }
  return {};
}

// --- an engine, ticking -------------------------------------------------------

/// Counts the ticks the engine finishes, off the line `LogTickEnd` writes for
/// every one of them -- which is the only per-tick signal the engine gives that
/// does not depend on a second-granularity timestamp. Allocates nothing: a sink
/// that did would be measuring itself.
class TickCounter final : public rlog::Sink {
 public:
  void Write(rlog::Level, std::string_view line) override {
    if (line.find("sync.tick") != std::string_view::npos) {
      ticks_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  std::int64_t ticks() const { return ticks_.load(std::memory_order_relaxed); }

 private:
  std::atomic<std::int64_t> ticks_{0};
};

/// The sysmodule's engine on a sandboxed card, paired with the fixture RomM,
/// with its own worker running and its tick parked until asked for.
///
/// `interval_min = 0` with `on_boot = false` is `engine.syncnow`'s shape: no
/// timer at all, so every tick is one this test pressed for. That is the clock
/// accelerated in the only way that keeps a tick count exact -- the half hour
/// between two ticks on a console is spent here not waiting.
class Console {
 public:
  Console(checks::Checks& c, std::string_view name, const std::string& base,
          const harness::Fixture& fixture, http::HttpClient& client,
          std::string_view sync = "enabled = true\ninterval_min = 0\non_boot = false\n",
          const sysmodule::Clock* clock = nullptr)
      : sandbox(c, name), directory(sandbox.Host(harness::kConfigDir) + "/") {
    for (const char* folder : {"/tico/roms/nes", "/tico/roms/gba"}) {
      sandbox.MakeDirs(folder);
    }
    engine.UseClock(clock);
    ok = sandbox.Write("/config/rommsync/config.ini",
                       "[server]\nurl = " + base + "\n\n[sync]\n" + std::string(sync));
    auth::StoredToken token;
    token.server_url = base;
    token.access_token = fixture.token;
    token.device_id = fixture.device_id;
    ok = ok && auth::SaveToken(directory + auth::kTokenFileName, token).ok();
    card = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    engine.UseCard(card.get());
    engine.UseServer(&client, std::string());
    engine.Load(directory);
    core = std::make_unique<ipc::ServiceCore>(engine);
  }

  ipc::Error Call(ipc::Command command, const std::string& request, std::string* response) {
    return ipc::Dispatch(*core, static_cast<std::uint32_t>(command), request, response);
  }

  ipc::Status Status() {
    std::string response;
    Call(ipc::Command::kGetStatus, ipc::EncodeEmpty(), &response);
    return ipc::DecodeStatus(response).value;
  }

  /// Press "Sync now" and wait for the tick it starts to finish.
  bool Tick(const TickCounter& counter, std::chrono::seconds budget) {
    const std::int64_t before = counter.ticks();
    std::string response;
    Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response);
    if (ipc::DecodeSyncOutcome(response).value != ipc::SyncOutcome::kAccepted) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
      if (counter.ticks() > before && !Status().sync_in_progress) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
  }

  /// Queue `rom_id`, wait for the worker to drain it onto the card, and for the
  /// tick a finished download asks for to bring its save (#241) -- so a round
  /// ends with the worker idle, as the next round's press needs it.
  bool Download(std::int64_t rom_id, const TickCounter& counter, std::chrono::seconds budget) {
    const std::int64_t before = counter.ticks();
    std::string response;
    if (Call(ipc::Command::kEnqueue, ipc::EncodeRomId(rom_id), &response) != ipc::Error::kOk) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    bool drained = false;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!drained && Status().queue_depth == 0) {
        // The finished row stays for the queue screen; taking it out is what a
        // user tidying the screen does, and it is what lets the next round
        // queue the same rom again.
        if (Call(ipc::Command::kDequeue, ipc::EncodeRomId(rom_id), &response) !=
            ipc::Error::kOk) {
          return false;
        }
        drained = true;
      }
      if (drained && counter.ticks() > before && !Status().sync_in_progress) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return false;
  }

  harness::Sandbox sandbox;
  std::string directory;
  std::unique_ptr<fs::FileSystem> card;
  sysmodule::SdEngine engine;
  std::unique_ptr<ipc::ServiceCore> core;
  bool ok = false;
};

/// What is outstanding while nothing is happening: the least of a few readings
/// a moment apart. The worker loops once more after a tick ends, and the IPC
/// round trip that asked whether it had is itself an allocation; neither is
/// held, and a minimum is what lets them pass through without being counted.
Sample Quiet() {
  Sample least{INT64_MAX, INT64_MAX};
  for (int reading = 0; reading < 5; ++reading) {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const count_new::Snapshot now = count_new::Now();
    least.blocks = std::min(least.blocks, now.outstanding);
    least.bytes = std::min(least.bytes, now.outstanding_bytes);
  }
  return least;
}

/// Rounds enough to fill every bounded buffer the engine keeps before the floor
/// is worth reading: the log ring is `log::kTailLines` lines and a round writes
/// at least one, so a smaller warm-up would be measuring the ring filling.
///
/// Warmed up with the *same* round that is then measured. The soak's first run
/// warmed up on bare ticks and then measured ticks with downloads, and its floor
/// fell 48 bytes a round for eight rounds: the download's shorter lines pushing
/// the warm-up's longer ones out of the ring. That is not a leak, but a judge
/// cannot tell a slope from a slope.
constexpr int kWarmupRounds = static_cast<int>(rlog::kTailLines) + 8;

/// The slack the judge allows. Measured against the fixture RomM, between two
/// quiet ticks the floor does not move at all -- not a block, not a byte. What
/// it leaves room for is a log line one digit longer than the one it pushed out
/// of the ring, which is a byte a line and `log::kTailLines` lines.
constexpr Slack kSlack{2, 64};

struct Run {
  std::vector<Sample> samples;
  std::int64_t peak_over_floor = 0;
  std::int64_t floor_bytes = 0;
  std::int64_t allocations_per_round = 0;
};

/// Warm up, then `rounds` rounds of `round`, sampled after each one.
bool Drive(checks::Checks& c, int rounds, const std::function<bool(int)>& round, Run* run) {
  for (int at = 0; at < kWarmupRounds; ++at) {
    if (!round(at)) {
      c.Expect(false, "warm-up round " + std::to_string(at) + " finished");
      return false;
    }
  }
  // Reserved before the floor is read: the samples are allocations too, and a
  // vector doubling as it fills is exactly the shape of growth this hunts for.
  // The first run of this scenario found that, and it was this line missing.
  run->samples.reserve(static_cast<std::size_t>(rounds));
  const Sample floor = Quiet();
  run->floor_bytes = floor.bytes;
  const count_new::Window window;
  for (int at = 0; at < rounds; ++at) {
    count_new::ResetPeak();
    if (!round(at)) {
      c.Expect(false, "round " + std::to_string(at) + " finished");
      return false;
    }
    run->peak_over_floor =
        std::max(run->peak_over_floor, count_new::Now().peak_bytes - floor.bytes);
    run->samples.push_back(Quiet());
  }
  run->allocations_per_round = window.allocations() / std::max(rounds, 1);
  return true;
}

void Report(const std::string& what, const Run& run, const char* path) {
  std::cout << what << ": floor " << run.floor_bytes << " bytes, a round peaks "
            << run.peak_over_floor << " bytes over it, ~" << run.allocations_per_round
            << " allocations a round\n";
  for (std::size_t at = 0; at < run.samples.size(); ++at) {
    std::cout << "  round " << at << ": " << run.samples[at].blocks << " blocks, "
              << run.samples[at].bytes << " bytes\n";
  }
  if (path != nullptr && *path != '\0') {
    std::ofstream out(path);
    out << "round,blocks,bytes\n";
    for (std::size_t at = 0; at < run.samples.size(); ++at) {
      out << at << "," << run.samples[at].blocks << "," << run.samples[at].bytes << "\n";
    }
  }
  // The headline numbers in benchmark-action's `customSmallerIsBetter` shape,
  // for the soak workflow to track from one night to the next (#250): a floor
  // that creeps up across months, a little each PR, never trips `Judge` in any
  // one run.
  if (const char* bench = std::getenv("ROMMSYNC_BENCH_JSON"); bench != nullptr && *bench != '\0') {
    std::ofstream out(bench);
    out << "[\n"
        << "  {\"name\": \"" << what << ": held between rounds\", \"unit\": \"bytes\", \"value\": "
        << run.floor_bytes << "},\n"
        << "  {\"name\": \"" << what << ": a round's peak over that\", \"unit\": \"bytes\", "
        << "\"value\": " << run.peak_over_floor << "},\n"
        << "  {\"name\": \"" << what << ": allocations a round\", \"unit\": \"allocations\", "
        << "\"value\": " << run.allocations_per_round << "}\n"
        << "]\n";
  }
}

// --- the scenarios ------------------------------------------------------------

/// The counter is what every other assertion here stands on, so it is checked
/// first, against allocations whose sizes this file chose.
void Counts(checks::Checks& c) {
  struct alignas(64) Wide {
    char bytes[64];
  };
  const count_new::Snapshot before = count_new::Now();
  {
    const count_new::Window window;
    // Elidable, which is the point: [expr.new]/14 allows this pair to vanish,
    // and the target is built so that it may not (tests/CMakeLists.txt).
    delete new int(7);
    c.Expect(window.allocations() >= 1,
             "a bare `delete new int` is counted -- the replacement is installed and the "
             "allocation was not elided");
  }
  {
    auto* scalar = new std::int64_t(1);
    auto* array = new char[1000];
    auto* wide = new Wide;
    auto* nothrow = new (std::nothrow) char[24];
    const count_new::Snapshot held = count_new::Now();
    c.ExpectEq(held.outstanding - before.outstanding, std::int64_t{4},
               "four blocks outstanding, one from each family");
    c.ExpectEq(held.outstanding_bytes - before.outstanding_bytes,
               std::int64_t{8 + 1000 + 64 + 24}, "and exactly the bytes asked for");
    c.Expect(reinterpret_cast<std::uintptr_t>(wide) % 64 == 0,
             "an over-aligned type comes back aligned");
    delete scalar;
    delete[] array;
    delete wide;
    delete[] nothrow;
  }
  const count_new::Snapshot after = count_new::Now();
  c.ExpectEq(after.outstanding, before.outstanding, "every block is given back");
  c.ExpectEq(after.outstanding_bytes, before.outstanding_bytes, "and every byte");

  count_new::ResetPeak();
  {
    const std::string transient(4096, 'x');
    c.Expect(!transient.empty(), "a transient 4 KiB string");
  }
  c.Expect(count_new::Now().peak_bytes - after.outstanding_bytes >= 4096,
           "is gone again, and the peak remembers it");
}

/// The rule, on series whose answer is known before it is asked.
void JudgeRule(checks::Checks& c) {
  // Sixteen samples, the length `alloc.steady` takes under ctest.
  std::vector<Sample> flat;
  std::vector<Sample> leaking;
  std::vector<Sample> doubling;
  std::vector<Sample> jumped;
  for (std::int64_t at = 0; at < 16; ++at) {
    // Noise only ever adds to a quiet reading, so it is added on odd rounds.
    const std::int64_t noise = (at % 2) * 200;
    // A digit gained by every line in the log ring, half way through.
    const std::int64_t digit = at < 8 ? 0 : 24;
    flat.push_back({1000 + (at % 3 == 0 ? 1 : 0), 50000 + digit + noise});
    // One 48-byte block kept per round: a string a tick, the smallest leak worth
    // finding.
    leaking.push_back({1000 + at, 50000 + 48 * at + noise});
    // One 16-byte element appended per round to a vector that is never cleared:
    // no new block, only a reallocation each time the capacity doubles. The
    // first run of `alloc.steady` was this exact series -- its own sample
    // vector, unreserved -- and it is the shape a block count cannot see.
    std::int64_t capacity = 0;
    while (capacity < at) {
      capacity = capacity == 0 ? 1 : capacity * 2;
    }
    doubling.push_back({1000, 50000 + 16 * capacity + noise});
    jumped.push_back({at < 10 ? 1000 : 1400, at < 10 ? 50000 : 90000});
  }
  c.Expect(Judge(flat, kSlack).empty(), "a flat series is steady: " + Judge(flat, kSlack));
  c.Expect(!Judge(leaking, kSlack).empty(), "a block kept every round is growth");
  c.Expect(!Judge(doubling, kSlack).empty(),
           "and so is a vector that only ever grows, though it never adds a block");
  c.Expect(!Judge(jumped, kSlack).empty(), "and so is a step that never comes back down");
  c.Expect(!Judge({{1, 1}, {1, 1}}, kSlack).empty(), "and two samples are no trend at all");
}

/// The log's ring, full, written to again and again: what it holds has to stay
/// exactly where it is.
///
/// Found by `soak` rather than designed: a hundred rounds of a tick and a
/// download held flat for sixteen, then gained a 4 KiB block and kept it, then
/// eight bytes a round for ten more. The block was the ring's `std::deque`
/// allocating its next chunk as the window of `log::kTailLines` lines crossed a
/// chunk boundary -- bounded, but a heap that churns a chunk every few dozen
/// lines on a console with 1.5 MiB of it, and a floor no judge can call steady.
///
/// Short lines first and then lines of every length, because the second half of
/// what the soak found was the slots' capacity creeping up as longer lines
/// happened to land in more of them: a ring that only reused capacity passes
/// with lines of one length and fails here.
void LogRing(checks::Checks& c) {
  rlog::Reset();
  for (std::size_t at = 0; at < 2 * rlog::kTailLines; ++at) {
    rlog::Info(rlog::Event::kSyncTick, "short");
  }
  const count_new::Snapshot full = count_new::Now();
  std::int64_t lowest = full.outstanding;
  std::int64_t highest = full.outstanding;
  std::int64_t moved = 0;
  // Ten times the ring, so the window crosses whatever boundary a container
  // under it has, several times over.
  for (std::size_t at = 0; at < 10 * rlog::kTailLines; ++at) {
    // 7 is prime to the ring's 24, so every length lands in every slot.
    rlog::Info(rlog::Event::kSyncTick, std::string(20 + (at % 7) * 60, 'x'));
    const count_new::Snapshot now = count_new::Now();
    lowest = std::min(lowest, now.outstanding);
    highest = std::max(highest, now.outstanding);
    moved = std::max(moved, std::abs(now.outstanding_bytes - full.outstanding_bytes));
  }
  c.ExpectEq(highest - lowest, std::int64_t{0},
             "a full log ring holds the same blocks however many more lines go through it");
  c.ExpectEq(moved, std::int64_t{0},
             "and the same bytes, whatever the length of the lines going through it");
  rlog::Reset();
}

// --- a page is read a row at a time (M9-19, #217) ------------------------------

/// One row the shape of RomM 5.2.0's list schema, as far as the parse is
/// concerned: the fields the three readers take, and seventy more that each of
/// them throws away, two of them nested. Synthetic rather than the capture,
/// because what is under test is how the peak scales with the *row count*, and
/// that needs a page of any length.
std::string Row(std::int64_t id) {
  const std::string name = "Synthetic Game " + std::to_string(id) + " (USA)";
  std::string row = "{\"id\":" + std::to_string(id) + ",\"fs_name_no_ext\":\"" + name +
                    "\",\"fs_name_no_tags\":\"" + name + "\",\"fs_name\":\"" + name +
                    ".nes\",\"name\":\"" + name +
                    "\",\"platform_fs_slug\":\"nes\",\"fs_slug\":\"nes\",\"display_name\":\"" +
                    name + "\",\"rom_count\":1,\"fs_size_bytes\":4096,"
                           "\"has_multiple_files\":false";
  for (int field = 0; field < 70; ++field) {
    row += ",\"discarded_metadata_field_" + std::to_string(field) + "\":null";
  }
  row += ",\"rom_user\":{\"is_main_sibling\":false,\"backlogged\":false,\"note_raw_markdown\":"
         "\"\",\"last_played\":null},\"files\":[{\"file_name\":\"" +
         name + ".nes\",\"file_size_bytes\":4096,\"crc_hash\":\"00000000\"}]}";
  return row;
}

/// `rows` of `Row`, joined, as a bare array or inside the `/api/roms` envelope.
std::string Page(std::size_t rows, bool envelope) {
  std::string items = "[";
  for (std::size_t at = 0; at < rows; ++at) {
    items += (at == 0 ? "" : ",") + Row(static_cast<std::int64_t>(at) + 1);
  }
  items += "]";
  if (!envelope) {
    return items;
  }
  return "{\"items\":" + items + ",\"total\":" + std::to_string(rows) +
         ",\"limit\":" + std::to_string(rows) + ",\"offset\":0}";
}

/// The bytes `parse` reaches over what was outstanding before it, the body
/// itself excluded: it is the caller's, and already allocated.
template <class Parse>
std::int64_t PeakOf(const std::string& body, const Parse& parse) {
  count_new::ResetPeak();
  const std::int64_t before = count_new::Now().outstanding_bytes;
  parse(body);
  return count_new::Now().peak_bytes - before;
}

/// The rows `PageParse` and `ResponseParse` hold a page to. Namespace scope
/// because `ResponseParse`'s `check` takes it as a default argument, where no
/// local may appear ([dcl.fct.default]) -- clang lets a constexpr one through,
/// GCC does not.
constexpr std::size_t kRows = 64;

/// A page of rows costs the rows it becomes and **one** row's document, not the
/// whole page's.
///
/// `json::Value` is ~100 bytes a node and a RomM row is ~100 nodes, so a parsed
/// row weighs about eleven times its text -- measured at 23 KiB a row on
/// aarch64's libstdc++ against the fixture RomM (sysmodule/source/main.cpp).
/// Holding a 64-row page as a document is ~1.5 MiB, the whole heap, which is a
/// `std::terminate` on a console the first time a library has that many roms.
/// So the peak of a 64-row page may exceed a 1-row page's by what the 63 extra
/// rows become -- a struct and a few short strings each -- and by nothing that
/// scales with their documents.
void PageParse(checks::Checks& c) {
  // Far more than the struct and strings one row becomes on either standard
  // library, and far less than one row's document (~8 KiB of nodes for `Row`).
  constexpr std::int64_t kPerRow = 1024;

  const std::string one_page = Page(1, true);
  const std::string full_page = Page(kRows, true);
  const auto index_page = [](const std::string& body) {
    rommsync::roms::Page page;
    return rommsync::roms::ParsePage(body, &page).ok() && !page.roms.empty();
  };
  c.Expect(index_page(one_page) && index_page(full_page), "the index pages parse");
  const std::int64_t index_one = PeakOf(one_page, index_page);
  const std::int64_t index_full = PeakOf(full_page, index_page);
  std::cout << "roms::ParsePage: " << index_one << " bytes at 1 row, " << index_full
            << " at " << kRows << " (" << full_page.size() << " bytes of text)\n";
  c.Expect(index_full - index_one < static_cast<std::int64_t>(kRows) * kPerRow,
           "roms::ParsePage holds one row's document at a time, not the page's");

  const auto list_page = [](const std::string& body) {
    rommsync::lists::RomPage page;
    return rommsync::lists::ParseRomPage(body, &page).ok() && !page.roms.empty();
  };
  c.Expect(list_page(one_page) && list_page(full_page), "the list pages parse");
  const std::int64_t list_one = PeakOf(one_page, list_page);
  const std::int64_t list_full = PeakOf(full_page, list_page);
  std::cout << "lists::ParseRomPage: " << list_one << " bytes at 1 row, " << list_full
            << " at " << kRows << "\n";
  c.Expect(list_full - list_one < static_cast<std::int64_t>(kRows) * kPerRow,
           "lists::ParseRomPage holds one row's document at a time, not the page's");

  const std::string one_platform = Page(1, false);
  const std::string all_platforms = Page(kRows, false);
  const auto platforms = [](const std::string& body) {
    std::vector<rommsync::lists::PlatformRow> rows;
    return rommsync::lists::ParsePlatforms(body, &rows).ok() && !rows.empty();
  };
  c.Expect(platforms(one_platform) && platforms(all_platforms), "the platform lists parse");
  const std::int64_t platforms_one = PeakOf(one_platform, platforms);
  const std::int64_t platforms_all = PeakOf(all_platforms, platforms);
  std::cout << "lists::ParsePlatforms: " << platforms_one << " bytes at 1 row, "
            << platforms_all << " at " << kRows << "\n";
  c.Expect(platforms_all - platforms_one < static_cast<std::int64_t>(kRows) * kPerRow,
           "lists::ParsePlatforms holds one row's document at a time, not the list's");
}

// --- every other response that grows with a library (M9-20, #246) -------------

/// `rows` copies of `row(i)`, joined into an array, inside `head`/`tail` when an
/// envelope holds it.
std::string ArrayOf(std::size_t rows, const std::function<std::string(std::size_t)>& row,
                    const std::string& head = "", const std::string& tail = "") {
  std::string body = head + "[";
  for (std::size_t at = 0; at < rows; ++at) {
    body += (at == 0 ? "" : ",") + row(at);
  }
  return body + "]" + tail;
}

/// A negotiate operation as RomM 5.4.0 sends one
/// (server/contract/captures/5.4.0/sync-negotiate-download.json), ids varied.
std::string OperationRow(std::size_t at) {
  return "{\"action\":\"download\",\"rom_id\":" + std::to_string(at + 1) +
         ",\"save_id\":" + std::to_string(at + 1) +
         ",\"file_name\":\"probe [2026-10-21_09-02-11].srm\",\"slot\":\"slot-" +
         std::to_string(at) +
         "\",\"emulator\":\"probe-emulator\",\"reason\":\"Save exists on server but not on "
         "client\",\"server_updated_at\":\"2026-10-21T09:02:11+00:00\","
         "\"server_content_hash\":\"abd8fff93894e8112c7dd17386e54a5f\"}";
}

/// A `StateSchema` row (captures/5.4.0/states-list.json).
std::string StateRow(std::size_t at) {
  return "{\"id\":" + std::to_string(at + 1) + ",\"rom_id\":" + std::to_string(at + 1) +
         ",\"user_id\":1,\"file_name\":\"probe-873af96c.state\",\"file_name_no_tags\":"
         "\"probe-873af96c\",\"file_name_no_ext\":\"probe-873af96c\",\"file_extension\":"
         "\"state\",\"file_path\":\"users/557365723a31/states/nes/4/probe-other-emulator\","
         "\"file_size_bytes\":32,\"full_path\":\"users/557365723a31/states/nes/4/"
         "probe-other-emulator/probe-873af96c.state\",\"download_path\":\"/api/states/1/"
         "content\",\"missing_from_fs\":false,\"created_at\":\"2026-09-06T15:10:41+00:00\","
         "\"updated_at\":\"2026-09-06T15:10:41+00:00\",\"emulator\":\"probe-other-emulator\","
         "\"is_public\":false,\"screenshot\":null}";
}

/// A `DeviceSchema` row (captures/5.4.0/devices-get.json).
std::string DeviceRow(std::size_t at) {
  return "{\"id\":\"b4ee319c-aea5-43ff-a0b3-" + std::to_string(100000000000 + at) +
         "\",\"user_id\":1,\"name\":\"probe_contract.py\",\"platform\":\"switch\","
         "\"client\":\"rommsync-nx-probe\",\"client_version\":\"0.0.0\",\"ip_address\":null,"
         "\"mac_address\":null,\"hostname\":null,\"client_device_identifier\":\"device-" +
         std::to_string(at) +
         "\",\"sync_mode\":\"api\",\"sync_enabled\":true,\"sync_config\":null,\"last_seen\":"
         "\"2026-09-06T15:10:39+00:00\",\"created_at\":\"2026-09-06T15:10:27+00:00\","
         "\"updated_at\":\"2026-09-06T15:10:39+00:00\"}";
}

/// The other responses whose size comes from an array are read an element at a
/// time too: the negotiate plan, the state listing, the device list, and
/// `queue.json` at boot. `PageParse`'s rule for each -- the peak may grow by
/// what the rows become, and by nothing that scales with their documents.
///
/// Held whole, each was ~10x its text on aarch64: a 512-operation plan peaked at
/// 1.3 MiB and a 117 KiB state listing at 980 KiB, on a 1.5 MiB heap.
void ResponseParse(checks::Checks& c) {
  // Over what one row becomes on either standard library -- an operation is
  // the largest, ~450 bytes on aarch64 -- and under one row's document, which is
  // ~3 KiB for the smallest of the four.
  constexpr std::int64_t kPerRow = 1024;
  const auto check = [&c](const char* what, const std::string& one, const std::string& many,
                          const std::function<bool(const std::string&)>& parse,
                          std::size_t rows = kRows) {
    c.Expect(parse(one) && parse(many), std::string(what) + " parses");
    const std::int64_t at_one = PeakOf(one, parse);
    const std::int64_t at_many = PeakOf(many, parse);
    std::cout << what << ": " << at_one << " bytes at 1 row, " << at_many << " at " << rows
              << " (" << many.size() << " bytes of text)\n";
    c.Expect(at_many - at_one < static_cast<std::int64_t>(rows) * kPerRow,
             std::string(what) + " holds one row's document at a time, not the whole body's");
  };

  const auto plan = [](std::size_t rows) {
    return ArrayOf(rows, OperationRow, "{\"session_id\":4,\"operations\":",
                   ",\"total_upload\":0,\"total_download\":" + std::to_string(rows) +
                       ",\"total_conflict\":0,\"total_no_op\":0,\"total_delete\":0}");
  };
  check("sync::ParseNegotiateResponse", plan(1), plan(kRows), [](const std::string& body) {
    return rommsync::sync::ParseNegotiateResponse(body).ok();
  });

  check("sync::ParseStateList", ArrayOf(1, StateRow), ArrayOf(kRows, StateRow),
        [](const std::string& body) { return rommsync::sync::ParseStateList(body).ok(); });

  check("auth::ParseDeviceList", ArrayOf(1, DeviceRow), ArrayOf(kRows, DeviceRow),
        [](const std::string& body) { return auth::ParseDeviceList(body).ok(); });

  // The console's own search holds the one row it finds, however many devices
  // the list has (#248).
  {
    const auto find = [](const std::string& body) {
      return auth::FindInDeviceList(body, "device-0").value.matches == 1;
    };
    const std::string one = ArrayOf(1, DeviceRow);
    const std::string many = ArrayOf(kRows, DeviceRow);
    c.Expect(find(one) && find(many), "auth::FindInDeviceList finds the row");
    const std::int64_t at_one = PeakOf(one, find);
    const std::int64_t at_many = PeakOf(many, find);
    std::cout << "auth::FindInDeviceList: " << at_one << " bytes at 1 row, " << at_many << " at "
              << kRows << "\n";
    c.Expect(at_many - at_one < 1024,
             "auth::FindInDeviceList holds one device, not the list: " +
                 std::to_string(at_many - at_one));
  }

  const auto queue = [](std::size_t rows) {
    std::vector<rommsync::download::QueueEntry> entries;
    for (std::size_t at = 0; at < rows; ++at) {
      rommsync::download::QueueEntry entry;
      entry.rom_id = static_cast<std::int64_t>(at) + 1;
      entry.platform_fs_slug = "gba";
      entry.fs_name = "Synthetic Game " + std::to_string(at) + " (USA).gba";
      entry.destination = "/tico/roms/gba/" + entry.fs_name;
      entry.queued_at = 1704067200;
      entries.push_back(std::move(entry));
    }
    return rommsync::download::SerializeQueue(entries);
  };
  // A queue holds no more than `kMaxQueueEntries` -- fewer than the other three
  // carry since #247 -- and a file with more is refused whole.
  constexpr std::size_t kQueueRows = rommsync::download::kMaxQueueEntries;
  check(
      "download::ParseQueue", queue(1), queue(kQueueRows),
      [](const std::string& text) {
        const rommsync::download::LoadedQueue loaded = rommsync::download::ParseQueue(text);
        return !loaded.discarded && !loaded.entries.empty();
      },
      kQueueRows);
}

/// A body of the wrong shape is refused before it is parsed, not after.
///
/// A bare array where the `/api/roms` envelope belongs never reached the member
/// `json::ParseEach` streams, so the whole of it was parsed -- a 64-row page at
/// ~11x its text -- before the envelope check refused it. The refusal was right;
/// the heap it took on the way had no row (M9-20, #246).
void WrongShape(checks::Checks& c) {
  const std::string bare = Page(64, false);
  const auto index_page = [](const std::string& body) {
    rommsync::roms::Page page;
    return rommsync::roms::ParsePage(body, &page).ok();
  };
  const auto list_page = [](const std::string& body) {
    rommsync::lists::RomPage page;
    return rommsync::lists::ParseRomPage(body, &page).ok();
  };
  const auto platforms = [](const std::string& body) {
    std::vector<rommsync::lists::PlatformRow> rows;
    return rommsync::lists::ParsePlatforms(body, &rows).ok();
  };
  const std::string enveloped = Page(64, true);
  // The refusal's own error is a few short strings; a document is hundreds of
  // KiB. 4 KiB is far from both.
  constexpr std::int64_t kRefusal = 4 * 1024;
  struct Case {
    const char* what;
    const std::string* body;
    std::function<bool(const std::string&)> parse;
  };
  const Case cases[] = {
      {"roms::ParsePage over a bare array", &bare, index_page},
      {"lists::ParseRomPage over a bare array", &bare, list_page},
      {"lists::ParsePlatforms over an envelope", &enveloped, platforms},
  };
  for (const Case& refused : cases) {
    c.Expect(!refused.parse(*refused.body), std::string(refused.what) + " is refused");
    const std::int64_t peak = PeakOf(*refused.body, refused.parse);
    std::cout << refused.what << ": refused at a peak of " << peak << " bytes ("
              << refused.body->size() << " bytes of text)\n";
    c.Expect(peak < kRefusal, std::string(refused.what) + " is refused before it is parsed");
  }
}

/// One rig run: a console paired with the fixture, its worker started, the
/// log's tick counter installed, `round` driven `rounds` times after the
/// warm-up, and what it held judged. `what` names the run in the output and in
/// the failure message.
void Measure(checks::Checks& c, http::HttpClient& client, const std::string& base,
             const harness::Fixture& fixture, int rounds, const char* report,
             const std::string& what,
             const std::function<std::function<bool(int)>(Console&, const TickCounter&)>& make) {
  TickCounter counter;
  rlog::Sink* const previous = rlog::GetSink();
  rlog::SetSink(&counter);
  {
    Console console(c, what, base, fixture, client);
    c.Expect(console.ok, "a console configured for, and paired with, the fixture RomM");
    console.engine.StartWorker();
    Run run;
    if (Drive(c, rounds, make(console, counter), &run)) {
      Report(what, run, report);
      const std::string verdict = Judge(run.samples, kSlack);
      c.Expect(verdict.empty(), what + ": what the engine holds between rounds does not grow "
                                       "with the number of rounds: " + verdict);
      // llvm's `RequireAllocationGuard`: a round that allocated next to nothing
      // did not run, and a floor that never moved because nothing ran is the
      // wrong reason to pass.
      c.Expect(run.allocations_per_round >= 100,
               what + ": and the rounds measured really ran: " +
                   std::to_string(run.allocations_per_round) + " allocations a round");
    }
  }
  rlog::SetSink(previous);
}

/// Ticks, and nothing else: the steady state of a console with nothing to do.
void Steady(checks::Checks& c, http::HttpClient& client, const std::string& base,
            const harness::Fixture& fixture, int rounds, const char* report) {
  Measure(c, client, base, fixture, rounds, report, "alloc.steady",
          [](Console& console, const TickCounter& counter) {
            return [&console, &counter](int) {
              return console.Tick(counter, std::chrono::seconds{60});
            };
          });
}

/// A tick and a rom download a round, the rom taken off the card after each so
/// the next round downloads it again.
void Soak(checks::Checks& c, http::HttpClient& client, const std::string& base,
          const harness::Fixture& fixture, int rounds, const char* report) {
  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "240pee.nes", &rom)) {
    c.Expect(false, "the seeded library holds 240pee.nes -- re-seed and rescan: "
                    "./server/testing/seed.sh && ./.venv/bin/python server/testing/provision.py");
    return;
  }
  Measure(c, client, base, fixture, rounds, report, "soak",
          [&c, id = rom.id](Console& console, const TickCounter& counter) {
            return [&c, &console, &counter, id](int) {
              if (!console.Tick(counter, std::chrono::seconds{60}) ||
                  !console.Download(id, counter, std::chrono::seconds{120})) {
                return false;
              }
              std::error_code error;
              const bool arrived = std::filesystem::remove(
                  console.sandbox.Host("/tico/roms/nes/240pee.nes"), error);
              c.Expect(arrived, "the rom arrived on the card, and is taken away for the next "
                                "round");
              return arrived;
            };
          });
}

// --- the schedule's own clock, run fast (#250) ----------------------------------

/// `steady_clock` and `system_clock`, both running `scale` times fast from the
/// moment this is made, with the wall clock movable on its own: the divergence
/// a console's clock being set -- or reset -- produces, and the one libfaketime's
/// `FAKETIME_DONT_FAKE_MONOTONIC` exists to make.
class FastClock final : public sysmodule::Clock {
 public:
  explicit FastClock(std::int64_t scale) : scale_(scale) {}

  std::chrono::steady_clock::time_point Steady() const override {
    return origin_steady_ + Elapsed();
  }
  std::chrono::system_clock::time_point Wall() const override {
    return origin_wall_ +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(
               Elapsed() + std::chrono::milliseconds{wall_offset_ms_.load(std::memory_order_relaxed)});
  }
  std::chrono::milliseconds RealFor(std::chrono::milliseconds span) const override {
    if (span <= std::chrono::milliseconds::zero()) {
      return span;
    }
    // At least a millisecond, so a short backoff is still a wait and not a spin.
    return std::max(std::chrono::milliseconds{1}, span / scale_);
  }

  /// Move the wall clock and only the wall clock.
  void JumpWall(std::chrono::milliseconds by) {
    wall_offset_ms_.fetch_add(by.count(), std::memory_order_relaxed);
  }

  /// Real time for `span` of this clock to pass.
  std::chrono::milliseconds Real(std::chrono::milliseconds span) const { return span / scale_; }

  std::int64_t scale() const { return scale_; }

 private:
  std::chrono::steady_clock::duration Elapsed() const {
    return (std::chrono::steady_clock::now() - origin_real_) * scale_;
  }

  std::int64_t scale_;
  std::chrono::steady_clock::time_point origin_real_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point origin_steady_ = origin_real_;
  std::chrono::system_clock::time_point origin_wall_ = std::chrono::system_clock::now();
  std::atomic<std::int64_t> wall_offset_ms_{0};
};

/// The lines a run on the fast clock is judged by, counted as they are written.
/// Allocates nothing, for `TickCounter`'s reason.
class ScheduleWatch final : public rlog::Sink {
 public:
  void Write(rlog::Level, std::string_view line) override {
    if (line.find("sync.tick") != std::string_view::npos) {
      ticks.fetch_add(1, std::memory_order_relaxed);
      if (line.find("outcome=completed") != std::string_view::npos) {
        completed.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (line.find("auth.rejected") != std::string_view::npos) {
      rejected.fetch_add(1, std::memory_order_relaxed);
    }
  }

  std::atomic<std::int64_t> ticks{0};
  std::atomic<std::int64_t> completed{0};
  std::atomic<std::int64_t> rejected{0};
};

/// The real client, counting what reaches it.
class CountingClient final : public http::HttpClient {
 public:
  explicit CountingClient(http::HttpClient& inner) : inner_(inner) {}
  http::Result Send(const http::Request& request) override {
    requests.fetch_add(1, std::memory_order_relaxed);
    return inner_.Send(request);
  }
  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    requests.fetch_add(1, std::memory_order_relaxed);
    return inner_.Download(request, target);
  }
  std::atomic<std::int64_t> requests{0};

 private:
  http::HttpClient& inner_;
};

/// One stretch of simulated time, and what happened in it.
struct Stretch {
  std::int64_t ticks = 0;
  std::int64_t completed = 0;
  std::int64_t rejected = 0;
  std::int64_t requests = 0;
};

/// The schedule running on its own clock, a thousand-odd times fast, for days
/// of it (#204's "not done", #250): what the console does between two presses
/// of nothing at all.
///
/// The engine is configured as a console is -- a thirty-minute interval and a
/// boot tick -- and nothing asks it for a tick: every tick here is one the
/// scheduler decided was due. Each stretch is `hours` of the fast clock:
///
///   online     about two ticks an hour, every one completed;
///   offline    the proxy resets every connection. The scheduler backs off from
///              thirty seconds to its thirty-minute cap and stays there: it
///              keeps trying, and it does not storm. Then the network comes
///              back and the next tick completes within one capped backoff;
///   wall +1y   the console's clock jumps a year ahead and monotonic does not:
///              the interval holds -- no catching up a year of ticks;
///   wall -2y   and two years back, to before the last tick: no storm, and no
///              stall waiting for a wall clock to catch up -- monotonic drives;
///   expired    the server starts answering 401 to every request, as it does to
///              a token that has expired or been revoked. The gate blocks within
///              its three rejections and from then on nothing reaches the
///              server at all, while the log still says why once an interval.
void Schedule(checks::Checks& c, http::HttpClient& client, const std::string& base,
              const harness::Fixture& fixture, std::int64_t hours) {
  // ~4000x: a thirty-minute interval is 450 ms, a day 21.6 s. A tick against the
  // fixture is ~100 ms real, which is ~7 minutes of this clock -- well inside an
  // interval, so a tick's own length does not crowd the schedule it is judged on.
  FastClock clock(4000);
  ScheduleWatch watch;
  rlog::Sink* const previous = rlog::GetSink();
  rlog::SetSink(&watch);
  CountingClient counted(client);
  const auto snapshot = [&] {
    return Stretch{watch.ticks.load(), watch.completed.load(), watch.rejected.load(),
                   counted.requests.load()};
  };
  const auto stretch = [&](std::int64_t span_hours) {
    const Stretch start = snapshot();
    std::this_thread::sleep_for(clock.Real(std::chrono::hours{span_hours}));
    const Stretch end = snapshot();
    return Stretch{end.ticks - start.ticks, end.completed - start.completed,
                   end.rejected - start.rejected, end.requests - start.requests};
  };
  const auto say = [&](const char* what, const Stretch& s) {
    std::cout << "  " << what << " (" << hours << "h of the clock): " << s.ticks << " ticks, "
              << s.completed << " completed, " << s.rejected << " auth.rejected, " << s.requests
              << " requests\n";
  };
  // Two ticks an hour at the interval, at most. The interval runs from the end
  // of a tick, and a tick is minutes of this clock -- more on a slow runner -- so
  // fewer is the tick's own length, and half is the floor: a schedule that has
  // stalled makes none. Past a quarter over, plus a tick either side of a
  // stretch's edge, is a schedule that is not waiting out its interval.
  const std::int64_t expected = 2 * hours;
  const auto about = [&](std::int64_t ticks) {
    return ticks >= expected / 2 && ticks <= expected * 5 / 4 + 2;
  };

  {
    Console console(c, "schedule", base, fixture, counted,
                    "enabled = true\ninterval_min = 30\non_boot = true\n", &clock);
    c.Expect(console.ok, "a console configured for, and paired with, the fixture RomM");
    console.engine.StartWorker();

    const Stretch online = stretch(hours);
    say("online", online);
    c.Expect(about(online.ticks) && online.completed == online.ticks,
             "online, the schedule ticks twice an hour and every tick completes: " +
                 std::to_string(online.ticks) + " ticks, " + std::to_string(online.completed) +
                 " completed, for " + std::to_string(expected) + " intervals");

    Stretch offline;
    {
      harness::Fault down(c, client, base, R"({"mode":"drop","bytes":0,"count":1000000})");
      offline = stretch(hours);
    }
    say("offline", offline);
    // The backoff climbs 30 s, 1, 2, 4, 8, 16 minutes before it reaches the cap:
    // six attempts more than the interval would make, and then the interval's
    // rate. Past half as many again is a storm. The floor is lower than online's,
    // because an offline tick spends real time on a socket being reset and its
    // retries -- a few hundred milliseconds, which is most of an hour of this
    // clock -- so one every three hours is the line between backing off and
    // having given up.
    c.Expect(offline.completed == 0, "offline, no tick completes");
    c.Expect(offline.ticks >= hours / 3,
             "offline, the schedule keeps trying at its capped backoff: " +
                 std::to_string(offline.ticks) + " ticks for " + std::to_string(expected) +
                 " intervals");
    c.Expect(offline.ticks <= expected * 3 / 2 + 8,
             "...and does not storm a server that is not there: " +
                 std::to_string(offline.ticks) + " ticks for " + std::to_string(expected) +
                 " intervals");
    // Back: the next tick is at most one capped backoff away, and the backoff's
    // jitter can stretch that by a quarter.
    const Stretch back = stretch(1);
    say("back online", back);
    c.Expect(back.completed >= 1,
             "the network back, a tick completes within the hour of a capped backoff");

    clock.JumpWall(std::chrono::hours{24 * 365});
    const Stretch ahead = stretch(hours);
    say("wall +1y", ahead);
    c.Expect(about(ahead.ticks) && ahead.completed == ahead.ticks,
             "a wall clock a year ahead does not catch up a year of ticks: " +
                 std::to_string(ahead.ticks) + " ticks for " + std::to_string(expected) +
                 " intervals");

    clock.JumpWall(-std::chrono::hours{2 * 24 * 365});
    const Stretch behind = stretch(hours);
    say("wall -2y", behind);
    c.Expect(about(behind.ticks) && behind.completed == behind.ticks,
             "a wall clock set back two years neither storms nor stalls -- monotonic drives: " +
                 std::to_string(behind.ticks) + " ticks for " + std::to_string(expected) +
                 " intervals");

    Stretch expiring;
    Stretch expired;
    {
      harness::Fault revoked(c, client, base,
                             R"({"mode":"status","status":401,"count":1000000})");
      expiring = stretch(hours / 2);
      expired = stretch(hours - hours / 2);
    }
    say("token expiring", expiring);
    say("token expired", expired);
    c.Expect(expiring.completed == 0 && expired.completed == 0,
             "a token the server refuses completes nothing");
    c.Expect(expired.requests == 0,
             "once the gate has blocked, nothing reaches the server at all: " +
                 std::to_string(expired.requests) + " requests in the second half");
    c.Expect(expired.rejected >= (hours - hours / 2),
             "...and the log still says why, about once an interval: " +
                 std::to_string(expired.rejected) + " auth.rejected lines");
    c.Expect(console.Status().auth == ipc::AuthState::kUnauthenticated,
             "and the status screen says the pairing is gone");
  }
  rlog::SetSink(previous);
}

}  // namespace

/// M10-2 (#228): what reading the server's version costs on the heap, and that
/// it does not grow with the heartbeat.
///
/// The heartbeat is the server's whole configuration and widens with the
/// library -- every platform folder is in it -- and a whole-document parse
/// would hold ~11x its text (#217). `ParseHeartbeatVersion` walks the body and
/// parses only `SYSTEM`, so what it allocates is that member's document and the
/// version string: the same for the fixture's 1 KiB heartbeat as for one with
/// four thousand platforms in it. Measured here as a peak over the bytes
/// outstanding before the call; the body itself is the caller's, already held.
void HeartbeatCost(checks::Checks& c) {
  const std::string head = R"({"SYSTEM":{"VERSION":"5.4.0","GIT_BRANCH":null,)"
                           R"("SHOW_SETUP_WIZARD":false},"FILESYSTEM":{"FS_PLATFORMS":[)";
  const auto body_with = [&head](int platforms) {
    std::string body = head;
    for (int i = 0; i < platforms; ++i) {
      body += (i == 0 ? "\"" : ",\"") + std::string("platform-") + std::to_string(i) + "\"";
    }
    body += R"(]},"OIDC":{"ENABLED":false,"PROVIDER":""}})";
    return body;
  };
  const auto peak_of = [&c](const std::string& body) {
    count_new::ResetPeak();
    const std::int64_t before = count_new::Now().outstanding_bytes;
    const auto read = rommsync::server::ParseHeartbeatVersion(body);
    const std::int64_t peak = count_new::Now().peak_bytes - before;
    c.Expect(read.ok() && read.value == "5.4.0", "the heartbeat reads: " + read.error.Describe());
    return peak;
  };
  const std::string small = body_with(4);
  const std::string large = body_with(4000);
  const std::int64_t small_peak = peak_of(small);
  const std::int64_t large_peak = peak_of(large);
  std::cout << "heartbeat of " << small.size() << " bytes: parse peaks " << small_peak
            << " bytes over what was held\n"
            << "heartbeat of " << large.size() << " bytes: parse peaks " << large_peak
            << " bytes over what was held\n";
  c.Expect(small_peak > 0, "the parse allocates something, so the counter saw it");
  c.ExpectEq(large_peak, small_peak,
             "the cost does not move with the rest of the body -- only SYSTEM is parsed");
  // A whole-document parse of `large` would be hundreds of KiB; the bound is
  // the member's own, `kMaxSystemBytes`, and what RomM sends under it is three
  // fields. 4 KiB is an order of magnitude over what this measures on libc++.
  c.Expect(large_peak < 4 * 1024, "and it is a few hundred bytes, not a document: " +
                                      std::to_string(large_peak));
}

// --- the download queue, written and listed at its bound (M9-21, #247) --------

/// A string of exactly `width` bytes, ending in `tag` so no two are equal, built
/// at its own length: a `+` that grew one would measure libstdc++'s doubling,
/// which `queue.json`'s reader does not do.
std::string ExactText(std::size_t width, char fill, std::size_t tag) {
  std::string text(width, fill);
  const std::string digits = std::to_string(1000 + tag);
  text.replace(width - digits.size(), digits.size(), digits);
  return text;
}

/// A queue entry with every string at its own bound: `fs_name` and
/// `destination` at `config::kMaxPathLength`, `message` at `kMaxMessageChars`.
rommsync::download::QueueEntry WidestEntry(std::size_t at) {
  rommsync::download::QueueEntry entry;
  entry.rom_id = static_cast<std::int64_t>(at) + 1;
  entry.platform_fs_slug = "gba";
  entry.fs_name = ExactText(rommsync::config::kMaxPathLength, 'n', at);
  entry.destination = "/" + ExactText(rommsync::config::kMaxPathLength - 1, 'd', at);
  entry.sha1_hash = std::string(40, 'a');
  entry.message = ExactText(rommsync::download::kMaxMessageChars, 'm', at);
  entry.state = rommsync::download::QueueState::kDone;
  entry.queued_at = 1704067200;
  return entry;
}

/// Writing the queue costs a few entries, not the queue again.
///
/// Every change to the queue is written whole, from the IPC thread as well as
/// the worker, and until #247 each write held the queue three times over: the
/// queue, a copy of all of it to roll back to, and all of it serialised -- at
/// the entries' bounds a full queue is ~2 KiB an entry, so a write was a few
/// hundred KiB that no row of the heap table had. Listing the queue to the
/// overlay copied all of it too. Each must now cost what a handful of entries
/// cost, however full the queue is.
void QueueWrite(checks::Checks& c) {
  namespace download = rommsync::download;
  rommsync::host::InstallPosixFileSync();
  std::error_code error;
  const std::filesystem::path dir = std::filesystem::path(rig::ScratchDir()) / "alloc-queue";
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  const std::string path = (dir / download::kQueueFileName).string();

  std::vector<download::QueueEntry> entries;
  for (std::size_t at = 0; at < download::kMaxQueueEntries; ++at) {
    entries.push_back(WidestEntry(at));
  }
  // What one entry holds, so the bound below is in entries rather than bytes
  // either standard library would disagree about. A write holds the entry it
  // writes over, the piece of text it is writing and the quoted field that piece
  // is being built from -- each at most an entry, some of them grown by
  // doubling -- and nothing that grows with the queue: a write before #247 was
  // the queue three times, ~250 entries' worth at this one's bound.
  std::int64_t one = 0;
  {
    const std::int64_t before = count_new::Now().outstanding_bytes;
    const download::QueueEntry copy = entries.front();
    one = count_new::Now().outstanding_bytes - before + static_cast<std::int64_t>(sizeof(copy));
  }
  const std::int64_t kBound = 6 * one;
  c.Expect(download::SaveQueue(path, entries).ok(), "a full queue at its widest is writable");

  download::Queue queue;
  queue.Reset(std::move(entries));
  const auto store = [&path](const std::vector<download::QueueEntry>& rows) {
    return download::SaveQueue(path, rows).ok();
  };

  download::QueueEntry changed = queue.Find(1);
  changed.message = "moved on";
  count_new::ResetPeak();
  std::int64_t before = count_new::Now().outstanding_bytes;
  c.Expect(queue.UpdateAndStore(changed, store) == download::Queue::Committed::kOk,
           "an update is written");
  const std::int64_t update = count_new::Now().peak_bytes - before;

  download::QueueEntry removed;
  count_new::ResetPeak();
  before = count_new::Now().outstanding_bytes;
  c.Expect(queue.RemoveAndStore(2, &removed, store) == ipc::Error::kOk, "a removal is written");
  const std::int64_t remove = count_new::Now().peak_bytes - before;

  std::int32_t position = 0;
  count_new::ResetPeak();
  before = count_new::Now().outstanding_bytes;
  c.Expect(queue.EnqueueAndStore(5000, &position, store) == ipc::Error::kOk,
           "an enqueue into a full queue (a finished row makes way) is written");
  const std::int64_t enqueue = count_new::Now().peak_bytes - before;

  // A write that fails is rolled back exactly, at the same cost.
  download::QueueEntry refused = queue.Find(3);
  refused.message = "never written";
  count_new::ResetPeak();
  before = count_new::Now().outstanding_bytes;
  c.Expect(queue.UpdateAndStore(refused, [](const std::vector<download::QueueEntry>&) {
    return false;
  }) == download::Queue::Committed::kStoreFailed,
           "a refused write is reported");
  const std::int64_t rollback = count_new::Now().peak_bytes - before;
  c.Expect(queue.Find(3).message != "never written", "and undone");
  c.ExpectEq(queue.size(), download::kMaxQueueEntries, "and the queue is still full");

  std::cout << "one entry at its widest: " << one << " bytes; a write peaks at " << update
            << " (update), " << remove << " (remove), " << enqueue << " (enqueue), " << rollback
            << " (refused) over the " << download::kMaxQueueEntries << "-entry queue\n";
  c.Expect(update < kBound, "an update costs a few entries, not the queue: " +
                                std::to_string(update) + " >= " + std::to_string(kBound));
  c.Expect(remove < kBound, "a removal costs a few entries, not the queue: " +
                                std::to_string(remove) + " >= " + std::to_string(kBound));
  c.Expect(enqueue < kBound, "an enqueue costs a few entries, not the queue: " +
                                 std::to_string(enqueue) + " >= " + std::to_string(kBound));
  c.Expect(rollback < kBound, "a rollback costs a few entries, not the queue: " +
                                  std::to_string(rollback) + " >= " + std::to_string(kBound));

  // The overlay's queue screen, a page of it: the page is `kHeapListPages`'s,
  // and nothing beside it may be the whole queue. What is built beside the page
  // -- its items as they are measured against the payload, the item vector as
  // it grows -- scales with the page, which a payload bounds, not with the
  // queue; a copy of the queue was ~64 entries.
  const std::int64_t kPageBound = 16 * one;
  rommsync::config::Config configuration = rommsync::config::Defaults();
  rommsync::lists::Service service(rommsync::lists::Service::FixedConfig(configuration), queue);
  ipc::ListRequest request;
  request.kind = ipc::ListKind::kQueue;
  ipc::Cursor cursor = 0;
  c.Expect(service.ListBegin(request, &cursor) == ipc::Error::kOk, "the queue list opens");
  ipc::ListPage page;
  count_new::ResetPeak();
  before = count_new::Now().outstanding_bytes;
  c.Expect(service.ListNext(cursor, &page) == ipc::Error::kOk, "and serves a page");
  const std::int64_t listed = count_new::Now().peak_bytes - before;
  const std::int64_t kept = count_new::Now().outstanding_bytes - before;
  std::cout << "a queue page of " << page.items.size() << " items: " << kept
            << " bytes kept, " << listed << " at its peak\n";
  c.Expect(listed - kept < kPageBound,
           "a queue page is built without a copy of the whole queue beside it: " +
               std::to_string(listed - kept) + " >= " + std::to_string(kPageBound));
  std::filesystem::remove_all(dir, error);
}

// --- what #248 found beside the table ------------------------------------------

/// `n` short strings, as a JSON array: a metadata list the way RomM sends one.
std::string NameList(int n, const char* what) {
  std::string out = "[";
  for (int at = 0; at < n; ++at) {
    out += std::string(at == 0 ? "" : ",") + "\"" + what + " " + std::to_string(at) +
           " of a popular title\"";
  }
  return out + "]";
}

/// The members RomM 5.4.0 attaches to a rom it identified on IGDB, at a popular
/// title's widths: a summary, forty keywords, ten similar games with their
/// covers, the merged `metadatum`. ~9 KiB, against `Row`'s ~3 -- the fixture's
/// roms are homebrew nobody has metadata for, which is what the table's row
/// widths were measured on. And a platform's `description` and `firmware`,
/// which the platform list carries in the same rows.
std::string WideMembers() {
  std::string related = "[";
  for (int at = 0; at < 10; ++at) {
    related += std::string(at == 0 ? "" : ",") + "{\"id\":" + std::to_string(1000 + at) +
               ",\"name\":\"Related Game Title Number " + std::to_string(at) +
               ": Subtitle\",\"slug\":\"related-game-title-number-" + std::to_string(at) +
               "-subtitle\",\"type\":\"game\",\"cover_url\":\"https://images.igdb.com/igdb/"
               "image/upload/t_cover_big/co" +
               std::to_string(4000 + at) + ".jpg\"}";
  }
  related += "]";
  std::string firmware = "[";
  for (int at = 0; at < 6; ++at) {
    firmware += std::string(at == 0 ? "" : ",") + "{\"id\":" + std::to_string(at + 1) +
                ",\"file_name\":\"scph" + std::to_string(5500 + at) +
                ".bin\",\"file_path\":\"bios/psx\",\"file_size_bytes\":524288,\"md5_hash\":"
                "\"490f666e1afb15b7362b406ed1cea246\",\"is_verified\":true}";
  }
  firmware += "]";
  return ",\"summary\":\"" + std::string(900, 's') +
         "\",\"igdb_metadata\":{\"total_rating\":\"91.5\",\"genres\":" + NameList(3, "Genre") +
         ",\"keywords\":" + NameList(40, "Keyword") + ",\"themes\":" + NameList(3, "Theme") +
         ",\"alternative_names\":" + NameList(8, "Alternative name") +
         ",\"companies\":" + NameList(5, "Company") + ",\"game_modes\":" +
         NameList(3, "Mode") + ",\"ports\":" + related + ",\"similar_games\":" + related +
         "},\"metadatum\":{\"genres\":" + NameList(3, "Genre") +
         ",\"companies\":" + NameList(5, "Company") + ",\"collections\":" +
         NameList(2, "Collection") + "},\"merged_screenshots\":" + NameList(5, "Screenshot") +
         ",\"description\":\"" + std::string(1500, 'd') + "\",\"firmware\":" + firmware +
         ",\"has_nested_single_file\":false,\"missing_from_fs\":false,\"sha1_hash\":null,"
         "\"md5_hash\":null";
}

/// `Row` with `WideMembers` in it.
std::string WideRow(std::int64_t id) {
  std::string row = Row(id);
  row.pop_back();
  return row + WideMembers() + "}";
}

/// A row is what is read of it, however wide RomM makes it.
///
/// Every reader above parses one row at a time, but parses all of it: a row's
/// document is ~16x its text, so a rom RomM identified -- ~12 KiB of IGDB
/// metadata, none of which this client reads -- was ~190 KiB of `json::Value`s
/// in flight, beside the page's text, and a rom's detail before a download the
/// same. The fixture's homebrew rows are 2.4 KiB, which is what the heap table
/// was measured on (#248, §3). What a reader keeps of a row is a handful of
/// short fields, and that is what its document may cost.
void WideRows(checks::Checks& c) {
  // A row's kept members as nodes, their strings, and the member vector as it
  // grows: well under this on either standard library. The whole row's document
  // is twenty times it.
  constexpr std::int64_t kKeptRow = 8 * 1024;
  const std::string wide = WideRow(1);
  struct Case {
    const char* what;
    std::string body;
    std::function<bool(const std::string&)> parse;
  };
  const Case cases[] = {
      {"roms::ParsePage", "{\"items\":[" + wide + "],\"total\":1,\"limit\":1,\"offset\":0}",
       [](const std::string& body) {
         rommsync::roms::Page page;
         return rommsync::roms::ParsePage(body, &page).ok() && page.roms.size() == 1;
       }},
      {"lists::ParseRomPage", "{\"items\":[" + wide + "],\"total\":1}",
       [](const std::string& body) {
         rommsync::lists::RomPage page;
         return rommsync::lists::ParseRomPage(body, &page).ok() && page.roms.size() == 1;
       }},
      {"lists::ParsePlatforms", "[" + wide + "]",
       [](const std::string& body) {
         std::vector<rommsync::lists::PlatformRow> rows;
         return rommsync::lists::ParsePlatforms(body, &rows).ok() && rows.size() == 1;
       }},
      {"download::ParseRomDetail", wide,
       [](const std::string& body) {
         rommsync::download::RomDetail detail;
         return rommsync::download::ParseRomDetail(body, &detail).ok() &&
                detail.file_names.size() == 1;
       }},
  };
  for (const Case& one : cases) {
    c.Expect(one.parse(one.body), std::string(one.what) + " reads a wide row");
    const std::int64_t peak = PeakOf(one.body, one.parse);
    std::cout << one.what << ": " << peak << " bytes for a row of " << wide.size()
              << " bytes of text\n";
    c.Expect(peak < kKeptRow, std::string(one.what) + " costs what it reads of a wide row, not "
                                                     "its whole document: " +
                                  std::to_string(peak) + " >= " + std::to_string(kKeptRow));
  }
}

/// A fake RomM for one negotiation: it answers `answer` and notes what the
/// process held at the moment the request went out.
class Negotiating final : public http::HttpClient {
 public:
  explicit Negotiating(std::string answer) : answer_(std::move(answer)) {}

  http::Result Send(const http::Request& request) override {
    held_at_send = count_new::Now().outstanding_bytes;
    body_bytes = static_cast<std::int64_t>(request.body.size());
    http::Result result;
    result.response.status = 200;
    result.response.body = answer_;
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }

  std::int64_t held_at_send = 0;
  std::int64_t body_bytes = 0;

 private:
  std::string answer_;
};

/// The negotiate request's body is held once, and not beside the plan (§1).
///
/// One entry per save the card holds, ~250 bytes each at `scan::kMaxSaves`, and
/// until #248 it was there several times over while the plan arrived: the saves
/// copied into the payload, the body as it doubled its way to its length, and a
/// copy of it in the request -- all of it still held while the answer was
/// parsed into the plan, which is the tick's plan phase.
void NegotiateExchange(checks::Checks& c) {
  namespace sync = rommsync::sync;
  std::vector<sync::ClientSaveState> saves;
  for (std::size_t at = 0; at < rommsync::scan::kMaxSaves; ++at) {
    sync::ClientSaveState save;
    save.rom_id = static_cast<std::int64_t>(at) + 1;
    save.file_name = "Synthetic Adventure Game " + std::to_string(1000 + at) + " (USA, Europe).srm";
    save.emulator = "mgba_libretro";
    save.content_hash = std::string(32, 'a');
    save.updated_at = std::chrono::system_clock::now();
    save.file_size_bytes = 131072;
    saves.push_back(std::move(save));
  }
  const std::string answer = ArrayOf(
      rommsync::scan::kMaxSaves, OperationRow, "{\"session_id\":4,\"operations\":",
      ",\"total_upload\":0,\"total_download\":128,\"total_conflict\":0,\"total_no_op\":0}");
  const std::int64_t parsing = PeakOf(answer, [](const std::string& body) {
    return sync::ParseNegotiateResponse(body).ok();
  });

  auth::StoredToken token;
  token.server_url = "http://romm.invalid";
  token.access_token = "rmm_" + std::string(64, 'a');
  token.device_id = "3e175584-2641-44bd-913b-e42d8fc64f85";
  Negotiating client(answer);
  sync::NegotiateOptions options;
  options.max_attempts = 1;

  count_new::ResetPeak();
  const std::int64_t before = count_new::Now().outstanding_bytes;
  const sync::Negotiation negotiated = sync::Negotiate(client, token, saves, options);
  const std::int64_t peak = count_new::Now().peak_bytes - before;
  c.Expect(negotiated.ok() && negotiated.plan.operations.size() == saves.size(),
           "the plan is read: " + negotiated.message);
  const std::int64_t sending = client.held_at_send - before;
  std::cout << "the request body is " << client.body_bytes << " bytes; " << sending
            << " held as it went out; " << peak << " at the negotiation's peak, against "
            << answer.size() << " of answer and a parse that peaks at " << parsing << "\n";
  c.Expect(sending < client.body_bytes + 2048,
           "the body is held once as it goes out: " + std::to_string(sending) + " for a body of " +
               std::to_string(client.body_bytes));
  c.Expect(peak < static_cast<std::int64_t>(answer.size()) + parsing + 4096,
           "and is let go of before the plan is read: " + std::to_string(peak) + " >= " +
               std::to_string(static_cast<std::int64_t>(answer.size()) + parsing + 4096));
}

/// A list page costs the page, not the page again while it is measured (§6).
///
/// `ipc::AppendIfItFits` encoded the whole page on every append to see whether
/// it still fit, so the last append held the page's text as it doubled past
/// `kMaxPayloadBytes` -- ~24 KiB on the IPC thread, beside everything, measured
/// 13 KiB past `kListPageBytes` for a queue page (#247). The encode that hands a
/// page over is its text, once.
void ListBuild(checks::Checks& c) {
  const auto item = [](std::size_t at) {
    ipc::ListItem built;
    built.fields.push_back({"rom_id", ipc::ListValue::Integer(static_cast<std::int64_t>(at))});
    built.fields.push_back(
        {"fs_name", ipc::ListValue::Text("Synthetic Adventure Game " + std::to_string(at) +
                                         " (USA, Europe).gba")});
    built.fields.push_back({"platform_fs_slug", ipc::ListValue::Text("gba")});
    built.fields.push_back({"state", ipc::ListValue::Text("queued")});
    built.fields.push_back({"bytes_done", ipc::ListValue::Integer(0)});
    built.fields.push_back({"size_bytes", ipc::ListValue::Integer(16777216)});
    built.fields.push_back({"bytes_per_second", ipc::ListValue::Integer(0)});
    built.fields.push_back({"attempts", ipc::ListValue::Integer(0)});
    built.fields.push_back({"message", ipc::ListValue::Text("")});
    return built;
  };
  std::int64_t one = 0;
  {
    const std::int64_t before = count_new::Now().outstanding_bytes;
    const ipc::ListItem probe = item(1);
    one = count_new::Now().outstanding_bytes - before;
  }

  ipc::ListPage page;
  count_new::ResetPeak();
  std::int64_t before = count_new::Now().outstanding_bytes;
  for (std::size_t at = 0; ipc::AppendIfItFits(&page, item(at)); ++at) {
  }
  const std::int64_t held = count_new::Now().outstanding_bytes - before;
  const std::int64_t built = count_new::Now().peak_bytes - before;
  std::cout << "a page of " << page.items.size() << " items holds " << held
            << " bytes and peaked at " << built << " while it was built (one item is " << one
            << ")\n";
  c.Expect(built - held < one + 1024,
           "a page is built for the page and one item in flight: " +
               std::to_string(built - held) + " >= " + std::to_string(one + 1024));

  count_new::ResetPeak();
  before = count_new::Now().outstanding_bytes;
  const std::string text = ipc::EncodeListPage(page);
  const std::int64_t encoded = count_new::Now().peak_bytes - before;
  std::cout << "and is " << text.size() << " bytes of text, encoded at a peak of " << encoded
            << "\n";
  c.Expect(encoded < static_cast<std::int64_t>(text.size()) + 512,
           "a page is encoded into its text once: " + std::to_string(encoded) + " for " +
               std::to_string(text.size()));
}

/// A fake RomM for a tick that splits: the first negotiation names more than a
/// plan holds, so the tick halves it and asks about each half in turn.
class Chunking final : public http::HttpClient {
 public:
  http::Result Send(const http::Request& request) override {
    http::Result result;
    result.response.status = 200;
    result.response.body = "{}";
    if (request.url.find("/api/sync/negotiate") == std::string::npos) {
      return result;
    }
    held_at_negotiation.push_back(count_new::Now().outstanding_bytes);
    const bool whole = request.body.find("\"rom_ids\":[1,2]") != std::string::npos;
    const std::size_t rows = whole ? rommsync::sync::kMaxPlanOperations + 1 : 100;
    const std::int64_t rom = request.body.find("\"rom_ids\":[2]") != std::string::npos ? 2 : 1;
    result.response.body = ArrayOf(
        rows,
        [rom](std::size_t at) {
          return "{\"action\":\"no_op\",\"rom_id\":" + std::to_string(rom) + ",\"save_id\":" +
                 std::to_string(at + 1) + ",\"file_name\":\"Synthetic Adventure Game (USA, "
                 "Europe) [2026-10-21_09-02-11].srm\",\"slot\":\"slot-" +
                 std::to_string(at) +
                 "\",\"emulator\":\"mgba_libretro\",\"reason\":\"No changes since last sync\","
                 "\"server_updated_at\":\"2026-10-21T09:02:11+00:00\",\"server_content_hash\":"
                 "\"abd8fff93894e8112c7dd17386e54a5f\"}";
        },
        "{\"session_id\":" + std::to_string(held_at_negotiation.size()) + ",\"operations\":",
        ",\"total_upload\":0,\"total_download\":0,\"total_conflict\":0,\"total_no_op\":" +
            std::to_string(rows) + "}");
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }

  std::vector<std::int64_t> held_at_negotiation;
};

/// A tick asks about its next chunk without the last one's plan in hand (#248).
///
/// `TickResult` kept the last plan and what it did, so a tick of several chunks
/// held the one before beside the next one arriving -- two plans and a run's
/// results where `kHeapTickSaves` counts one plan. Each plan has been handed to
/// `TickOptions::on_plan` by then, and nothing else reads it.
void TickChunks(checks::Checks& c) {
  namespace sync = rommsync::sync;
  rommsync::host::InstallPosixFileSync();
  std::error_code error;
  const std::filesystem::path dir = std::filesystem::path(rig::ScratchDir()) / "alloc-chunks";
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  const std::unique_ptr<fs::FileSystem> files =
      rommsync::host::MakeNativeFileSystem(dir.string());

  auth::StoredToken token;
  token.server_url = "http://romm.invalid";
  token.access_token = "rmm_" + std::string(64, 'a');
  token.device_id = "3e175584-2641-44bd-913b-e42d8fc64f85";
  sync::TickOptions options;
  options.server = rommsync::server::CapabilitiesFor("5.4.0");
  options.rom_scope = std::vector<std::int64_t>{1, 2};
  options.negotiate.max_attempts = 1;
  options.finish.complete.max_attempts = 1;
  std::size_t plans = 0;
  options.on_plan = [&plans](const sync::SyncPlan&, const sync::ExecutionReport&) { ++plans; };

  Chunking client;
  const sync::TickResult tick = sync::RunTick(client, *files, token, {}, {}, {}, options);
  c.ExpectEq(plans, std::size_t{2}, "the tick ran both halves");
  if (client.held_at_negotiation.size() != 3) {
    c.Expect(false, "three negotiations: the whole scope, then each half; saw " +
                        std::to_string(client.held_at_negotiation.size()));
    return;
  }
  const std::int64_t first = client.held_at_negotiation[1];
  const std::int64_t second = client.held_at_negotiation[2];
  std::cout << "held as the first half was asked: " << first << " bytes; as the second: "
            << second << " (" << tick.totals.plans << " plans)\n";
  // A hundred operations and their results are ~100 KiB; the baseline the first
  // half wrote is a few KiB.
  c.Expect(second - first < 32 * 1024,
           "the second half is asked without the first half's plan held: " +
               std::to_string(second - first) + " bytes more");
  std::filesystem::remove_all(dir, error);
}

/// One conflict-history entry at the names the heap table budgets: a No-Intro
/// rom name and file name, a core's emulator id, RomM's reason sentence, and the
/// save's path and its backup's under a 63-byte folder.
rommsync::conflicts::Entry HistoryEntry(std::size_t at) {
  rommsync::conflicts::Entry entry;
  entry.kind = rommsync::conflicts::EntryKind::kSave;
  entry.event = rommsync::conflicts::Event::kConflict;
  entry.rom_id = static_cast<std::int64_t>(at) + 1;
  entry.rom_name = "Synthetic Adventure Game " + std::to_string(1000 + at) + " (USA, Europe)";
  entry.file_name = entry.rom_name + ".srm";
  entry.slot = "retroarch-srm";
  entry.emulator = "mgba_libretro";
  entry.when = 1757000000;
  entry.reason = "Both sides changed since last sync";
  entry.sd_path = "/retroarch/cores/savefiles/mGBA/xxxxxxxxxxxxxxxxxxxxxxxxxxxxx/" + entry.file_name;
  entry.local_size_bytes = 131072;
  entry.local_content_hash = std::string(32, 'a');
  entry.local_modified = 1757000000;
  entry.server_content_hash = std::string(32, 'b');
  entry.server_updated_at = "2026-10-21T09:02:11+00:00";
  entry.server_size_bytes = 131072;
  entry.backup_sd_path = "/config/rommsync/.backup/" + entry.file_name + ".1757000000";
  return entry;
}

/// The conflict history is resident, and a conflict recorded mid-run costs an
/// entry, not the history again (#248, §7).
///
/// Every `Append` rewrote `conflicts.db` from one string of the whole history,
/// grown by doubling to `kMaxHistoryBytes` -- and, at `kMaxEntries`, inserted
/// into a vector with no room left, which doubled it too. It is called from
/// `TickOptions::on_plan`, with the plan and its results alive: the tick's run
/// phase, which had no room for either.
void ConflictHistory(checks::Checks& c) {
  namespace conflicts = rommsync::conflicts;
  rommsync::host::InstallPosixFileSync();
  std::error_code error;
  const std::filesystem::path dir = std::filesystem::path(rig::ScratchDir()) / "alloc-history";
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);

  std::int64_t one = 0;
  {
    const std::int64_t before = count_new::Now().outstanding_bytes;
    const conflicts::Entry probe = HistoryEntry(0);
    one = count_new::Now().outstanding_bytes - before + static_cast<std::int64_t>(sizeof(probe));
  }
  conflicts::History history((dir / conflicts::kHistoryFileName).string());
  history.Load();
  for (std::size_t at = 0; at < conflicts::kMaxEntries; ++at) {
    c.Expect(history.Append(HistoryEntry(at)).ok(), "a full history is written");
  }
  conflicts::Entry next = HistoryEntry(conflicts::kMaxEntries);
  count_new::ResetPeak();
  const std::int64_t before = count_new::Now().outstanding_bytes;
  c.Expect(history.Append(std::move(next)).ok(), "an entry is recorded over a full history");
  const std::int64_t peak = count_new::Now().peak_bytes - before;
  std::cout << "one entry is " << one << " bytes; recording one over a full history of "
            << conflicts::kMaxEntries << " peaks at " << peak << "\n";
  c.Expect(peak < 4 * one, "recording a conflict costs a few entries, not the history: " +
                               std::to_string(peak) + " >= " + std::to_string(4 * one));
  std::filesystem::remove_all(dir, error);
}

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "counts";
  // Refused rather than clamped: a mistyped round count on the soak workflow's
  // form would otherwise be a four-round soak reported green.
  const int rounds = argc > 2 ? std::atoi(argv[2]) : 16;
  if (rounds < 4) {
    std::cerr << "rounds must be a number, at least 4: '" << (argc > 2 ? argv[2] : "") << "'\n";
    return 2;
  }
  // Where a rig scenario writes its samples as CSV, for the soak workflow to
  // keep as an artifact. Unset under ctest.
  const char* report = std::getenv("ROMMSYNC_ALLOC_REPORT");

  if (!count_new::kActive) {
    std::cerr << "built under a sanitizer, which replaces operator new itself; the counts "
                 "here would not be ours (count_new.hpp)\n";
    return rig::kSkip;
  }

  rig::Checks checks;
  if (scenario == "counts" || scenario == "judge" || scenario == "log_ring" ||
      scenario == "page_parse" || scenario == "heartbeat" || scenario == "response_parse" ||
      scenario == "wrong_shape" || scenario == "queue_write" || scenario == "wide_rows" ||
      scenario == "negotiate_exchange" || scenario == "list_build" ||
      scenario == "tick_chunks" || scenario == "conflict_history") {
    if (scenario == "counts") {
      Counts(checks);
    } else if (scenario == "wide_rows") {
      WideRows(checks);
    } else if (scenario == "negotiate_exchange") {
      NegotiateExchange(checks);
    } else if (scenario == "list_build") {
      ListBuild(checks);
    } else if (scenario == "tick_chunks") {
      TickChunks(checks);
    } else if (scenario == "conflict_history") {
      ConflictHistory(checks);
    } else if (scenario == "response_parse") {
      ResponseParse(checks);
    } else if (scenario == "wrong_shape") {
      WrongShape(checks);
    } else if (scenario == "queue_write") {
      QueueWrite(checks);
    } else if (scenario == "heartbeat") {
      HeartbeatCost(checks);
    } else if (scenario == "judge") {
      JudgeRule(checks);
    } else if (scenario == "page_parse") {
      PageParse(checks);
    } else {
      LogRing(checks);
    }
    if (checks.failures() == 0) {
      std::cout << "alloc." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }
  if (scenario != "steady" && scenario != "soak" && scenario != "schedule") {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  rommsync::host::InstallPosixFileSync();
  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);
  const std::string base = rig::BaseUrl();
  const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }
  harness::CloseOpenSessions(*client, base, fixture);

  if (scenario == "steady") {
    Steady(checks, *client, base, fixture, rounds, report);
  } else if (scenario == "schedule") {
    // The second argument is the hours of the fast clock each stretch lasts.
    Schedule(checks, *client, base, fixture, rounds);
  } else {
    Soak(checks, *client, base, fixture, rounds, report);
  }
  const int failures = checks.failures();
  harness::CloseOpenSessions(*client, base, fixture);
  if (failures == 0) {
    std::cout << scenario << " ok against " << base << " over " << rounds << " rounds\n";
  }
  return failures == 0 ? 0 : 1;
}

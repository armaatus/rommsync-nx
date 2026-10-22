// What the sysmodule holds between ticks, measured, and held flat (M9-15, #204).
//
// The process is meant to run for weeks without restarting, on a 1.4 MiB heap
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
#include "rommsync/device_registration.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/list_service.hpp"
#include "rommsync/log.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/server_capabilities.hpp"
#include "rommsync/state_sync.hpp"
#include "rommsync/sync.hpp"
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
          const harness::Fixture& fixture, http::HttpClient& client)
      : sandbox(c, name), directory(sandbox.Host(harness::kConfigDir) + "/") {
    for (const char* folder : {"/tico/roms/nes", "/tico/roms/gba"}) {
      sandbox.MakeDirs(folder);
    }
    ok = sandbox.Write("/config/rommsync/config.ini",
                       "[server]\nurl = " + base +
                           "\n\n[sync]\nenabled = true\ninterval_min = 0\non_boot = false\n");
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
/// lines on a console with 1.4 MiB of it, and a floor no judge can call steady.
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

/// A page of rows costs the rows it becomes and **one** row's document, not the
/// whole page's.
///
/// `json::Value` is ~100 bytes a node and a RomM row is ~100 nodes, so a parsed
/// row weighs about eleven times its text -- measured at 23 KiB a row on
/// aarch64's libstdc++ against the fixture RomM (sysmodule/source/main.cpp).
/// Holding a 64-row page as a document is ~1.5 MiB on a 1.4 MiB heap, which is a
/// `std::terminate` on a console the first time a library has that many roms.
/// So the peak of a 64-row page may exceed a 1-row page's by what the 63 extra
/// rows become -- a struct and a few short strings each -- and by nothing that
/// scales with their documents.
void PageParse(checks::Checks& c) {
  constexpr std::size_t kRows = 64;
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
/// 1.3 MiB and a 117 KiB state listing at 980 KiB, on a 1.4 MiB heap.
void ResponseParse(checks::Checks& c) {
  constexpr std::size_t kRows = 64;
  // Over what one row becomes on either standard library -- an operation is
  // the largest, ~450 bytes on aarch64 -- and under one row's document, which is
  // ~3 KiB for the smallest of the four.
  constexpr std::int64_t kPerRow = 1024;
  const auto check = [&c](const char* what, const std::string& one, const std::string& many,
                          const std::function<bool(const std::string&)>& parse) {
    c.Expect(parse(one) && parse(many), std::string(what) + " parses");
    const std::int64_t at_one = PeakOf(one, parse);
    const std::int64_t at_many = PeakOf(many, parse);
    std::cout << what << ": " << at_one << " bytes at 1 row, " << at_many << " at " << kRows
              << " (" << many.size() << " bytes of text)\n";
    c.Expect(at_many - at_one < static_cast<std::int64_t>(kRows) * kPerRow,
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
  check("download::ParseQueue", queue(1), queue(kRows), [](const std::string& text) {
    const rommsync::download::LoadedQueue loaded = rommsync::download::ParseQueue(text);
    return !loaded.discarded && !loaded.entries.empty();
  });
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
  // either standard library would disagree about.
  std::int64_t one = 0;
  {
    const std::int64_t before = count_new::Now().outstanding_bytes;
    const download::QueueEntry copy = entries.front();
    one = count_new::Now().outstanding_bytes - before + static_cast<std::int64_t>(sizeof(copy));
  }
  const std::int64_t kBound = 4 * one;
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
  // and nothing beside it may be the whole queue.
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
  c.Expect(listed - kept < kBound,
           "a queue page is built without a copy of the whole queue beside it: " +
               std::to_string(listed - kept) + " >= " + std::to_string(kBound));
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
      scenario == "wrong_shape" || scenario == "queue_write") {
    if (scenario == "counts") {
      Counts(checks);
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
  if (scenario != "steady" && scenario != "soak") {
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

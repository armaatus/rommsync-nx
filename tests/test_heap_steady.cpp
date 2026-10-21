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
#include <vector>

#include "count_new.hpp"
#include "engine.hpp"
#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/log.hpp"
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
  const auto half = static_cast<std::ptrdiff_t>(samples.size() / 2);
  const auto least = [](auto first, auto last, std::int64_t Sample::*field) {
    std::int64_t value = (*first).*field;
    for (auto it = first; it != last; ++it) {
      value = std::min(value, (*it).*field);
    }
    return value;
  };
  const auto most = [](auto first, auto last, std::int64_t Sample::*field) {
    std::int64_t value = (*first).*field;
    for (auto it = first; it != last; ++it) {
      value = std::max(value, (*it).*field);
    }
    return value;
  };
  const auto mid = samples.begin() + half;
  for (const auto& [field, allowed, unit] :
       {std::tuple{&Sample::blocks, slack.blocks, "blocks"},
        std::tuple{&Sample::bytes, slack.bytes, "bytes"}}) {
    const std::int64_t early = least(samples.begin(), mid, field);
    const std::int64_t late = least(mid, samples.end(), field);
    if (late - early > allowed) {
      return std::string("the floor rose by ") + std::to_string(late - early) + " " + unit +
             " between the first and second half of the run (" + std::to_string(early) +
             " -> " + std::to_string(late) + "), past the " + std::to_string(allowed) +
             " allowed";
    }
    const std::int64_t low = least(samples.begin(), samples.end(), field);
    const std::int64_t high = most(samples.begin(), samples.end(), field);
    if (high - low > 4 * allowed + 64) {
      return std::string("outstanding ") + unit + " spread " + std::to_string(low) + " .. " +
             std::to_string(high) + " across the run";
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

  /// Queue `rom_id` and wait for the worker to drain it onto the card.
  bool Download(std::int64_t rom_id, std::chrono::seconds budget) {
    std::string response;
    if (Call(ipc::Command::kEnqueue, ipc::EncodeRomId(rom_id), &response) != ipc::Error::kOk) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
      if (Status().queue_depth == 0) {
        // The finished row stays for the queue screen; taking it out is what a
        // user tidying the screen does, and it is what lets the next round
        // queue the same rom again.
        return Call(ipc::Command::kDequeue, ipc::EncodeRomId(rom_id), &response) ==
               ipc::Error::kOk;
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
  std::int64_t allocations_per_tick = 0;
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
  run->allocations_per_tick = window.allocations() / std::max(rounds, 1);
  return true;
}

void Report(const std::string& what, const Run& run, const char* path) {
  std::cout << what << ": floor " << run.floor_bytes << " bytes, a round peaks "
            << run.peak_over_floor << " bytes over it, ~" << run.allocations_per_tick
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

int Steady(checks::Checks& c, http::HttpClient& client, const std::string& base,
           const harness::Fixture& fixture, int rounds, const char* report) {
  TickCounter counter;
  rlog::Sink* const previous = rlog::GetSink();
  rlog::SetSink(&counter);
  {
    Console console(c, "alloc-steady", base, fixture, client);
    c.Expect(console.ok, "a console configured for, and paired with, the fixture RomM");
    console.engine.StartWorker();
    Run run;
    if (Drive(c, rounds,
              [&](int) { return console.Tick(counter, std::chrono::seconds{60}); }, &run)) {
      Report("alloc.steady", run, report);
      const std::string verdict = Judge(run.samples, kSlack);
      c.Expect(verdict.empty(), "what the engine holds between ticks does not grow with the "
                                "number of ticks: " + verdict);
      c.Expect(run.allocations_per_tick >= 100,
               "and the ticks measured really ran: " + std::to_string(run.allocations_per_tick) +
                   " allocations a tick");
    }
  }
  rlog::SetSink(previous);
  return c.failures();
}

int Soak(checks::Checks& c, http::HttpClient& client, const std::string& base,
         const harness::Fixture& fixture, int rounds, const char* report) {
  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "240pee.nes", &rom)) {
    c.Expect(false, "the seeded library holds 240pee.nes -- re-seed and rescan: "
                    "./server/testing/seed.sh && ./.venv/bin/python server/testing/provision.py");
    return c.failures();
  }
  TickCounter counter;
  rlog::Sink* const previous = rlog::GetSink();
  rlog::SetSink(&counter);
  {
    Console console(c, "soak", base, fixture, client);
    c.Expect(console.ok, "a console configured for, and paired with, the fixture RomM");
    console.engine.StartWorker();
    const std::string landed = console.sandbox.Host("/tico/roms/nes/240pee.nes");
    Run run;
    const auto round = [&](int) {
      if (!console.Tick(counter, std::chrono::seconds{60})) {
        return false;
      }
      if (!console.Download(rom.id, std::chrono::seconds{120})) {
        return false;
      }
      std::error_code error;
      const bool arrived = std::filesystem::remove(landed, error);
      c.Expect(arrived, "the rom arrived on the card, and is taken away for the next round");
      return arrived;
    };
    if (Drive(c, rounds, round, &run)) {
      Report("soak", run, report);
      const std::string verdict = Judge(run.samples, kSlack);
      c.Expect(verdict.empty(), "what the engine holds across ticks and downloads does not "
                                "grow with the number of rounds: " + verdict);
    }
  }
  rlog::SetSink(previous);
  return c.failures();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "counts";
  const int rounds = argc > 2 ? std::max(4, std::atoi(argv[2])) : 16;
  // Where a rig scenario writes its samples as CSV, for the soak workflow to
  // keep as an artifact. Unset under ctest.
  const char* report = std::getenv("ROMMSYNC_ALLOC_REPORT");

  if (!count_new::kActive) {
    std::cerr << "built under a sanitizer, which replaces operator new itself; the counts "
                 "here would not be ours (count_new.hpp)\n";
    return rig::kSkip;
  }

  rig::Checks checks;
  if (scenario == "counts" || scenario == "judge" || scenario == "log_ring") {
    if (scenario == "counts") {
      Counts(checks);
    } else if (scenario == "judge") {
      JudgeRule(checks);
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

  const int failures = scenario == "steady"
                           ? Steady(checks, *client, base, fixture, rounds, report)
                           : Soak(checks, *client, base, fixture, rounds, report);
  harness::CloseOpenSessions(*client, base, fixture);
  if (failures == 0) {
    std::cout << scenario << " ok against " << base << " over " << rounds << " rounds\n";
  }
  return failures == 0 ? 0 : 1;
}

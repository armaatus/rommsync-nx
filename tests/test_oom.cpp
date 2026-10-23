// An allocation failing at every point of a tick and of a download (#250).
//
// The sysmodule runs on a 1.5 MiB heap, built `-fno-exceptions`: an allocation
// that does not fit is a `bad_alloc` nothing catches, and that is
// `std::terminate`. M9-15 (#204) measured what a tick holds and held it flat;
// this asks the question that measurement cannot -- *when* the heap does run
// out, what does it cost? Hard rule 2 says it may not cost a save, and until
// this the answer on a console was also "and nobody will ever know why the
// sysmodule stopped".
//
// curl's way, because it is the precedent and it is exhaustive: run the
// operation once and count its allocations, then run it again failing
// allocation 1, again failing allocation 2, and so on (`count_new::FailAt`).
// Each run is a **child process**, which is what makes it a faithful model:
// the child dies in `std::terminate` exactly as the console would, with no
// destructor run, no file closed and no buffer flushed, and the parent reads
// what it left on the card.
//
//   oom.last_words  the line itself: `log::WriteLast` allocates nothing, and a
//                   child that fails an allocation with the sysmodule's
//                   terminate handler installed dies by `abort` having written
//                   `process.terminated ... St9bad_alloc` to its log.
//   oom.tick        a sync tick that downloads the server's newer copy over a
//                   save, failed at allocation N for every N it makes. After
//                   each death and the next boot's sweep the save is its old
//                   bytes or the server's, and the server's only beside a
//                   backup of the old (`crash::OldOrNew`) -- and the log's last
//                   line says why the process ended.
//   oom.download    a rom download from the queue, the same way: the rom is
//                   whole or absent, `queue.json` still reads, a save already
//                   on the card is untouched, and the line is there.
//
// **Replayed, not re-asked.** Every child would otherwise be a tick against the
// rig, and a tick that completes changes what the server says to the next one
// -- the device has synced, so the next plan is an upload, not a download. And
// failing allocation N means nothing unless allocation N is the same one in
// every run. So the parent runs the operation once against the real RomM
// through a recording client, and each child replays those answers in order
// (`Replaying`), writing a download's bytes through `io::Disk` the way the
// host backend does. The server's answers are RomM's own; only the asking is
// not repeated. A child that asks something the recording does not hold says
// so, rather than passing on a request nobody answered.
//
// **What a child counts is the whole process**: the engine, the replaying
// transport, and the standard library under both. The console's transport is a
// different one, so a failure inside the replay is a model of "the transport
// ran out", not a claim about any particular allocation the console makes.
//
// Every N is run. A tick against the fixture is a few hundred allocations and a
// child a millisecond or two, so the whole sweep is about a second and there is
// no sample to choose. `ROMMSYNC_OOM_STRIDE=k` runs every k-th and the last,
// for a library large enough that it is not -- curl's `--shallow`.
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "count_new.hpp"
#include "crash_disk.hpp"
#include "harness.hpp"
#include "last_words.hpp"
#include "rig.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/disk.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/file_sync.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/log.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/sync_tick.hpp"

namespace {

// Inside the unnamed namespace, where they hide `::sync` from <unistd.h>.
namespace auth = rommsync::auth;
namespace config = rommsync::config;
namespace crypto = rommsync::crypto;
namespace download = rommsync::download;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace rlog = rommsync::log;
namespace sync = rommsync::sync;

// --- a child, and how it ended ------------------------------------------------

/// What `waitpid` said about one child.
struct Fate {
  bool exited = false;
  int code = 0;
  int signal = 0;

  bool Aborted() const { return !exited && signal == SIGABRT; }

  std::string Describe() const {
    return exited ? "exited " + std::to_string(code)
                  : "died of signal " + std::to_string(signal);
  }
};

/// Run `body` in a child process and wait for it. The child leaves with
/// `_exit`, never `exit`: the parent's static destructors -- the scratch leaf
/// above all (tests/scratch.hpp) -- are the parent's to run.
Fate InChild(const std::function<int()>& body) {
  std::cout.flush();
  std::cerr.flush();
  const pid_t pid = ::fork();
  if (pid == 0) {
    const int code = body();
    std::cout.flush();
    std::cerr.flush();
    ::_exit(code);
  }
  Fate fate;
  int status = 0;
  if (pid < 0 || ::waitpid(pid, &status, 0) != pid) {
    fate.exited = true;
    fate.code = -1;
    return fate;
  }
  if (WIFEXITED(status)) {
    fate.exited = true;
    fate.code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    fate.signal = WTERMSIG(status);
  }
  return fate;
}

/// The child's exit code for "the replay was asked something it never heard".
constexpr int kDiverged = 3;
/// ...and for "the operation finished, but not the way the recording did".
constexpr int kWrongEnding = 4;

/// The last line of the file at `path`, or empty.
std::string LastLine(const std::string& path) {
  std::string text = rig::ReadFile(path);
  while (!text.empty() && text.back() == '\n') {
    text.pop_back();
  }
  const std::string::size_type newline = text.rfind('\n');
  return newline == std::string::npos ? text : text.substr(newline + 1);
}

// --- record once, replay in every child ---------------------------------------

/// One exchange, as the real RomM answered it.
struct Exchange {
  http::Method method = http::Method::kGet;
  std::string url;
  bool download = false;
  http::Result result;
  std::string bytes;  ///< a successful download's file
};

/// The real client, with every answer kept.
class Recording final : public http::HttpClient {
 public:
  explicit Recording(http::HttpClient& inner) : inner_(inner) {}

  http::Result Send(const http::Request& request) override {
    http::Result result = inner_.Send(request);
    exchanges.push_back({request.method, request.url, false, result, {}});
    return result;
  }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    http::Result result = inner_.Download(request, target);
    exchanges.push_back({request.method, request.url, true, result,
                         result.successful() ? rig::ReadFile(target.path) : std::string()});
    return result;
  }

  std::vector<Exchange> exchanges;

 private:
  http::HttpClient& inner_;
};

/// The recording, answered in order. A download is staged and committed
/// through `io::Disk` exactly as the host backend does it -- `.part`, close,
/// `Sync`, rename -- so the card sees the same calls it would.
class Replaying final : public http::HttpClient {
 public:
  explicit Replaying(const std::vector<Exchange>& exchanges) : exchanges_(exchanges) {}

  http::Result Send(const http::Request& request) override {
    const Exchange* next = Next(request, false);
    return next == nullptr ? Refused() : next->result;
  }

  http::Result Download(const http::Request& request,
                        const http::DownloadTarget& target) override {
    const Exchange* next = Next(request, true);
    if (next == nullptr) {
      return Refused();
    }
    if (!next->result.successful()) {
      return next->result;
    }
    const std::string partial = http::PartialPathFor(target.path);
    std::FILE* file = io::OpenForWriting(partial, "wb");
    if (file == nullptr) {
      return Refused();
    }
    const bool wrote = io::Write(file, next->bytes.data(), next->bytes.size()) == next->bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed || !io::Sync(partial) || io::Rename(partial, target.path) != 0) {
      io::Remove(partial);
      return Refused();
    }
    return next->result;
  }

  bool diverged() const { return diverged_; }
  bool finished() const { return at_ == exchanges_.size(); }

 private:
  const Exchange* Next(const http::Request& request, bool download) {
    if (diverged_ || at_ >= exchanges_.size() || exchanges_[at_].download != download ||
        exchanges_[at_].method != request.method || exchanges_[at_].url != request.url) {
      diverged_ = true;
      return nullptr;
    }
    return &exchanges_[at_++];
  }

  static http::Result Refused() {
    http::Result result;
    result.error = http::Error::kConnectFailed;
    result.message = "the replay holds no answer to this request";
    return result;
  }

  const std::vector<Exchange>& exchanges_;
  std::size_t at_ = 0;
  bool diverged_ = false;
};

// --- the sweep ----------------------------------------------------------------

/// A copy of `from` at `to`, replacing whatever was there.
bool CopyCard(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code error;
  std::filesystem::remove_all(to, error);
  std::filesystem::create_directories(to.parent_path(), error);
  std::filesystem::copy(from, to, std::filesystem::copy_options::recursive, error);
  return !error;
}

/// Which N to fail: all of them, or every `stride`-th and the last.
std::vector<std::int64_t> Selection(std::int64_t allocations) {
  std::int64_t stride = 1;
  if (const char* text = std::getenv("ROMMSYNC_OOM_STRIDE"); text != nullptr && *text != '\0') {
    stride = std::max<std::int64_t>(1, std::atoll(text));
  }
  std::vector<std::int64_t> chosen;
  for (std::int64_t n = 1; n <= allocations; n += stride) {
    chosen.push_back(n);
  }
  if (!chosen.empty() && chosen.back() != allocations) {
    chosen.push_back(allocations);
  }
  return chosen;
}

/// One operation under test, as the sweep needs it.
struct Subject {
  std::string what;
  /// The card as it stands before the operation.
  std::filesystem::path card;
  /// Runs in the child, on a fresh copy of `card` at `root`, with the replay
  /// for a client and the log's sink installed. Answers the child's exit code
  /// when the operation runs to its end: 0 when it ended the way the recording
  /// did, `kWrongEnding` otherwise.
  std::function<int(const std::filesystem::path& root, Replaying& client)> operate;
  /// What must be true of the card after a death and the next boot's sweep.
  /// Empty when it is.
  std::function<std::string(const std::filesystem::path& root)> judge;
};

std::string LogPathUnder(const std::filesystem::path& root) {
  return (root / "config" / "rommsync" / rlog::kLogFileName).string();
}

/// Run `subject` in a child with no failure armed; return how many allocations
/// the operation itself made, or -1.
std::int64_t CountAllocations(checks::Checks& c, const Subject& subject,
                              const std::vector<Exchange>& recording,
                              const std::filesystem::path& root) {
  if (!CopyCard(subject.card, root)) {
    c.Expect(false, subject.what + ": the card could be copied for the counting run");
    return -1;
  }
  int channel[2];
  if (::pipe(channel) != 0) {
    c.Expect(false, "a pipe for the counting run");
    return -1;
  }
  const Fate fate = InChild([&]() {
    ::close(channel[0]);
    rlog::FileSink sink(LogPathUnder(root));
    rlog::SetSink(&sink);
    Replaying client(recording);
    const std::int64_t before = count_new::Now().allocations;
    const int code = subject.operate(root, client);
    const std::int64_t made = count_new::Now().allocations - before;
    if (client.diverged() || !client.finished()) {
      return kDiverged;
    }
    const bool sent = ::write(channel[1], &made, sizeof made) == static_cast<ssize_t>(sizeof made);
    return sent ? code : 1;
  });
  ::close(channel[1]);
  std::int64_t made = -1;
  if (::read(channel[0], &made, sizeof made) != static_cast<ssize_t>(sizeof made)) {
    made = -1;
  }
  ::close(channel[0]);
  c.Expect(fate.exited && fate.code == 0,
           subject.what + ": replayed with no failure, the operation ends as the recording "
                          "did (" + fate.Describe() + ")");
  if (fate.exited && fate.code == 0) {
    c.Expect(subject.judge(root).empty(),
             subject.what + ": and leaves a card the judge accepts: " + subject.judge(root));
  }
  return fate.exited && fate.code == 0 ? made : -1;
}

/// The sweep itself: fail allocation N in a child, for every N chosen.
void Sweep(checks::Checks& c, const Subject& subject, const std::vector<Exchange>& recording) {
  const std::filesystem::path root =
      std::filesystem::path(rig::ScratchDir()) / "oom" / (subject.what + "-card");
  const std::int64_t allocations = CountAllocations(c, subject, recording, root);
  if (allocations <= 0) {
    c.Expect(false, subject.what + ": the counting run measured its allocations");
    return;
  }
  const std::vector<std::int64_t> chosen = Selection(allocations);
  std::cout << subject.what << ": " << allocations << " allocations; failing " << chosen.size()
            << " of them, one run each\n";

  int reported = 0;
  for (const std::int64_t n : chosen) {
    const std::string at = subject.what + ", allocation " + std::to_string(n) + " of " +
                           std::to_string(allocations);
    if (!CopyCard(subject.card, root)) {
      c.Expect(false, at + ": the card could be copied");
      return;
    }
    const Fate fate = InChild([&]() {
      rlog::FileSink sink(LogPathUnder(root));
      rlog::SetSink(&sink);
      rommsync::sysmodule::InstallLastWords();
      Replaying client(recording);
      count_new::FailAt(n);
      const int code = subject.operate(root, client);
      count_new::FailAt(0);
      if (client.diverged()) {
        return kDiverged;
      }
      // Reached the end: the failed allocation was a `nothrow` one the code
      // answered for itself, and the run is judged like any other.
      return count_new::InjectedFailures() > 0 ? code : kWrongEnding;
    });

    // Either the failure ended the process through the terminate handler -- an
    // `abort` -- or a `nothrow` allocation was refused and the code carried on
    // to an ordinary end. Anything else -- a segfault on a null nobody
    // checked, a replay that went off the recording -- is a failure here.
    const bool died = fate.Aborted();
    const bool carried_on = fate.exited && fate.code == 0;
    if (!died && !carried_on) {
      c.Expect(false, at + ": ends in std::terminate or carries on; it " + fate.Describe() +
                          (fate.exited && fate.code == kDiverged
                               ? " (the replay was asked something it never recorded)"
                               : ""));
      continue;
    }
    if (died) {
      const std::string last = LastLine(LogPathUnder(root));
      const bool said = last.find("error process.terminated") != std::string::npos &&
                        last.find("bad_alloc") != std::string::npos;
      c.Expect(said, at + ": the log's last line says the sysmodule stopped, and why: '" +
                         last + "'");
      reported += said ? 1 : 0;
    }
    const std::string verdict = subject.judge(root);
    c.Expect(verdict.empty(), at + ": " + verdict);
    if (c.failures() > 20) {
      std::cerr << "  (stopping after twenty failures)\n";
      return;
    }
  }
  std::cout << subject.what << ": " << reported << " of " << chosen.size()
            << " runs died, every one with its last line written, and no save was lost\n";
  c.Expect(reported > 0, subject.what + ": at least one failed allocation ended the process, "
                                        "so the sweep exercised the terminate path at all");
}

// --- the scenarios ------------------------------------------------------------

/// The line, and the handler that writes it.
void LastWords(checks::Checks& c) {
  const std::filesystem::path dir = std::filesystem::path(rig::ScratchDir()) / "oom-last-words";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  const std::string path = (dir / "rommsync.log").string();

  {
    char detail[96];
    rommsync::sysmodule::DescribeTermination("St9bad_alloc", detail, sizeof detail);
    c.ExpectEq(std::string(detail),
               std::string("an uncaught St9bad_alloc: the heap is spent, and the sysmodule stopped"),
               "a bad_alloc is named, and said to be the heap");
    rommsync::sysmodule::DescribeTermination("St12length_error", detail, sizeof detail);
    c.ExpectEq(std::string(detail),
               std::string("an uncaught St12length_error: the sysmodule stopped"),
               "anything else is named by its type alone");
    char narrow[25];
    rommsync::sysmodule::DescribeTermination("St9bad_alloc", narrow, sizeof narrow);
    c.ExpectEq(std::string(narrow), std::string("an uncaught St9bad_alloc"),
               "and the sentence is cut to the buffer rather than written past it");
    rommsync::sysmodule::DescribeTermination(nullptr, detail, sizeof detail);
    c.ExpectEq(std::string(detail), std::string("std::terminate with no exception in flight"),
               "a terminate with nothing thrown says so");
  }

  rlog::Reset();
  rlog::FileSink sink(path);
  rlog::Sink* const previous = rlog::GetSink();
  rlog::SetSink(&sink);
  rlog::Info(rlog::Event::kBoot, "a line before the last one");
  {
    const count_new::Window window;
    rlog::WriteLast(rlog::Event::kTerminated, "an uncaught St9bad_alloc");
    c.ExpectEq(window.allocations(), std::int64_t{0}, "the last line allocates nothing");
  }
  c.ExpectEq(LastLine(path), std::string("2 error process.terminated an uncaught St9bad_alloc"),
             "it is the next ordinal, at error, in the shape every line has");

  rlog::SetSinkEnabled(false);
  rlog::WriteLast(rlog::Event::kTerminated, "while asleep");
  c.Expect(LastLine(path).find("while asleep") == std::string::npos,
           "a sleeping console's card is not written, even for the last line");
  rlog::SetSinkEnabled(true);
  rlog::SetSink(previous);

  // The whole path, in a process that is allowed to die of it.
  const Fate fate = InChild([&]() {
    rlog::FileSink dying(path);
    rlog::SetSink(&dying);
    rommsync::sysmodule::InstallLastWords();
    count_new::FailAt(1);
    auto* lost = new std::string(64, 'x');  // the allocation that fails
    return lost->empty() ? 1 : 2;           // never reached
  });
  c.Expect(fate.Aborted(), "a failed allocation ends the child in abort: " + fate.Describe());
  const std::string last = LastLine(path);
  c.Expect(last.find(" error process.terminated an uncaught ") != std::string::npos &&
               last.find("bad_alloc") != std::string::npos,
           "after writing why, by the exception's type: '" + last + "'");
  std::filesystem::remove_all(dir, error);
}

auth::StoredToken TokenFor(const std::string& base, const harness::Fixture& fixture) {
  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  return token;
}

/// A tick whose plan is a download over a save, failed everywhere.
void Tick(checks::Checks& c, http::HttpClient& client, const std::string& base,
          const harness::Fixture& fixture, const harness::Rom& rom) {
  const std::string name = "oom-tick.srm";
  const std::string slot = harness::UniqueSlot("m9-23-oom");
  const std::string previous = "the save running out of memory must not cost\n";
  const std::string server_bytes = "the server's newer copy of it\n";
  const std::string save = harness::SavePath(name);

  harness::Sandbox card(c, "oom-tick");
  card.Detach();  // the judge below is the audit, run after every death
  card.Write(save, previous);

  harness::Save server;
  {
    const std::string staged = card.Host("/config/rommsync/server-copy.srm");
    rig::WriteFile(staged, server_bytes);
    const bool stored = harness::UploadSave(client, base, fixture, rom.id, slot, "m9-23", staged,
                                            name, /*with_device=*/false, &server);
    std::filesystem::remove(staged);
    if (!stored) {
      c.Expect(false, "the server's copy was stored");
      return;
    }
  }

  // Reported an hour old, so the plan is a download and not a conflict.
  const sync::ClientSaveState local = harness::LocalSave(
      rom.id, name, slot, "m9-23", crypto::Md5Hex(previous),
      std::chrono::system_clock::now() - std::chrono::hours{1},
      static_cast<std::int64_t>(previous.size()));
  const std::vector<sync::SaveTarget> targets = {{rom.id, slot, save, name}};
  sync::TickOptions options;
  options.execute.backup_dir = harness::kBackupDir;
  options.execute.now = [] { return sync::Timestamp{} + std::chrono::seconds{1'757'000'900}; };
  options.negotiate.wait = [](std::chrono::milliseconds) {};
  options.negotiate.max_attempts = 1;
  options.finish.complete.wait = [](std::chrono::milliseconds) {};
  options.finish.complete.max_attempts = 1;
  options.recover_dirs = {harness::kSavesDir, harness::kBackupDir};
  const auth::StoredToken token = TokenFor(base, fixture);

  const auto run = [&](http::HttpClient& with, const std::filesystem::path& root) {
    const auto files = rommsync::host::MakeNativeFileSystem(root.string());
    return sync::RunTick(with, *files, token, {local}, targets, /*previous=*/{}, options);
  };

  // Once against the real RomM, on a copy, to record what it says.
  std::vector<Exchange> recording;
  {
    const std::filesystem::path root =
        std::filesystem::path(rig::ScratchDir()) / "oom" / "tick-recorded";
    CopyCard(card.root(), root);
    Recording recorder(client);
    const sync::TickResult tick = run(recorder, root);
    c.Expect(tick.outcome == sync::TickOutcome::kCompleted && tick.executed.completed == 1,
             std::string("the recorded tick downloads the server's copy: ") +
                 sync::ToString(tick.outcome));
    recording = std::move(recorder.exchanges);
  }
  harness::DeleteSave(client, base, fixture, server.id);
  if (c.failures() > 0) {
    return;
  }

  const crash::Snapshot before = crash::Read(card.root());
  const crash::Judge old_or_new =
      crash::OldOrNew({save, previous, server_bytes, harness::kBackupDir});
  Subject subject;
  subject.what = "oom.tick";
  subject.card = card.root();
  subject.operate = [&](const std::filesystem::path& root, Replaying& replay) {
    const sync::TickResult tick = run(replay, root);
    return tick.outcome == sync::TickOutcome::kCompleted ? 0 : kWrongEnding;
  };
  subject.judge = [&](const std::filesystem::path& root) -> std::string {
    // What the next boot does first, before anything reads the card.
    const auto files = rommsync::host::MakeNativeFileSystem(root.string());
    sync::RecoverStaging(*files, options.recover_dirs);
    return old_or_new(crash::Read(root).files, before);
  };
  Sweep(c, subject, recording);
}

/// A rom download from the queue, failed everywhere.
void Download(checks::Checks& c, http::HttpClient& client, const std::string& base,
              const harness::Fixture& fixture, const harness::Rom& rom) {
  const std::string save = harness::SavePath("240pee.srm");
  const std::string save_bytes = "a save the rom download must not touch\n";
  const std::string destination = "/tico/roms/nes/240pee.nes";

  harness::Sandbox card(c, "oom-download");
  card.Detach();
  card.MakeDirs("/tico/roms/nes");
  card.Write(save, save_bytes);

  const config::Config settings =
      config::ParseConfig("[server]\nurl = https://romm.example.com\n").value;
  const auto drain = [&](http::HttpClient& with, const std::filesystem::path& root) {
    const auto files = rommsync::host::MakeNativeFileSystem(root.string());
    download::WorkerOptions options;
    options.base_url = base;
    options.bearer_token = fixture.token;
    options.queue_path = (root / "config" / "rommsync" / download::kQueueFileName).string();
    options.max_attempts = 1;
    options.wait = [](std::chrono::milliseconds) {};
    download::Queue queue;
    std::int32_t position = 0;
    queue.Enqueue(rom.id, &position);
    return download::Drain(with, *files, settings, queue, options);
  };

  std::vector<Exchange> recording;
  std::string rom_bytes;
  {
    const std::filesystem::path root =
        std::filesystem::path(rig::ScratchDir()) / "oom" / "download-recorded";
    CopyCard(card.root(), root);
    Recording recorder(client);
    const download::DrainResult result = drain(recorder, root);
    c.Expect(result.outcome == download::DrainOutcome::kCompleted && result.downloaded == 1,
             std::string("the recorded drain downloads the rom: ") +
                 download::ToString(result.outcome) + " " + result.message);
    rom_bytes = rig::ReadFile((root / "tico" / "roms" / "nes" / "240pee.nes").string());
    recording = std::move(recorder.exchanges);
  }
  if (c.failures() > 0 || rom_bytes.empty()) {
    c.Expect(!rom_bytes.empty(), "the recorded drain left the rom on the card");
    return;
  }

  Subject subject;
  subject.what = "oom.download";
  subject.card = card.root();
  subject.operate = [&](const std::filesystem::path& root, Replaying& replay) {
    const download::DrainResult result = drain(replay, root);
    return result.outcome == download::DrainOutcome::kCompleted ? 0 : kWrongEnding;
  };
  subject.judge = [&](const std::filesystem::path& root) -> std::string {
    const auto host = [&root](const std::string& sd_path) {
      return (root / crash::RelativeOf(sd_path)).string();
    };
    if (rig::ReadFile(host(save)) != save_bytes) {
      return "the save beside the rom is not the bytes it held";
    }
    std::error_code error;
    if (std::filesystem::exists(host(destination), error) &&
        rig::ReadFile(host(destination)) != rom_bytes) {
      return "the rom is on the card and is not the server's bytes";
    }
    const std::string queue_path =
        (root / "config" / "rommsync" / download::kQueueFileName).string();
    if (std::filesystem::exists(queue_path, error)) {
      const download::LoadedQueue loaded = download::LoadQueue(queue_path);
      if (!loaded.diagnostics.empty()) {
        return "queue.json does not read back: " + loaded.DescribeDiagnostics();
      }
    }
    return {};
  };
  Sweep(c, subject, recording);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "last_words";
  if (!count_new::kActive) {
    std::cerr << "built under a sanitizer, which replaces operator new itself; nothing here "
                 "can fail an allocation (count_new.hpp)\n";
    return rig::kSkip;
  }
  rommsync::host::InstallPosixFileSync();
  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);

  rig::Checks checks;
  if (scenario == "last_words") {
    LastWords(checks);
  } else if (scenario == "tick" || scenario == "download") {
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
    harness::Rom rom;
    if (!harness::FindRom(*client, base, fixture, "240pee.nes", &rom)) {
      checks.Expect(false, "the seeded library holds 240pee.nes -- re-seed and rescan: "
                           "./server/testing/seed.sh && ./.venv/bin/python "
                           "server/testing/provision.py");
    } else if (scenario == "tick") {
      harness::CloseOpenSessions(*client, base, fixture);
      Tick(checks, *client, base, fixture, rom);
      harness::CloseOpenSessions(*client, base, fixture);
    } else {
      Download(checks, *client, base, fixture, rom);
    }
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }
  if (checks.failures() == 0) {
    std::cout << "oom." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

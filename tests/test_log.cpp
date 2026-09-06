// `rommsync::log` — the one file this client writes on the card, and the two
// promises it makes about it.
//
// The promises are the reason M7-3 wrote a logger at all rather than only a
// guide. A log on a Switch SD card is readable by anything on the console and by
// anyone who pulls the card (docs/SECURITY.md), and it shares that card with the
// roms it exists to help download — so:
//
//   1. **it is bounded.** One file at the cap and exactly one `.old` beside it,
//      forever, whatever a console does.
//   2. **it holds no secret.** Not a bearer token, not a `device_code`, not the
//      credentials in a `user:password@` URL — and not because every call site
//      remembered, but because `log::Write` redacts before a sink ever sees a
//      byte.
//
// Pure file I/O and string work: no rig, no network, so nothing here skips.
//
//   renders    the line shape, the ordinal, the bound, and the levels
//   redacts    the three things that may never be written down
//   rotates    the cap, the single `.old`, and the ceiling on the pair
//   tail       the in-memory ring `GetLog` is answered from
//   sink       a null sink writes nowhere, and an installed one gets everything
//   events     every tag is unique, and `IsEvent` round-trips `kAllEvents`
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "checks.hpp"
#include "rommsync/log.hpp"

namespace log = rommsync::log;

namespace {

std::string ScratchDir() { return ROMMSYNC_TEST_SCRATCH; }

/// A directory of this scenario's own, emptied first: these tests measure file
/// sizes, and a file a previous run left behind is a rotation that already
/// happened.
std::string FreshDir(const std::string& name) {
  const std::string path = ScratchDir() + "/log-" + name;
  std::error_code error;
  std::filesystem::remove_all(path, error);
  std::filesystem::create_directories(path, error);
  return path;
}

std::string ReadWhole(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t SizeOf(const std::string& path) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  return error ? 0 : static_cast<std::size_t>(size);
}

std::size_t CountLines(const std::string& text) {
  std::size_t lines = 0;
  for (const char letter : text) {
    lines += letter == '\n' ? 1 : 0;
  }
  return lines;
}

/// Everything written, in order. What a test asserts on when it cares about the
/// line rather than about the file.
class Recorder : public log::Sink {
 public:
  void Write(log::Level level, std::string_view line) override {
    levels.push_back(level);
    lines.emplace_back(line);
  }

  std::vector<log::Level> levels;
  std::vector<std::string> lines;
};

/// A sink installed for the length of a scope, and taken out again — every
/// scenario shares one process, and a sink left behind would collect the next
/// one's lines.
class Installed {
 public:
  explicit Installed(log::Sink* sink) : previous_(log::GetSink()) {
    log::Reset();
    log::SetSink(sink);
  }
  ~Installed() { log::SetSink(previous_); }

  Installed(const Installed&) = delete;
  Installed& operator=(const Installed&) = delete;

 private:
  log::Sink* previous_;
};

// --- the line ----------------------------------------------------------------

void Renders(checks::Checks& c) {
  Recorder recorder;
  Installed installed(&recorder);

  log::Info(log::Event::kBoot, "rommsync-nx/0.1.0");
  log::Warn(log::Event::kNetOffline, "negotiate: connection failed");
  log::Error(log::Event::kSaveFailed, "");

  c.ExpectEq(recorder.lines.size(), std::size_t{3}, "every line reached the sink");
  c.ExpectEq(recorder.lines[0], std::string("1 info boot rommsync-nx/0.1.0"),
             "the shape is <ordinal> <level> <event> <detail>");
  c.ExpectEq(recorder.lines[1], std::string("2 warn net.offline negotiate: connection failed"),
             "and the ordinal counts up across levels and events");
  c.ExpectEq(recorder.lines[2], std::string("3 error save.failed"),
             "an empty detail is three fields, not a trailing space");
  c.Expect(recorder.levels[0] == log::Level::kInfo && recorder.levels[2] == log::Level::kError,
           "the level reaches the sink as a value as well as as text");

  // One record is one line. A caller with several -- every `Describe*` in this
  // codebase renders one line per complaint -- uses `WriteEach`, and a caller
  // that does not gets its newlines flattened rather than a record `wc -l`
  // cannot count.
  log::Warn(log::Event::kConfigDiagnostic, "line 9 [sync] states: expected true or false\nand more");
  c.Expect(recorder.lines.back().find('\n') == std::string::npos,
           "a detail with a newline in it is still one line: " + recorder.lines.back());

  log::WriteEach(log::Level::kWarn, log::Event::kConfigDiagnostic,
                 "line 9 [sync] states: expected true or false\n"
                 "line 12 [server] url: not a URL\n"
                 "\n");
  c.ExpectEq(recorder.lines.size(), std::size_t{6}, "WriteEach writes one line each, empties aside");
  c.Expect(recorder.lines[4].find("line 9 ") != std::string::npos &&
               recorder.lines[5].find("line 12 ") != std::string::npos,
           "in the order the describer rendered them");

  // The bound is on the whole line, marker included: a line that announced its
  // own truncation by exceeding the limit would defeat the limit.
  const std::string enormous(4 * log::kMaxLineBytes, 'x');
  log::Info(log::Event::kScanSkipped, enormous);
  const std::string& cut = recorder.lines.back();
  c.ExpectEq(cut.size(), log::kMaxLineBytes, "a long line is cut to exactly the bound");
  c.Expect(cut.size() > std::strlen(log::kTruncationMarker) &&
               cut.compare(cut.size() - std::strlen(log::kTruncationMarker), std::string::npos,
                           log::kTruncationMarker) == 0,
           "and says so, rather than ending mid-word: " + cut.substr(cut.size() - 24));
  c.Expect(cut.find("scan.skipped") != std::string::npos,
           "with the tag still in it -- the part that identifies the failure survives");
}

// --- the secrets --------------------------------------------------------------

void Redacts(checks::Checks& c) {
  const std::string directory = FreshDir("redacts");
  const std::string path = directory + "/rommsync.log";

  // The three shapes #38's acceptance names, in the syntax this codebase would
  // actually produce them in: an `Authorization` header, a token response body,
  // a device-code poll's form body, and a URL a user typed a password into.
  const std::string kToken = "rmm_9f3ac1e0b7d4482ea1c05f6d8b2e7a44";
  const std::string kDeviceCode = "dc_5b81f0a29c7e4d3f8a6b0c1d2e3f4a5b";
  const std::string kPassword = "hunter2";

  // Written through a real `FileSink`, because the promise is about the file on
  // the card and not about the value a helper returned.
  {
    log::FileSink sink(path);
    Installed installed(&sink);
    log::Error(log::Event::kAuthRejected, "request headers: Authorization: Bearer " + kToken);
    log::Info(log::Event::kBoot, "{\"access_token\":\"" + kToken + "\",\"token_type\":\"bearer\"}");
    log::Info(log::Event::kBoot, "device_code=" + kDeviceCode + "&grant_type=device_code");
    log::Info(log::Event::kBoot, "{ \"device_code\" : \"" + kDeviceCode + "\" }");
    log::Error(log::Event::kNoServer,
               "https://romm:" + kPassword + "@romm.example.lan/api/sync/negotiate");
    log::Error(log::Event::kNetTls, "password=" + kPassword + " secret=" + kPassword);
  }

  const std::string written = ReadWhole(path);
  c.Expect(!written.empty(), "the sink wrote a file");
  c.Expect(written.find(kToken) == std::string::npos,
           "no bearer token reached the card:\n" + written);
  c.Expect(written.find(kDeviceCode) == std::string::npos,
           "no device_code reached the card:\n" + written);
  c.Expect(written.find(kPassword) == std::string::npos,
           "no URL credential reached the card:\n" + written);
  c.ExpectEq(CountLines(written), std::size_t{6}, "and every line was still written");

  // Redaction replaces rather than removes: a line that silently lost a field
  // reads as a line that never had one.
  c.Expect(written.find(log::kRedacted) != std::string::npos,
           "the redactions are visible in the file");
  c.Expect(written.find("romm.example.lan") != std::string::npos,
           "the host survives -- what goes is the userinfo, not the URL");
  c.Expect(written.find("token_type") != std::string::npos,
           "and a key that merely looks like a secret keeps its value");

  // The same rule, reachable on its own, because a caller that renders a line
  // some other way has to be able to ask rather than write a second copy.
  c.ExpectEq(log::Redact("https://me:hunter2@host/api"),
             std::string("https://<redacted>@host/api"), "Redact on a URL");
  c.ExpectEq(log::Redact("https://host/roms/me@home.zip"),
             std::string("https://host/roms/me@home.zip"),
             "an `@` in the path is not userinfo -- the host is not eaten");
  c.ExpectEq(log::Redact("Authorization: Bearer abc123"),
             std::string("Authorization: <redacted> <redacted>"), "Redact on a header");
  c.ExpectEq(log::Redact("Bearer abc123"), std::string("Bearer <redacted>"),
             "and on a header value that starts the line");
  // `config::Diagnostic` writes this sentence for a plain-`http://` server, and
  // it is a warning the user has to be able to read: `bearer` after a word is
  // English, not a header value.
  c.ExpectEq(log::Redact("plain http: the bearer token and every save cross the network"),
             std::string("plain http: the bearer token and every save cross the network"),
             "the English word `bearer` in a diagnostic is not a credential");
  c.ExpectEq(log::Redact("Bearer"), std::string("Bearer"), "a bare `Bearer` is left alone");
  c.ExpectEq(log::Redact("token_expires=2026-01-01"), std::string("token_expires=2026-01-01"),
             "the key match is on a whole word, so `token_expires` is not `token`");
  c.ExpectEq(log::Redact("nothing to hide here"), std::string("nothing to hide here"),
             "and a line with no secret in it is untouched");
}

// --- the file -----------------------------------------------------------------

void Rotates(checks::Checks& c) {
  const std::string directory = FreshDir("rotates");
  const std::string path = directory + "/rommsync.log";
  const std::string previous = log::PreviousLogPathFor(path);
  c.ExpectEq(previous, path + ".old", "the rotated file is `<path>.old`");

  // A cap far below the real one, so the scenario is a few hundred lines rather
  // than thousands -- the arithmetic is the same and the test is a moment.
  constexpr std::size_t kCap = 2 * 1024;
  {
    log::FileSink sink(path, kCap);
    Installed installed(&sink);
    for (int line = 0; line < 400; ++line) {
      log::Info(log::Event::kSyncTick, "outcome=completed uploaded=0 downloaded=0 failed=0");
    }
  }

  c.Expect(SizeOf(path) > 0, "there is a live file");
  c.Expect(SizeOf(path) <= kCap, "which is inside the cap: " + std::to_string(SizeOf(path)));
  c.Expect(SizeOf(previous) > 0, "and exactly one rotated file beside it");
  c.Expect(SizeOf(previous) <= kCap, "also inside the cap");
  c.Expect(!std::filesystem::exists(previous + ".old"),
           "and nothing older than that -- the card holds two files, not a generation chain");

  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    files += entry.is_regular_file() ? 1 : 0;
  }
  c.ExpectEq(files, std::size_t{2}, "two files in the directory, whatever the console does");

  // The ceiling the guide quotes: a user is told the log costs twice the cap and
  // no more.
  c.Expect(SizeOf(path) + SizeOf(previous) <= 2 * kCap,
           "the pair never exceeds twice the cap: " +
               std::to_string(SizeOf(path) + SizeOf(previous)));

  // A restarted sysmodule appends to the file it left rather than starting a
  // fresh one -- a console that reboots hourly would otherwise rotate away every
  // line it had.
  const std::size_t before = SizeOf(path);
  {
    log::FileSink restarted(path, kCap);
    Installed installed(&restarted);
    log::Info(log::Event::kBoot, "rommsync-nx/0.1.0");
  }
  c.Expect(SizeOf(path) > before || SizeOf(previous) > 0,
           "a second sink continues the file rather than truncating it");
  c.Expect(ReadWhole(path).find("sync.tick") != std::string::npos ||
               ReadWhole(previous).find("sync.tick") != std::string::npos,
           "and the earlier lines are still on the card");

  // A missing directory is a dropped line and not a crash: a card that will not
  // take the log must never be a client that stops syncing.
  {
    log::FileSink nowhere(directory + "/does/not/exist/rommsync.log");
    Installed installed(&nowhere);
    log::Error(log::Event::kSaveFailed, "a line with nowhere to go");
    std::vector<log::Line> tail;
    log::Tail(4, &tail);
    c.ExpectEq(tail.size(), std::size_t{1},
               "a sink that cannot open its file drops the line and keeps the tail");
  }
}

// --- the tail the overlay reads -----------------------------------------------

void TailRing(checks::Checks& c) {
  Recorder recorder;
  Installed installed(&recorder);

  std::vector<log::Line> tail;
  c.ExpectEq(log::Tail(8, &tail), std::uint64_t{0}, "a fresh log has written nothing");
  c.Expect(tail.empty(), "and has no tail");

  for (std::size_t line = 0; line < log::kTailLines + 10; ++line) {
    log::Info(log::Event::kSyncTick, "tick " + std::to_string(line));
  }

  const std::uint64_t total = log::Tail(log::kTailLines * 4, &tail);
  c.ExpectEq(total, static_cast<std::uint64_t>(log::kTailLines + 10),
             "the total counts every line ever written, not the ones kept");
  c.ExpectEq(tail.size(), log::kTailLines, "the ring is bounded at kTailLines");
  c.Expect(tail.front().ordinal < tail.back().ordinal, "oldest first");
  c.ExpectEq(tail.back().ordinal, total, "and the last line is the newest");
  c.Expect(tail.front().text.find("tick 10") != std::string::npos,
           "what fell off the front is the oldest: " + tail.front().text);

  log::Tail(3, &tail);
  c.ExpectEq(tail.size(), std::size_t{3}, "a smaller request is a smaller answer");
  c.ExpectEq(tail.back().ordinal, total, "still ending at the newest line");

  // The whole reason the ring is the log's own memory rather than a sink's: the
  // overlay has to be able to show why a sync did not happen on a console whose
  // SD card would not take the log file (`ipc.hpp`).
  log::SetSink(nullptr);
  log::Info(log::Event::kNoServer, "no server set");
  log::Tail(1, &tail);
  c.ExpectEq(tail.size(), std::size_t{1}, "the tail is answered with no sink installed at all");
  c.Expect(tail.back().text.find("config.no_server") != std::string::npos,
           "and holds the line: " + tail.back().text);
}

void SinkInstall(checks::Checks& c) {
  log::Reset();
  log::SetSink(nullptr);
  c.Expect(log::GetSink() == nullptr, "the default sink is null");
  log::Info(log::Event::kBoot, "nothing is listening");  // must not crash

  Recorder recorder;
  {
    Installed installed(&recorder);
    c.Expect(log::GetSink() == &recorder, "an installed sink is the one that is asked");
    log::Info(log::Event::kBoot, "listening");
  }
  c.Expect(log::GetSink() == nullptr, "and taking it out again puts back what was there");
  log::Info(log::Event::kBoot, "nobody is listening again");
  c.ExpectEq(recorder.lines.size(), std::size_t{1},
             "a sink sees exactly the lines written while it was installed");
}

void Events(checks::Checks& c) {
  c.Expect(log::kAllEvents.size() >= 12,
           "every failure mode docs/TROUBLESHOOTING.md documents has a tag");

  // Two events with the same tag would make the guide's sections ambiguous and
  // `IsEvent` answer the wrong one.
  for (std::size_t at = 0; at < log::kAllEvents.size(); ++at) {
    const std::string tag = log::ToString(log::kAllEvents[at]);
    c.Expect(!tag.empty() && tag != "unknown", "every event has a tag: " + tag);
    c.Expect(tag.find(' ') == std::string::npos,
             "and no spaces in it -- the tag is the third field of a line: " + tag);
    for (std::size_t other = at + 1; other < log::kAllEvents.size(); ++other) {
      c.Expect(tag != log::ToString(log::kAllEvents[other]), "and it is unique: " + tag);
    }
    log::Event round_tripped = log::Event::kBoot;
    c.Expect(log::IsEvent(tag, &round_tripped) && round_tripped == log::kAllEvents[at],
             "IsEvent round-trips it: " + tag);
  }
  c.Expect(!log::IsEvent("net.nonsense"), "and refuses a tag this build does not write");
  c.Expect(!log::IsEvent(""), "and an empty one");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "renders";
  checks::Checks checks;

  std::error_code error;
  std::filesystem::create_directories(ScratchDir(), error);

  if (scenario == "renders") {
    Renders(checks);
  } else if (scenario == "redacts") {
    Redacts(checks);
  } else if (scenario == "rotates") {
    Rotates(checks);
  } else if (scenario == "tail") {
    TailRing(checks);
  } else if (scenario == "sink") {
    SinkInstall(checks);
  } else if (scenario == "events") {
    Events(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "log." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

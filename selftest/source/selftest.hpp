// rommsync-selftest -- the pieces main.cpp wires together (M12-1, #306).
//
// Every row the report can carry is named in scripts/selftest-judge.py's ROWS,
// and `ctest -R selftest.rows` holds this directory to exactly those ids: a row
// written here and not judged there is a measurement nobody reads, and a row
// judged there and not written here is a FAIL on every console.
#pragma once

#include <switch.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace selftest {

/// The one folder this program writes under. Everything it creates -- the
/// report, its log, the pairing token, the scratch saves and downloads, the
/// backups the tick makes -- is a path built from these, and `ctest -R
/// selftest.roots` refuses any other `sdmc:` literal in this directory (#306
/// acceptance). Never the sysmodule's `/config/rommsync/`: a selftest that
/// paired over the real token or wrote into the real `.backup/` would be
/// testing the console it was meant to leave alone.
inline constexpr const char* kRoot = "sdmc:/config/rommsync-selftest/";
/// The same folder SD-root absolute, the way core/ names paths
/// (`fs::FileSystem::Resolve` turns one into the other).
inline constexpr const char* kRootSd = "/config/rommsync-selftest";

/// The sysmodule's `kInnerHeapSize` (sysmodule/source/main.cpp): what the
/// footprint row is held under. `ctest -R selftest.rows` checks the two agree.
inline constexpr std::size_t kSysmoduleHeap = 0x180000;

/// `selftest.ini`. No defaults for where to go: with no `url` there is no run,
/// for tlsprobe's reason -- a test with a compiled-in target can reach
/// something nobody chose, and the thing it must never reach is a production
/// RomM.
struct Config {
  std::string url;          ///< the disposable RomM, `https://host[:port]`
  std::string ca_file;      ///< optional PEM, as `[server] ca_file`
  std::int64_t rom_id = 0;  ///< a single-file homebrew rom on it, for the download row
  std::string bad_tls_url;  ///< an https URL whose handshake fails (the leak row)
  std::string blackhole_host = "10.255.255.1";  ///< routes nowhere (the timeout row)
  std::string stall_url;    ///< optional: accepts and never answers (the read timeout)
  std::string tls13_url;    ///< optional: a TLS 1.3-only listener
};

bool LoadConfig(const std::string& path, Config* config, std::string* why);

/// What `selftest.ini` should say, printed when it is missing.
extern const char* const kConfigTemplate;

enum class Outcome { kPass, kFail, kSkip, kNa };

/// The report, rewritten in full after every row: a run the console killed
/// halfway still leaves valid JSON on the card, whose missing rows the judge
/// names (`MISSING`) -- evidence of where it died rather than nothing.
class Report {
 public:
  Report(std::string path, std::string started, std::string version, std::string server);

  void Console(std::string hos, std::string ams, bool emummc, std::string sd_format);
  void Row(const char* id, Outcome result, const std::string& detail,
           const std::string& value_json = "");

  /// The rows so far as `pass/fail/skip/na` counts, for the screen.
  int count(Outcome result) const;
  const std::string& path() const { return path_; }

 private:
  void Flush();

  struct Line {
    std::string id;
    Outcome result;
    std::string detail;
    std::string value_json;
  };
  std::string path_;
  std::string started_;
  std::string version_;
  std::string server_;
  std::string hos_ = "unknown";
  std::string ams_ = "unknown";
  bool emummc_ = false;
  std::string sd_format_ = "unknown";
  std::vector<Line> rows_;
};

/// What the stages share. Filled in as they run: a later stage reads what an
/// earlier one learned (the card's format, the token) rather than asking again.
struct Run {
  Config config;
  Report* report = nullptr;
  PadState* pad = nullptr;
  std::unique_ptr<rommsync::fs::FileSystem> card;
  std::unique_ptr<rommsync::http::HttpClient> http;
  std::string scratch_sd;  ///< `/config/rommsync-selftest/scratch-<ts>`
  std::string sd_format = "unknown";
  std::optional<rommsync::auth::StoredToken> token;
  std::optional<rommsync::sync::Timestamp> server_date;
  bool network = false;
};

/// `io::SetFileSync` with the sysmodule's hook: `fsdevCommitDevice("sdmc:")`
/// after every atomic write, counted for the card.commit_device row.
void InstallCommitHook();

// The five stages, in #306's order.
void RunPlatform(Run& run);
void RunCard(Run& run);
void RunNetwork(Run& run);
void RunProtocol(Run& run);
void RunFootprint(Run& run);

/// One line on the screen. Everything the report says is also shown, so a run
/// someone watched and a report someone judged cannot tell different stories.
void Say(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// `sdmc:/config/rommsync-selftest/<leaf>`.
std::string UnderRoot(const std::string& leaf);

/// The network system clock, when the console has synchronised it.
std::optional<rommsync::sync::Timestamp> NetworkClock();

// The heap counters (heap_count.cpp): every C++ allocation this process makes,
// counted the way tests/count_new.hpp counts the host suite's.
struct HeapCount {
  std::size_t outstanding_bytes = 0;
  std::size_t peak_bytes = 0;
  std::uint64_t allocations = 0;
};
HeapCount HeapNow();
void HeapResetPeak();

}  // namespace selftest

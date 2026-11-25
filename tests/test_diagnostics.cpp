// M11-20 (#304): the diagnostics bundle a beta tester attaches to a bug report.
//
//   redaction   a token, a device_code, a user_code, a password and a
//               `user:password@` planted in config.ini and in the log reach
//               no byte of the bundle -- the acceptance line, proven
//   sections    versions, status, state.db's rows, backup usage (including
//               "not measured yet") and config.ini are all in it
//   log         the last `kLogLines` lines, across the rotated file and the
//               live one, oldest first; the in-memory tail when neither has any
//   crashes     only our program id's crash reports, newest first, bounded,
//               names and dates only
//   name        one file per press, named for the moment in UTC
//   program_id  `diag::kProgramId` is sys-rommsync.json's
//   missing     a card with no config, no state.db and no log still gets a bundle
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/diagnostics.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/ipc.hpp"
#include "scratch.hpp"

namespace {

using namespace rommsync;
namespace stdfs = std::filesystem;

// The secrets. Each one distinctive, so a bundle containing any part of one
// is a find rather than a coincidence.
constexpr const char* kToken = "tok3n-eyJhbGciOiJIUzI1NiJ9-SECRET";
constexpr const char* kDeviceCode = "devc0de-4f1a9b-SECRET";
constexpr const char* kUserCode = "USRC-ODE9";
constexpr const char* kPassword = "hunter2-SECRET";

stdfs::path Dir(const std::string& name) {
  const stdfs::path dir = stdfs::path(scratch::Dir()) / ("diagnostics-" + name);
  stdfs::remove_all(dir);
  stdfs::create_directories(dir);
  return dir;
}

void Put(const stdfs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string Slurp(const stdfs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

diag::Bundle BundleIn(const stdfs::path& dir) {
  diag::Bundle bundle;
  bundle.written_at = 1795000000;
  bundle.versions.overlay = "0.9.0";
  bundle.versions.overlay_interface = ipc::kVersion;
  bundle.versions.libultrahand = "v2.5.4";
  bundle.versions.sysmodule = "0.9.0";
  bundle.versions.libnx = "4.10.0-1";
  bundle.versions.hos = "19.0.1";
  bundle.versions.atmosphere = "not read: no spl: grant";
  bundle.config_path = (dir / "config.ini").string();
  bundle.state_path = (dir / "state.db").string();
  bundle.log_paths = {(dir / "rommsync.log.old").string(), (dir / "rommsync.log").string()};
  return bundle;
}

std::string WriteAndRead(checks::Checks& checks, const stdfs::path& dir,
                         const diag::Bundle& bundle) {
  const stdfs::path out = dir / diag::FileName(bundle.written_at);
  const io::WriteResult written = diag::Write(out.string(), bundle);
  checks.Expect(written.ok(), "the bundle is written: " + written.message);
  return Slurp(out);
}

void Redaction(checks::Checks& checks) {
  const stdfs::path dir = Dir("redaction");
  Put(dir / "config.ini",
      std::string("[server]\n"
                  "url = https://me:") + kPassword + "@romm.lan\n"
      "token = " + kToken + "\n"
      "api_token=" + kToken + "\n"
      "device_code = " + kDeviceCode + "\n"
      "password: " + kPassword + "\n"
      "[sync]\n"
      "enabled = true\n");
  Put(dir / "rommsync.log",
      std::string("1 info boot rommsync-nx/0.9.0\n") +
          "2 warn http GET /api/x Authorization: Bearer " + kToken + "\n" +
          "3 info auth device_code=" + kDeviceCode + "&user_code=" + kUserCode + "\n" +
          "4 info pair {\"device_code\":\"" + kDeviceCode + "\",\"user_code\":\"" + kUserCode +
          "\"}\n");
  diag::Bundle bundle = BundleIn(dir);
  bundle.memory_tail = {std::string("9 info token=") + kToken};
  const std::string text = WriteAndRead(checks, dir, bundle);

  for (const char* secret : {kToken, kDeviceCode, kUserCode, kPassword}) {
    checks.Expect(text.find(secret) == std::string::npos,
                  std::string("no byte of the bundle is the secret ") + secret);
  }
  checks.Expect(text.find("SECRET") == std::string::npos, "nor any part of one");
  checks.Expect(text.find("<redacted>") != std::string::npos,
                "a redacted value says so rather than vanishing");
  // What is not secret survives the redaction: triage needs it.
  checks.Expect(text.find("romm.lan") != std::string::npos, "the server's host is kept");
  checks.Expect(text.find("enabled = true") != std::string::npos,
                "an ordinary config line is kept as written");
  checks.Expect(text.find("boot rommsync-nx/0.9.0") != std::string::npos,
                "an ordinary log line is kept as written");

  checks.ExpectEq(diag::RedactConfigLine(std::string("refresh_token = ") + kToken),
                  std::string("refresh_token = <redacted>"), "a secret key's value is blanked");
  checks.ExpectEq(diag::RedactConfigLine("ca_file = /config/rommsync/ca.pem"),
                  std::string("ca_file = /config/rommsync/ca.pem"),
                  "and a key that only looks like one is not");
}

void Sections(checks::Checks& checks) {
  const stdfs::path dir = Dir("sections");
  Put(dir / "config.ini", "[sync]\nstates = true\n");
  Put(dir / "state.db",
      "rommsync-state 4 1790000000 https://romm.lan\nrow one\nrow two\nrow three\n");
  diag::Bundle bundle = BundleIn(dir);
  bundle.engine.auth = ipc::AuthState::kPaired;
  bundle.engine.queue_depth = 3;
  bundle.engine.last_sync_result = ipc::SyncResult::kPartial;
  bundle.configured = true;
  std::string text = WriteAndRead(checks, dir, bundle);

  for (const char* expected :
       {"overlay = 0.9.0", "libultrahand = v2.5.4", "sysmodule = 0.9.0", "libnx = 4.10.0-1",
        "hos = 19.0.1", "atmosphere = not read: no spl: grant", "emummc = unknown",
        "auth = paired", "queue_depth = 3", "last_sync_result = partial",
        "state.db = 3 rows", "states = true", "measured = no (not measured yet"}) {
    checks.Expect(text.find(expected) != std::string::npos,
                  std::string("the bundle says \"") + expected + "\"");
  }
  checks.Expect(text.find("ipc " + std::to_string(ipc::kVersion)) != std::string::npos,
                "with the contract each half speaks");

  bundle.engine.backup.measured = true;
  bundle.engine.backup.bytes = 5 * 1024 * 1024;
  bundle.engine.backup.backups = 12;
  bundle.engine.backup.kept_unique = 2;
  bundle.engine.backup.over_cap = true;
  bundle.engine.backup.state_budget_pruned = 3;
  bundle.engine.backup.state_orphans = 4;
  bundle.written_at += 1;
  text = WriteAndRead(checks, dir, bundle);
  for (const char* expected : {"measured = yes", "bytes = 5242880", "backups = 12",
                               "kept_unique = 2", "over_cap = yes",
                               "state_budget_pruned = 3", "state_orphans = 4"}) {
    checks.Expect(text.find(expected) != std::string::npos,
                  std::string("backup usage is #316's numbers: \"") + expected + "\"");
  }
  checks.Expect(text.find("at least") == std::string::npos,
                "a whole listing's numbers are not called a floor");

  // #323: a `.backup/` past one listing measured only what it listed.
  bundle.engine.backup.truncated = true;
  bundle.written_at += 1;
  text = WriteAndRead(checks, dir, bundle);
  for (const char* expected : {"bytes = at least 5242880", "backups = at least 12",
                               "kept_unique = at least 2", "truncated = yes",
                               "state_orphans = at least 4"}) {
    checks.Expect(text.find(expected) != std::string::npos,
                  std::string("a listing cut short says its numbers are a floor: \"") + expected +
                      "\"");
  }
}

void Log(checks::Checks& checks) {
  const stdfs::path dir = Dir("log");
  std::string old_lines;
  for (int line = 1; line <= 150; ++line) {
    old_lines += std::to_string(line) + " info old\n";
  }
  std::string live_lines;
  for (int line = 151; line <= 300; ++line) {
    live_lines += std::to_string(line) + " info live\n";
  }
  Put(dir / "rommsync.log.old", old_lines);
  Put(dir / "rommsync.log", live_lines);
  diag::Bundle bundle = BundleIn(dir);
  bundle.memory_tail = {"999 info only-in-memory"};
  const std::string text = WriteAndRead(checks, dir, bundle);

  checks.Expect(text.find("\n100 info old\n") == std::string::npos,
                "line 100 of 300 is older than the last 200");
  const std::size_t first = text.find("\n101 info old\n");
  const std::size_t rotated = text.find("\n151 info live\n");
  const std::size_t last = text.find("\n300 info live\n");
  checks.Expect(first != std::string::npos, "line 101 is the first of the last 200");
  checks.Expect(first < rotated && rotated < last,
                "oldest first, across the rotated file into the live one");
  checks.Expect(text.find("only-in-memory") == std::string::npos,
                "the in-memory tail is not written when the files have lines");

  const stdfs::path bare = Dir("log-memory");
  diag::Bundle fallback = BundleIn(bare);
  fallback.memory_tail = {"7 info only-in-memory"};
  const std::string from_memory = WriteAndRead(checks, bare, fallback);
  checks.Expect(from_memory.find("7 info only-in-memory") != std::string::npos,
                "with no log file, the in-memory tail is what is attached");
}

void Crashes(checks::Checks& checks) {
  fs::Listing listing;
  // Atmosphère's `%011lu_%016lx.log`: time, then the program that crashed.
  listing.entries.push_back({"01795000000_4200000000524d53.log", false, 900, 0});
  listing.entries.push_back({"01795000300_0100000000001000.log", false, 900, 0});
  listing.entries.push_back({"01795000600_4200000000524D53.log", false, 900, 0});
  listing.entries.push_back({"00000004242_4200000000524d53.log", false, 900, 0});
  listing.entries.push_back({"dumps", true, 0, 0});
  listing.entries.push_back({"notes.txt", false, 10, 0});
  const std::vector<diag::CrashReport> ours = diag::OurCrashReports(listing);
  checks.ExpectEq(ours.size(), std::size_t{3}, "only our program id's reports are listed");
  if (ours.size() == 3) {
    checks.ExpectEq(ours[0].name, std::string("01795000600_4200000000524D53.log"),
                    "newest first, whatever case the id is in");
    checks.ExpectEq(ours[0].at, std::int64_t{1795000600}, "dated by its name");
    checks.ExpectEq(ours[2].at, std::int64_t{0},
                    "a tick count is not a date, and is not passed off as one");
  }

  fs::Listing many;
  for (int report = 0; report < 40; ++report) {
    many.entries.push_back(
        {"0179500" + std::to_string(1000 + report) + "_4200000000524d53.log", false, 1, 0});
  }
  checks.ExpectEq(diag::OurCrashReports(many).size(), diag::kMaxCrashReports,
                  "and at most kMaxCrashReports of them");

  const stdfs::path dir = Dir("crashes");
  diag::Bundle bundle = BundleIn(dir);
  bundle.crashes = ours;
  const std::string text = WriteAndRead(checks, dir, bundle);
  checks.Expect(text.find("01795000600_4200000000524D53.log") != std::string::npos,
                "a report is named in the bundle");
  checks.Expect(text.find("2026-11-18") != std::string::npos, "with its date");
  checks.Expect(text.find("0100000000001000") == std::string::npos,
                "and someone else's crash is not");
}

void Name(checks::Checks& checks) {
  checks.ExpectEq(diag::FileName(1795000000), std::string("diagnostics-20261118-110640.txt"),
                  "named for the moment, in UTC");
  checks.Expect(diag::FileName(1795000000) != diag::FileName(1795000001),
                "a second press a second later is a second file");
  checks.ExpectEq(diag::FileName(-5), std::string("diagnostics-unknown-time.txt"),
                  "a clock before 1970 is said, not spelled");
}

void ProgramId(checks::Checks& checks) {
  const std::string json = Slurp(stdfs::path(ROMMSYNC_SOURCE_DIR) / "sysmodule" /
                                 "sys-rommsync.json");
  std::ostringstream expected;
  expected << "\"program_id\": \"0x" << std::hex << std::uppercase << diag::kProgramId << "\"";
  checks.Expect(json.find(expected.str()) != std::string::npos,
                "diag::kProgramId is the npdm's program_id: " + expected.str());
}

void Missing(checks::Checks& checks) {
  const stdfs::path dir = Dir("missing");
  const diag::Bundle bundle = BundleIn(dir);
  const std::string text = WriteAndRead(checks, dir, bundle);
  checks.Expect(text.find("state.db = missing") != std::string::npos,
                "a card with no state.db says so");
  checks.Expect(text.find("config.ini: missing") != std::string::npos,
                "and one with no config.ini");
  checks.Expect(text.find("[log]") != std::string::npos, "and still has a log section");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  checks::Checks checks;
  if (scenario == "redaction") {
    Redaction(checks);
  } else if (scenario == "sections") {
    Sections(checks);
  } else if (scenario == "log") {
    Log(checks);
  } else if (scenario == "crashes") {
    Crashes(checks);
  } else if (scenario == "name") {
    Name(checks);
  } else if (scenario == "program_id") {
    ProgramId(checks);
  } else if (scenario == "missing") {
    Missing(checks);
  } else {
    std::cerr << "unknown scenario " << scenario << "\n";
    return 2;
  }
  if (checks.failures() == 0) std::cout << "diagnostics." << scenario << " ok\n";
  return checks.failures() == 0 ? 0 : 1;
}

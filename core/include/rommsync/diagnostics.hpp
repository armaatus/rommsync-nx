// The diagnostics bundle: one file a beta tester attaches to a bug report
// (M11-20, #304).
//
// In the Switch scene a file the user attaches *is* the telemetry -- there is
// no other channel, and this client sends nothing anywhere but the user's own
// RomM (docs/PRIVACY.md). So the overlay's log screen has a "Write diagnostics"
// press, the sysmodule answers it with `ipc::Command::kWriteDiagnostics`, and
// what lands on the card is written here: versions, the state of the engine,
// `config.ini`, the `state.db` row count, `.backup/` usage, the end of the log
// and the Atmosphère crash reports that are ours. Nothing leaves the console;
// the user decides whether to attach it.
//
// **Redaction is the one property that matters**, and it is applied to every
// line on its way out rather than at the places that might carry a secret:
// `log::Redact` takes out a bearer token, a `device_code`, a `user_code` and a
// `user:password@`, and `RedactConfigLine` additionally blanks the value of any
// `config.ini` key whose name says secret. `token.dat` and `device.dat` are
// never opened. `ctest -R diagnostics` proves it with each secret planted.
//
// **Streamed, never held whole.** The sysmodule heap's table has 4 KiB of slack
// (`sysmodule/source/main.cpp`), and the bundle is up to `kLogLines` log lines
// plus a config file the user wrote. So it is written through
// `io::WriteAtomically`'s piecewise form: the short header is one piece, and
// `config.ini`, `state.db` and the log are read a line at a time from the card,
// each line one piece. What is in memory at once is a line, not the bundle.
//
// Hard rule 4: standard headers only. The platform's facts -- the firmware
// version, Atmosphère's, the crash-report listing -- arrive as strings and a
// `fs::Listing`, read by the sysmodule.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/ipc.hpp"

namespace rommsync::diag {

/// sys-rommsync's program id: `program_id` in `sysmodule/sys-rommsync.json`,
/// which `ctest -R diagnostics.program_id` holds this to. Atmosphère names a
/// crash report after the program that crashed, and this is how the bundle
/// picks ours out of everyone's.
inline constexpr std::uint64_t kProgramId = 0x4200000000524D53ULL;

/// Where Atmosphère's creport writes, as an SD-root path.
inline constexpr const char* kCrashReportsSdDir = "/atmosphere/crash_reports";

/// How many of the log's last lines the bundle carries. The issue's number:
/// several ticks' worth at real line lengths, and well under the two files a
/// full rotation leaves (`log::kMaxFileBytes` each).
inline constexpr std::size_t kLogLines = 200;

/// The newest this many of our crash reports are listed. A console that crashed
/// more often than this has told triage what it needs to know.
inline constexpr std::size_t kMaxCrashReports = 16;

/// How many lines of `config.ini` are copied. `config::kMaxConfigBytes` bounds
/// the file the parser reads; this bounds the copy, which reads it again.
inline constexpr std::size_t kMaxConfigLines = 512;

/// The longest line copied from any file, in bytes; the rest of a longer one is
/// dropped and the line marked. `log::kMaxLineBytes` plus room for a config
/// line that quotes a long path.
inline constexpr std::size_t kMaxCopiedLineBytes = 512;

/// The bundle's file name for a moment: `diagnostics-YYYYMMDD-HHMMSS.txt`, in
/// UTC. One file per press, so a second press never replaces the bundle a user
/// already attached. A moment no clock can spell -- before 1970 -- is
/// `diagnostics-unknown-time.txt`.
std::string FileName(std::int64_t unix_seconds);

/// Which build each half is, and the platform under them. Empty is "not known"
/// and is written as `unknown`, never left blank.
struct Versions {
  /// The overlay's, as it sent them with the request: its `rommsync::version()`
  /// and its `ipc::kVersion`, so a mismatched pair on one card is visible.
  std::string overlay;
  std::uint32_t overlay_interface = 0;
  std::string libultrahand;

  std::string sysmodule;
  std::uint32_t interface = ipc::kVersion;
  std::string libnx;

  /// Horizon's firmware, from `setsysGetFirmwareVersion`.
  std::string hos;

  /// Atmosphère's version and whether this is emuMMC -- or, when the sysmodule
  /// cannot read them, the sentence saying why.
  std::string atmosphere;
  std::string emummc;
};

/// One Atmosphère crash report: the file name and when it was written. Only
/// those, never the contents, which carry memory dumps.
struct CrashReport {
  std::string name;

  /// Whole Unix seconds, from the name's leading number. `0` when that number
  /// is not a plausible date -- creport falls back to a tick count when the
  /// console has no clock -- and the name alone is written.
  std::int64_t at = 0;
};

/// The entries of `listing` that are our crash reports, newest first, at most
/// `kMaxCrashReports`. Atmosphère names them `<time>_<program id>.log`.
std::vector<CrashReport> OurCrashReports(const fs::Listing& listing,
                                         std::uint64_t program_id = kProgramId);

/// `line` of `config.ini` with anything secret taken out: `log::Redact`, and
/// the value of any `key = value` whose key names a token, code, password,
/// secret or authorization. `config.ini` holds none by design -- the token is
/// `token.dat` -- but it is a file the user types into.
std::string RedactConfigLine(std::string_view line);

/// Everything the bundle says. The paths are opened by `Write`, a line at a
/// time, and a missing one is written as missing rather than refused.
struct Bundle {
  /// When the press was answered, Unix seconds, for the header.
  std::int64_t written_at = 0;

  Versions versions;

  /// The engine as `GetStatus` reads it, plus the three `Status` fields the
  /// snapshot does not carry.
  ipc::EngineSnapshot engine;
  bool enabled = false;
  bool configured = false;
  std::int64_t config_errors = 0;

  /// Paths as `std::fopen` takes them: `sdmc:/config/rommsync/...` on the
  /// console.
  std::string config_path;
  std::string state_path;

  /// The log, oldest file first: `rommsync.log.old`, then `rommsync.log`.
  std::vector<std::string> log_paths;

  /// The log's in-memory tail (`log::Tail`), written when no log file had a
  /// line -- a card the sink could not write to still has a log to attach.
  std::vector<std::string> memory_tail;

  std::vector<CrashReport> crashes;

  /// Why the crash reports could not be listed, when they could not. Empty
  /// otherwise, including for a console with no crash-report directory.
  std::string crash_note;
};

/// Write `bundle` to `path`, atomically, streamed a line at a time. Every line
/// goes through `log::Redact`; config lines through `RedactConfigLine` first.
io::WriteResult Write(const std::string& path, const Bundle& bundle);

}  // namespace rommsync::diag

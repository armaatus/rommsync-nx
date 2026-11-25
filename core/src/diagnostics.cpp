#include "rommsync/diagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/log.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::diag {
namespace {

struct FileCloser {
  void operator()(std::FILE* file) const { std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

/// Open for reading, and say why not: a missing file and one the card refused
/// send a reader to different places.
File Open(const std::string& path, std::string* why) {
  errno = 0;
  File file(std::fopen(path.c_str(), "rb"));
  if (!file && why != nullptr) {
    *why = (errno == ENOENT || errno == ENOTDIR) ? "missing" : "unreadable";
  }
  return file;
}

/// The next line of `file` into `line`, without its terminator. False at the
/// end. A line past `kMaxCopiedLineBytes` keeps its start and says it was cut,
/// so one runaway line costs a bounded buffer rather than the heap.
bool ReadLine(std::FILE* file, std::string* line) {
  line->clear();
  bool any = false;
  bool cut = false;
  int c = 0;
  while ((c = std::getc(file)) != EOF) {
    any = true;
    if (c == '\n') {
      break;
    }
    if (c == '\r') {
      continue;
    }
    if (line->size() < kMaxCopiedLineBytes) {
      line->push_back(static_cast<char>(c));
    } else {
      cut = true;
    }
  }
  if (cut) {
    line->append(" [cut]");
  }
  return any;
}

std::string OrUnknown(const std::string& text) { return text.empty() ? "unknown" : text; }

const char* YesNo(bool value) { return value ? "yes" : "no"; }

/// `unix_seconds` as RFC 3339, or empty for a moment no clock spells.
std::string Date(std::int64_t unix_seconds) {
  if (unix_seconds < sync::kMinTimestampSeconds) {
    return {};
  }
  return sync::FormatTimestamp(sync::Timestamp(std::chrono::seconds(unix_seconds)));
}

void Line(std::string* out, std::string_view key, std::string_view value) {
  out->append(key);
  out->append(" = ");
  out->append(value);
  out->push_back('\n');
}

void Line(std::string* out, std::string_view key, std::int64_t value) {
  Line(out, key, std::to_string(value));
}

/// `state.db`'s rows and bytes, counted a line at a time: a baseline at its
/// bound is `kHeapStateBaseline`, and loading one here would be a second.
std::string StateSummary(const std::string& path) {
  std::string why;
  const File file = Open(path, &why);
  if (!file) {
    return why;
  }
  std::int64_t lines = 0;
  std::int64_t bytes = 0;
  std::string line;
  while (ReadLine(file.get(), &line)) {
    bytes += static_cast<std::int64_t>(line.size()) + 1;
    if (!line.empty()) {
      ++lines;
    }
  }
  // The first line is the header (`rommsync-state <version> ...`), not a row.
  const std::int64_t rows = lines > 0 ? lines - 1 : 0;
  return std::to_string(rows) + (rows == 1 ? " row, " : " rows, ") + std::to_string(bytes) +
         " bytes";
}

/// Everything before `config.ini` and the log: short, bounded by the overlay's
/// strings (`ipc::DecodeDiagnosticsRequest`) and `kMaxCrashReports`.
std::string Header(const Bundle& bundle) {
  std::string out;
  out += "rommsync-nx diagnostics\n";
  const std::string written = Date(bundle.written_at);
  Line(&out, "written", written.empty() ? "unknown" : written);
  out +=
      "Attach this file to a bug report. Tokens, pairing codes and passwords are\n"
      "replaced by <redacted>; token.dat and device.dat are never read.\n";

  const Versions& v = bundle.versions;
  out += "\n[versions]\n";
  Line(&out, "overlay",
       OrUnknown(v.overlay) + " (ipc " +
           (v.overlay_interface == 0 ? std::string("unknown") : std::to_string(v.overlay_interface)) +
           ")");
  Line(&out, "libultrahand", OrUnknown(v.libultrahand));
  Line(&out, "sysmodule", OrUnknown(v.sysmodule) + " (ipc " + std::to_string(v.interface) + ")");
  Line(&out, "libnx", OrUnknown(v.libnx));
  Line(&out, "hos", OrUnknown(v.hos));
  Line(&out, "atmosphere", OrUnknown(v.atmosphere));
  Line(&out, "emummc", OrUnknown(v.emummc));

  const ipc::EngineSnapshot& e = bundle.engine;
  out += "\n[status]\n";
  Line(&out, "auth", ipc::ToString(e.auth));
  Line(&out, "enabled", YesNo(bundle.enabled));
  Line(&out, "configured", YesNo(bundle.configured));
  Line(&out, "config_errors", bundle.config_errors);
  Line(&out, "online", YesNo(e.online));
  const std::string last = Date(e.last_sync_at);
  Line(&out, "last_sync_at", last.empty() ? "never" : last);
  Line(&out, "last_sync_result", ipc::ToString(e.last_sync_result));
  Line(&out, "sync_in_progress", YesNo(e.sync_in_progress));
  Line(&out, "uploaded", e.uploaded);
  Line(&out, "downloaded", e.downloaded);
  Line(&out, "conflicts", e.conflicts);
  Line(&out, "failed", e.failed);
  Line(&out, "queue_depth", e.queue_depth);
  Line(&out, "download", ipc::ToString(e.download.state));
  Line(&out, "server_version", OrUnknown(e.server_version));
  Line(&out, "server_supported", YesNo(e.server_supported));
  Line(&out, "clock_trusted", YesNo(e.clock_trusted));
  Line(&out, "clock_skew_seconds", e.clock_skew_seconds);

  out += "\n[state]\n";
  Line(&out, "state.db", StateSummary(bundle.state_path));

  // #316's numbers, never a second walk of `.backup/`: "not measured yet" is a
  // finding of its own -- no prune has finished since boot.
  const ipc::BackupUsage& b = e.backup;
  out += "\n[backup]\n";
  if (!b.measured) {
    Line(&out, "measured", "no (not measured yet: no prune has finished since boot)");
  } else {
    Line(&out, "measured", "yes");
    // A listing cut short counted only what it listed (#323): each count is a
    // floor, and says so on its own line rather than only in `truncated`.
    const std::string at_least = b.truncated ? "at least " : "";
    Line(&out, "bytes", at_least + std::to_string(b.bytes));
    Line(&out, "backups", at_least + std::to_string(b.backups));
    Line(&out, "kept_unique", at_least + std::to_string(b.kept_unique));
    Line(&out, "over_cap", YesNo(b.over_cap));
    Line(&out, "truncated", YesNo(b.truncated));
    // Since boot, and never a floor: it counts removals, not what a listing saw.
    Line(&out, "state_budget_pruned", b.state_budget_pruned);
  }

  out += "\n[crash_reports]\n";
  if (!bundle.crash_note.empty()) {
    out += bundle.crash_note + "\n";
  } else if (bundle.crashes.empty()) {
    out += "none from sys-rommsync\n";
  }
  for (const CrashReport& report : bundle.crashes) {
    const std::string at = Date(report.at);
    out += report.name + "  " + (at.empty() ? "undated" : at) + "\n";
  }
  return out;
}

/// `value` in decimal, zero-padded to `width`.
void AppendDigits(std::string* out, std::int64_t value, std::size_t width) {
  std::string digits = std::to_string(value);
  if (digits.size() < width) {
    out->append(width - digits.size(), '0');
  }
  out->append(digits);
}

bool IsHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

/// The pieces of the bundle, one per call: the header, then `config.ini` a line
/// at a time, then the log a line at a time. `io::WriteAtomically` asks for the
/// next until this says there is none.
class Pieces {
 public:
  explicit Pieces(const Bundle& bundle) : bundle_(bundle) {}

  bool Next(std::string* piece) {
    while (phase_ != Phase::kDone) {
      if (Step(piece)) {
        // Every line, whichever file it came from: the one rule that keeps a
        // secret out does not depend on knowing where secrets are.
        *piece = log::Redact(*piece);
        return true;
      }
    }
    return false;
  }

 private:
  enum class Phase { kHeader, kConfigOpen, kConfig, kLogOpen, kLog, kMemory, kDone };

  /// Produce a piece and return true, or move to the next phase and return false.
  bool Step(std::string* piece) {
    switch (phase_) {
      case Phase::kHeader:
        *piece = Header(bundle_);
        phase_ = Phase::kConfigOpen;
        return true;
      case Phase::kConfigOpen: {
        std::string why;
        file_ = Open(bundle_.config_path, &why);
        phase_ = file_ ? Phase::kConfig : Phase::kLogOpen;
        *piece = "\n[config.ini]\n";
        if (!file_) {
          *piece += "config.ini: " + why + " (the built-in defaults are in force)\n";
        }
        return true;
      }
      case Phase::kConfig:
        return ConfigLine(piece);
      case Phase::kLogOpen:
        return LogOpen(piece);
      case Phase::kLog:
        return LogLine(piece);
      case Phase::kMemory:
        if (memory_at_ < bundle_.memory_tail.size()) {
          *piece = bundle_.memory_tail[memory_at_++] + "\n";
          return true;
        }
        phase_ = Phase::kDone;
        return false;
      case Phase::kDone:
        return false;
    }
    return false;
  }

  bool ConfigLine(std::string* piece) {
    if (config_lines_ < kMaxConfigLines && ReadLine(file_.get(), &line_)) {
      ++config_lines_;
      *piece = RedactConfigLine(line_) + "\n";
      return true;
    }
    const bool more = config_lines_ >= kMaxConfigLines && ReadLine(file_.get(), &line_);
    file_.reset();
    phase_ = Phase::kLogOpen;
    if (more) {
      *piece = "[the rest of config.ini is not copied]\n";
      return true;
    }
    return false;
  }

  /// Count the log's lines first, a file at a time, so the copy can start at
  /// the right one without holding any of them. The logger may append -- or
  /// rotate -- between the count and the copy; the copy then starts a few lines
  /// early or late, which a bug report survives and a second buffer would not.
  bool LogOpen(std::string* piece) {
    std::int64_t total = 0;
    std::string line;
    for (const std::string& path : bundle_.log_paths) {
      const File file = Open(path, nullptr);
      while (file && ReadLine(file.get(), &line)) {
        ++total;
      }
    }
    skip_ = total > static_cast<std::int64_t>(kLogLines)
                ? total - static_cast<std::int64_t>(kLogLines)
                : 0;
    *piece = "\n[log]\n";
    if (total == 0) {
      *piece += "no log file on the card; the sysmodule's in-memory tail follows\n";
      phase_ = Phase::kMemory;
      return true;
    }
    *piece += "the last " + std::to_string(total - skip_) + " of " + std::to_string(total) +
              " lines on the card\n";
    path_at_ = 0;
    phase_ = Phase::kLog;
    return true;
  }

  bool LogLine(std::string* piece) {
    while (true) {
      if (!file_) {
        if (path_at_ >= bundle_.log_paths.size()) {
          phase_ = Phase::kDone;
          return false;
        }
        file_ = Open(bundle_.log_paths[path_at_++], nullptr);
        continue;
      }
      if (!ReadLine(file_.get(), &line_)) {
        file_.reset();
        continue;
      }
      if (skip_ > 0) {
        --skip_;
        continue;
      }
      *piece = line_ + "\n";
      return true;
    }
  }

  const Bundle& bundle_;
  Phase phase_ = Phase::kHeader;
  File file_;
  std::string line_;
  std::size_t config_lines_ = 0;
  std::int64_t skip_ = 0;
  std::size_t path_at_ = 0;
  std::size_t memory_at_ = 0;
};

}  // namespace

std::string FileName(std::int64_t unix_seconds) {
  if (unix_seconds < 0) {
    return "diagnostics-unknown-time.txt";
  }
  // Civil from days (Hinnant), so the name needs neither `gmtime` -- not
  // reentrant, and the IPC thread is not the only one with a clock -- nor a
  // range check `FormatTimestamp` would impose.
  const std::int64_t days = unix_seconds / 86400;
  const std::int64_t rest = unix_seconds % 86400;
  const std::int64_t z = days + 719468;
  const std::int64_t era = z / 146097;
  const std::int64_t doe = z - era * 146097;
  const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int64_t mp = (5 * doy + 2) / 153;
  const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  // Digits by hand rather than `snprintf`: that pulls newlib's whole
  // `_svfprintf_r` into the sysmodule, which `switch.builds` refuses.
  std::string name = "diagnostics-";
  AppendDigits(&name, year, 4);
  AppendDigits(&name, month, 2);
  AppendDigits(&name, day, 2);
  name += '-';
  AppendDigits(&name, rest / 3600, 2);
  AppendDigits(&name, rest % 3600 / 60, 2);
  AppendDigits(&name, rest % 60, 2);
  return name + ".txt";
}

std::vector<CrashReport> OurCrashReports(const fs::Listing& listing, std::uint64_t program_id) {
  char ours[16];
  for (std::size_t at = 0; at < 16; ++at) {
    ours[at] = "0123456789abcdef"[(program_id >> (4 * (15 - at))) & 0xF];
  }
  std::vector<CrashReport> reports;
  for (const fs::Entry& entry : listing.entries) {
    const std::string& name = entry.name;
    // `<digits>_<16 hex>.log`, and nothing else: creport's own spelling.
    const std::size_t underscore = name.find('_');
    if (entry.is_directory || underscore == std::string::npos || underscore == 0 ||
        name.size() != underscore + 1 + 16 + 4 || name.compare(name.size() - 4, 4, ".log") != 0) {
      continue;
    }
    bool match = true;
    for (std::size_t at = 0; at < 16; ++at) {
      const char c = name[underscore + 1 + at];
      match = match && IsHex(c) &&
              std::tolower(static_cast<unsigned char>(c)) == static_cast<unsigned char>(ours[at]);
    }
    std::int64_t when = 0;
    for (std::size_t at = 0; at < underscore && match; ++at) {
      if (!std::isdigit(static_cast<unsigned char>(name[at])) || when > 99999999999LL) {
        match = false;
        break;
      }
      when = when * 10 + (name[at] - '0');
    }
    if (!match) {
      continue;
    }
    // creport writes Unix seconds when the console has a clock and a tick count
    // when it has not. Before 2018 -- older than any firmware Atmosphère runs on
    // with this sysmodule -- is the second, and is not passed off as a date.
    constexpr std::int64_t kPlausible = 1514764800;  // 2018-01-01
    reports.push_back({name, when >= kPlausible ? when : 0});
  }
  std::sort(reports.begin(), reports.end(), [](const CrashReport& a, const CrashReport& b) {
    return a.at != b.at ? a.at > b.at : a.name > b.name;
  });
  if (reports.size() > kMaxCrashReports) {
    reports.resize(kMaxCrashReports);
  }
  return reports;
}

std::string RedactConfigLine(std::string_view line) {
  const std::size_t sep = line.find_first_of("=:");
  if (sep != std::string_view::npos) {
    std::string key;
    for (const char c : line.substr(0, sep)) {
      key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const char* secret : {"token", "password", "passwd", "secret", "authorization",
                               "device_code", "user_code", "api_key", "apikey"}) {
      if (key.find(secret) != std::string::npos) {
        std::string out(line.substr(0, sep + 1));
        if (sep + 1 < line.size() && line[sep + 1] == ' ') {
          out.push_back(' ');
        }
        return out + log::kRedacted;
      }
    }
  }
  return log::Redact(line);
}

io::WriteResult Write(const std::string& path, const Bundle& bundle) {
  Pieces pieces(bundle);
  return io::WriteAtomically(path, [&pieces](std::string* piece) { return pieces.Next(piece); });
}

}  // namespace rommsync::diag

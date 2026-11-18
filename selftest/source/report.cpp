// The report and selftest.ini: the two files this program and a person share.
//
// The report's schema is scripts/selftest-judge.py's to define, and
// selftest/README.md's to describe; what is written here is that schema, at
// version 1.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rommsync/atomic_file.hpp"
#include "rommsync/json.hpp"
#include "selftest.hpp"

namespace selftest {

const char* const kConfigTemplate =
    "# sdmc:/config/rommsync-selftest/selftest.ini -- see selftest/README.md\n"
    "url = https://romm.example.lan       # a DISPOSABLE RomM, never your own\n"
    "ca_file = sdmc:/config/rommsync-selftest/ca.pem   # only for a private CA\n"
    "rom_id = 12                          # a single-file homebrew rom on it\n"
    "bad_tls_url = https://192.168.1.20/  # same server by IP: the name check fails\n"
    "# blackhole_host = 10.255.255.1      # an address that routes nowhere\n"
    "# stall_url = https://192.168.1.20:9443/   # accepts, never answers\n"
    "# tls13_url = https://romm-tls13.example.lan/\n";

namespace {

const char* Name(Outcome result) {
  switch (result) {
    case Outcome::kPass:
      return "pass";
    case Outcome::kFail:
      return "fail";
    case Outcome::kSkip:
      return "skip";
    case Outcome::kNa:
      return "na";
  }
  return "fail";
}

std::string Trim(std::string text) {
  const std::size_t comment = text.find_first_of("#;");
  if (comment != std::string::npos) text.erase(comment);
  const char* space = " \t\r\n";
  const std::size_t first = text.find_first_not_of(space);
  if (first == std::string::npos) return "";
  return text.substr(first, text.find_last_not_of(space) - first + 1);
}

}  // namespace

std::string Describe(const rommsync::http::Result& result) {
  if (result.ok()) return "HTTP " + Number(result.response.status);
  return std::string(rommsync::http::ToString(result.error)) +
         (result.message.empty() ? "" : ": " + result.message);
}

std::string UnderRoot(const std::string& leaf) { return std::string(kRoot) + leaf; }

void Say(const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::printf("%s\n", text);
  consoleUpdate(nullptr);
}

/// `key = value`, one a line -- tlsprobe's format, not config.ini's schema:
/// this file names a test, not a client.
bool LoadConfig(const std::string& path, Config* config, std::string* why) {
  const rommsync::io::ReadResult read = rommsync::io::ReadFile(path);
  if (!read.ok()) {
    *why = "no " + path;
    return false;
  }
  std::size_t start = 0;
  const std::string& text = read.contents;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    if (key.empty() || value.empty()) continue;
    if (key == "url") {
      config->url = value;
      while (!config->url.empty() && config->url.back() == '/') config->url.pop_back();
    } else if (key == "ca_file") {
      config->ca_file = value;
    } else if (key == "rom_id") {
      config->rom_id = std::strtoll(value.c_str(), nullptr, 10);
    } else if (key == "bad_tls_url") {
      config->bad_tls_url = value;
    } else if (key == "blackhole_host") {
      config->blackhole_host = value;
    } else if (key == "stall_url") {
      config->stall_url = value;
    } else if (key == "tls13_url") {
      config->tls13_url = value;
    }
  }
  if (config->url.rfind("https://", 0) != 0) {
    *why = "url must be an https:// RomM -- the TLS rows are half of what this tests";
    return false;
  }
  if (config->rom_id <= 0) {
    *why = "rom_id must name a rom on that RomM";
    return false;
  }
  if (config->bad_tls_url.rfind("https://", 0) != 0) {
    *why = "bad_tls_url must be an https:// URL whose handshake fails";
    return false;
  }
  return true;
}

Report::Report(std::string path, std::string started, std::string version, std::string server)
    : path_(std::move(path)),
      started_(std::move(started)),
      version_(std::move(version)),
      server_(std::move(server)) {}

void Report::Console(std::string hos, std::string ams, bool emummc, std::string sd_format) {
  hos_ = std::move(hos);
  ams_ = std::move(ams);
  emummc_ = emummc;
  sd_format_ = std::move(sd_format);
  Flush();
}

void Report::Row(const char* id, Outcome result, const std::string& detail,
                 const std::string& value_json) {
  rows_.push_back({id, result, detail, value_json});
  const char* mark = result == Outcome::kPass   ? "PASS"
                     : result == Outcome::kFail ? "FAIL"
                     : result == Outcome::kSkip ? "skip"
                                               : " n/a";
  Say("[%s] %-22s %s", mark, id, detail.c_str());
  Flush();
}

int Report::count(Outcome result) const {
  int n = 0;
  for (const Line& line : rows_) n += line.result == result ? 1 : 0;
  return n;
}

void Report::Flush() {
  using rommsync::json::Quote;
  std::string out = "{\n  \"schema\": 1,\n  \"tool\": \"rommsync-selftest\",\n";
  out += "  \"version\": " + Quote(version_) + ",\n";
  out += "  \"started\": " + Quote(started_) + ",\n";
  out += "  \"console\": {\"hos\": " + Quote(hos_) + ", \"ams\": " + Quote(ams_) +
         ", \"emummc\": " + (emummc_ ? "true" : "false") + ", \"sd_format\": " +
         Quote(sd_format_) + "},\n";
  out += "  \"server\": " + Quote(server_) + ",\n  \"rows\": [";
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const Line& line = rows_[i];
    out += i == 0 ? "\n" : ",\n";
    out += "    {\"id\": " + Quote(line.id) + ", \"result\": " + Quote(Name(line.result)) +
           ", \"detail\": " + Quote(line.detail);
    if (!line.value_json.empty()) out += ", \"value\": " + line.value_json;
    out += "}";
  }
  out += "\n  ]\n}\n";
  // Atomically, through the same commit hook the sysmodule installs: a report
  // half-written by a power cut would be a MALFORMED one, which says less than
  // the last whole version did.
  const rommsync::io::WriteResult written = rommsync::io::WriteAtomically(path_, out);
  if (!written.ok()) Say("!! could not write %s: %s", path_.c_str(), written.message.c_str());
}

}  // namespace selftest

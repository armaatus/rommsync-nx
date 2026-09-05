// rommsync-tlsprobe -- the M0-1 spike, as a homebrew .nro someone launches by
// hand and closes again.
//
// It reads sdmc:/switch/rommsync-tlsprobe.ini, does exactly one HTTPS GET over
// the Horizon `ssl` service, prints what it cost, and writes the same report to
// sdmc:/switch/rommsync-tlsprobe.log so it can be read off an SD card or out of
// an emulator's virtual one.
//
// Two deliberate refusals, both about CLAUDE.md hard rule 1:
//
//   * No compiled-in target. With no ini this prints the ini it wants and
//     stops. A probe with a default host is a probe that can reach something
//     nobody chose, and the thing it must never reach is a production RomM.
//   * No retry, no loop, no persistence. One run per launch, then the console
//     waits for + and exits. Nothing here is installed and nothing survives.

#include <switch.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "probe.hpp"
#include "rommsync/version.hpp"

namespace {

constexpr const char* kConfigPath = "sdmc:/switch/rommsync-tlsprobe.ini";
constexpr const char* kLogPath = "sdmc:/switch/rommsync-tlsprobe.log";

// What to write into the ini, printed when there isn't one. The values are the
// docker fixture's shape (docs/TESTING.md): a loopback-only RomM behind a TLS
// terminator with a certificate nothing trusts, which is why ca_pem exists.
constexpr const char* kConfigTemplate =
    "# sdmc:/switch/rommsync-tlsprobe.ini\n"
    "# `./scripts/orca/tls-fixture.sh ini` prints this filled in.\n"
    "host = 127.0.0.1           # the fixture binds loopback; see the README\n"
    "port = 25000               # TLS_PORT from the worktree's .env\n"
    "path = /api/heartbeat\n"
    "sni = romm.fixture.local   # the name on the fixture certificate\n"
    "ca_pem = sdmc:/switch/rommsync-fixture-ca.pem\n"
    "verify = 1\n";

bool ParseBool(const char* value) {
  return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' ||
         value[0] == 'Y';
}

void CopyValue(char* dest, size_t size, const char* value) {
  std::snprintf(dest, size, "%s", value);
}

/// Trim ASCII space and a trailing `#`/`;` comment, in place.
char* Trim(char* text) {
  while (*text == ' ' || *text == '\t') ++text;
  char* comment = text;
  while (*comment != '\0' && *comment != '#' && *comment != ';') ++comment;
  *comment = '\0';
  char* end = comment;
  while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                        end[-1] == '\n')) {
    --end;
  }
  *end = '\0';
  return text;
}

/// `key = value`, one per line. Not core/'s config parser: that one parses
/// config.ini's schema and lives behind an interface this target does not
/// compile in (tlsprobe/Makefile). Twenty lines here beats linking the engine
/// into a measurement.
bool LoadConfig(tlsprobe::Config& config) {
  FILE* file = fopen(kConfigPath, "r");
  if (file == nullptr) return false;

  char line[512];
  while (fgets(line, sizeof(line), file) != nullptr) {
    char* separator = std::strchr(line, '=');
    if (separator == nullptr) continue;
    *separator = '\0';
    char* key = Trim(line);
    char* value = Trim(separator + 1);
    if (key[0] == '\0' || value[0] == '\0') continue;

    if (std::strcmp(key, "host") == 0) {
      CopyValue(config.host, sizeof(config.host), value);
    } else if (std::strcmp(key, "sni") == 0) {
      CopyValue(config.sni, sizeof(config.sni), value);
    } else if (std::strcmp(key, "path") == 0) {
      CopyValue(config.path, sizeof(config.path), value);
    } else if (std::strcmp(key, "ca_pem") == 0) {
      CopyValue(config.ca_path, sizeof(config.ca_path), value);
    } else if (std::strcmp(key, "port") == 0) {
      config.port = static_cast<u16>(std::atoi(value));
    } else if (std::strcmp(key, "verify") == 0) {
      config.verify = ParseBool(value);
    } else if (std::strcmp(key, "read_buf") == 0) {
      config.read_buf_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "connect_timeout_ms") == 0) {
      config.connect_timeout_ms = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "stall_timeout_ms") == 0) {
      config.stall_timeout_ms = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "tcp_tx") == 0) {
      config.socket.tcp_tx_buf_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "tcp_rx") == 0) {
      config.socket.tcp_rx_buf_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "tcp_tx_max") == 0) {
      config.socket.tcp_tx_buf_max_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "tcp_rx_max") == 0) {
      config.socket.tcp_rx_buf_max_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "udp_tx") == 0) {
      config.socket.udp_tx_buf_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "udp_rx") == 0) {
      config.socket.udp_rx_buf_size = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "sb_efficiency") == 0) {
      config.socket.sb_efficiency = static_cast<u32>(std::strtoul(value, nullptr, 0));
    } else if (std::strcmp(key, "bsd_sessions") == 0) {
      config.socket.num_bsd_sessions = static_cast<u32>(std::strtoul(value, nullptr, 0));
    }
  }
  fclose(file);

  if (config.host[0] == '\0') return false;
  if (config.path[0] == '\0') CopyValue(config.path, sizeof(config.path), "/");
  return true;
}

/// One line of the report, to the console and to the log at once. Everything
/// this probe learns goes through here, because a measurement only visible on a
/// screen someone photographed is not a measurement anyone can quote.
class Sink {
 public:
  explicit Sink(FILE* log) : log_(log) {}

  void Line(const char* format, ...) __attribute__((format(printf, 2, 3))) {
    char text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    std::printf("%s\n", text);
    if (log_ != nullptr) std::fprintf(log_, "%s\n", text);
    consoleUpdate(nullptr);
  }

  /// A blank separator line. Not Line(""), which -Wformat-zero-length refuses.
  void Blank() { Line("%s", ""); }

 private:
  FILE* log_;
};

void ReportStages(Sink& sink, const tlsprobe::Report& report) {
  const tlsprobe::HeapSample& base = report.stage[0];
  sink.Line("stage                 malloc     delta      process    delta");
  for (size_t i = 0; i < tlsprobe::kStageCount; ++i) {
    const tlsprobe::HeapSample& sample = report.stage[i];
    if (i != 0 && sample.malloc_in_use == 0 && sample.used_memory == 0) continue;
    sink.Line("%-18s %9zu %+9lld %10llu %+9lld",
              tlsprobe::StageName(static_cast<tlsprobe::Stage>(i)), sample.malloc_in_use,
              static_cast<long long>(sample.malloc_in_use) -
                  static_cast<long long>(base.malloc_in_use),
              static_cast<unsigned long long>(sample.used_memory),
              static_cast<long long>(sample.used_memory) -
                  static_cast<long long>(base.used_memory));
  }
}

}  // namespace

int main(int, char**) {
  consoleInit(nullptr);
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  std::printf("rommsync-tlsprobe %s -- M0-1 TLS feasibility spike\n", rommsync::kVersion);
  std::printf("issue: can a sysmodule do TLS through the `ssl` service?\n\n");

  tlsprobe::Config config;
  const bool have_config = LoadConfig(config);

  if (have_config) {
    FILE* log = fopen(kLogPath, "a");
    Sink sink(log);

    sink.Line("--- rommsync-tlsprobe %s ---", rommsync::kVersion);
    sink.Line("target   https://%s:%u%s", config.host, config.port, config.path);
    sink.Line("sni      %s", config.sni[0] != '\0' ? config.sni : config.host);
    sink.Line("verify   %s%s", config.verify ? "on" : "OFF (SkipDefaultVerify)",
              config.ca_path[0] != '\0' ? ", ImportServerPki" : "");
    sink.Line("buffers  bsd tmem %zu bytes, read buffer %u bytes",
              tlsprobe::ExpectedBsdTransferMemory(config.socket), config.read_buf_size);
    sink.Blank();

    tlsprobe::Report report;
    tlsprobe::RunProbe(config, report);

    if (report.ok) {
      sink.Line("RESULT   HTTP %d, %llu header bytes, %llu body bytes", report.status,
                static_cast<unsigned long long>(report.header_bytes),
                static_cast<unsigned long long>(report.body_bytes));
    } else {
      sink.Line("RESULT   FAILED at %s, rc=0x%08x (module %u, desc %u)", report.failed_at,
                report.rc, R_MODULE(report.rc), R_DESCRIPTION(report.rc));
      if (report.verify_cert_rc != 0) {
        sink.Line("         cert verify error rc=0x%08x", report.verify_cert_rc);
      }
    }
    sink.Line("tls      %s %s", report.protocol_version, report.cipher);
    sink.Line("timing   connect %llums, handshake %llums, total %llums",
              static_cast<unsigned long long>(report.connect_ms),
              static_cast<unsigned long long>(report.handshake_ms),
              static_cast<unsigned long long>(report.total_ms));
    sink.Line("console  %s", report.ip[0] != '\0' ? report.ip : "(no address from nifm)");
    if (report.read_end_rc != 0) {
      sink.Line("read end rc=0x%08x (a close after `Connection: close` reads as one)",
                report.read_end_rc);
    }
    sink.Blank();
    ReportStages(sink, report);
    sink.Blank();
    sink.Line("written to %s", kLogPath);

    if (log != nullptr) fclose(log);
  } else {
    std::printf("no %s\n\n", kConfigPath);
    std::printf("%s\n", kConfigTemplate);
    std::printf("Nothing was sent anywhere. This probe has no default target on\n");
    std::printf("purpose -- see tlsprobe/README.md.\n");
  }

  std::printf("\n+ to exit\n");
  while (appletMainLoop()) {
    padUpdate(&pad);
    if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) break;
    consoleUpdate(nullptr);
  }

  consoleExit(nullptr);
  return 0;
}

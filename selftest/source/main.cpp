// rommsync-selftest -- M12-1 (#306): the on-console selftest, as a homebrew
// .nro someone launches by hand against a disposable RomM, and closes again.
//
// It runs the sysmodule's real Horizon backends (the `ssl` transport, the SD
// card) and core/'s real engine calls (pairing, negotiate, a tick, a ranged
// download) and writes what each proved to
// sdmc:/config/rommsync-selftest/report-<ts>.json, which
// scripts/selftest-judge.py turns into PASS/FAIL per row on a host. The rows,
// and what each one asks, are selftest/README.md's.
//
// Three refusals, all about CLAUDE.md hard rule 1:
//
//   * No compiled-in target. With no selftest.ini this prints the ini it wants
//     and stops, having sent nothing anywhere -- tlsprobe's rule.
//   * Nothing outside its own folder. Every file it makes is under
//     sdmc:/config/rommsync-selftest/: its own token, identity, backups, state
//     and scratch saves -- never the sysmodule's /config/rommsync/, never an
//     emulator's folder (`ctest -R selftest.roots`).
//   * One run per launch, then + exits. Nothing is installed, nothing survives.
#include <switch.h>

#include <cstdio>
#include <ctime>
#include <string>

#include "rommsync/atomic_file.hpp"
#include "rommsync/log.hpp"
#include "rommsync/version.hpp"
#include "card.hpp"
#include "selftest.hpp"
#include "ssl_http_client.hpp"

namespace {

std::string UtcStamp(std::time_t now, const char* format) {
  std::tm parts = {};
  gmtime_r(&now, &parts);
  char text[32];
  std::strftime(text, sizeof(text), format, &parts);
  return text;
}

void WaitForPlus(PadState& pad) {
  selftest::Say("\n+ to exit");
  while (appletMainLoop()) {
    padUpdate(&pad);
    if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) break;
    consoleUpdate(nullptr);
  }
}

}  // namespace

int main(int, char**) {
  consoleInit(nullptr);
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  selftest::Say("rommsync-selftest %s -- M12-1, the on-console selftest", rommsync::kVersion);
  selftest::InstallCommitHook();

  // The folder first: the ini lives in it, and a missing one is the first-run
  // case, which ends with the template on the screen and nothing sent.
  std::unique_ptr<rommsync::fs::FileSystem> bootstrap =
      rommsync::sysmodule::MakeSdCard(rommsync::fs::Format::kUnknown);
  bootstrap->CreateDirectory(selftest::kRootSd);

  selftest::Run run;
  std::string why;
  if (!selftest::LoadConfig(selftest::UnderRoot("selftest.ini"), &run.config, &why)) {
    selftest::Say("%s\n\n%s", why.c_str(), selftest::kConfigTemplate);
    selftest::Say("Nothing was sent anywhere: this selftest has no default target on");
    selftest::Say("purpose (selftest/README.md).");
    WaitForPlus(pad);
    consoleExit(nullptr);
    return 0;
  }

  const std::time_t now = std::time(nullptr);
  const std::string stamp = UtcStamp(now, "%Y%m%dT%H%M%SZ");
  run.scratch_sd = std::string(selftest::kRootSd) + "/scratch-" + stamp;
  bootstrap->CreateDirectory(run.scratch_sd);

  // The engine's own log, into this folder: a FAIL's evidence is the line the
  // backend wrote, and the sysmodule's log is not ours to write.
  rommsync::log::FileSink log_file(selftest::UnderRoot("selftest-" + stamp + ".log"));
  rommsync::log::SetSink(&log_file);

  selftest::Report report(selftest::UnderRoot("report-" + stamp + ".json"),
                          UtcStamp(now, "%Y-%m-%dT%H:%M:%SZ"), rommsync::kVersion,
                          run.config.url);
  run.report = &report;
  run.pad = &pad;
  selftest::Say("server %s, report %s\n", run.config.url.c_str(), report.path().c_str());

  selftest::RunPlatform(run);
  selftest::RunCard(run);

  // The network the way the sysmodule brings it up: its socket budget, its
  // two ssl sessions, its CA trust. The peak is counted from here.
  selftest::HeapResetPeak();
  run.network = R_SUCCEEDED(rommsync::sysmodule::NetworkInitialize());
  if (!run.config.ca_file.empty()) rommsync::sysmodule::TrustServerCa(run.config.ca_file);
  run.http = rommsync::sysmodule::MakeHorizonHttpClient();
  selftest::RunNetwork(run);
  selftest::RunProtocol(run);
  selftest::RunFootprint(run);

  run.http.reset();
  rommsync::sysmodule::NetworkExit();
  rommsync::log::SetSink(nullptr);

  selftest::Say("\n%d passed, %d failed, %d skipped, %d n/a",
                report.count(selftest::Outcome::kPass), report.count(selftest::Outcome::kFail),
                report.count(selftest::Outcome::kSkip), report.count(selftest::Outcome::kNa));
  selftest::Say("written to %s", report.path().c_str());
  selftest::Say("judge it on a computer: scripts/selftest-judge.py report-%s.json",
                stamp.c_str());

  WaitForPlus(pad);
  consoleExit(nullptr);
  return 0;
}

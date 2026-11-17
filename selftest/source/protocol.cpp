// Stages 4 and 5: the protocol, end to end, and what it cost the heap.
//
// Pairing, negotiation and the tick are core/'s own -- PairingSession,
// sync::Negotiate, sync::RunTick -- over the console transport. The tick runs
// on one fixture save in this run's scratch folder, with its backups and state
// file there too, scoped to the one rom the ini names, and with no placement:
// a server save with nowhere of ours to go is declined rather than written into
// an emulator's folder (sync_execute.hpp, `place`).
#include <switch.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

#include "rommsync/atomic_file.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/download.hpp"
#include "rommsync/json.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/server_capabilities.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync_tick.hpp"
#include "selftest.hpp"
#include "ssl_http_client.hpp"

namespace selftest {
namespace {

using rommsync::sync::Timestamp;

std::string Number(long long value) { return std::to_string(value); }

// Larger than this and the download row would hold a rom in memory twice to
// compare it; the fixture's homebrew roms are kilobytes.
constexpr std::size_t kMaxCompareBytes = 8 * 1024 * 1024;

std::string Describe(const rommsync::http::Result& result) {
  if (result.ok()) return "HTTP " + Number(result.response.status);
  return std::string(rommsync::http::ToString(result.error)) +
         (result.message.empty() ? "" : ": " + result.message);
}

void NotRun(Report& report, const char* id, const char* because) {
  report.Row(id, Outcome::kFail, std::string("not run: ") + because);
}

/// Pairing by device code, shown on the screen for the person holding the
/// console to approve in RomM. B gives up.
bool Pair(Run& run) {
  Report& report = *run.report;
  rommsync::auth::IdentitySeed seed;
  // Entropy only, never the serial: this is a test device, and it must not
  // share an identity with the sysmodule's (device_identity.hpp).
  unsigned char bytes[2 * rommsync::auth::kMinimumEntropyBytes] = {};
  randomGet(bytes, sizeof(bytes));
  seed.entropy.assign(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  const rommsync::auth::IdentityResult identity =
      rommsync::auth::LoadOrCreateDeviceIdentity(UnderRoot("device.dat"), seed);
  if (!identity.ok()) {
    report.Row("proto.pair", Outcome::kFail, "device identity: " + identity.message);
    return false;
  }

  rommsync::auth::PairingConfig config;
  config.server_url = run.config.url;
  config.client_device_identifier = identity.value.client_device_identifier;
  config.device_name = "rommsync-nx selftest";
  rommsync::auth::PairingSession session(*run.http, config);
  rommsync::auth::PairingState state = session.Begin();
  bool shown = false;
  while (!rommsync::auth::IsTerminal(state) && appletMainLoop()) {
    const rommsync::auth::PairingStatus status = session.status();
    if (!shown && !status.user_code.empty()) {
      Say("\n  Approve this device in RomM: %s", status.verification_url.c_str());
      Say("  code  %s     (B gives up)\n", status.user_code.c_str());
      shown = true;
    }
    padUpdate(run.pad);
    if ((padGetButtonsDown(run.pad) & HidNpadButton_B) != 0) break;
    state = session.Poll();
    svcSleepThread(250'000'000);
  }
  const rommsync::auth::DeviceTokenResponse* granted = session.token();
  if (state != rommsync::auth::PairingState::kApproved || granted == nullptr) {
    report.Row("proto.pair", Outcome::kFail,
               std::string("pairing ended ") + rommsync::auth::ToString(state) +
                   (session.status().message.empty() ? "" : ": " + session.status().message));
    return false;
  }
  run.token = rommsync::auth::StoredTokenFrom(run.config.url, *granted);
  const rommsync::auth::StoreResult stored =
      rommsync::auth::SaveToken(UnderRoot("token.dat"), *run.token);
  report.Row("proto.pair", stored.ok() ? Outcome::kPass : Outcome::kFail,
             stored.ok() ? "approved after " + Number(session.status().polls) + " poll(s)"
                         : "approved, and the token could not be stored: " + stored.message);
  return stored.ok();
}

/// The fixture save, written fresh each run so the server has not seen it.
bool FixtureSave(Run& run, rommsync::sync::ClientSaveState* state,
                 rommsync::sync::SaveTarget* target) {
  const std::string dir_sd = run.scratch_sd + "/saves";
  run.card->CreateDirectory(dir_sd);
  const std::string name = "rommsync-selftest.srm";
  const std::string sd_path = dir_sd + "/" + name;
  const std::string path = run.card->Resolve(sd_path);
  const std::string contents =
      "rommsync-selftest fixture save " + Number(static_cast<long long>(std::time(nullptr))) +
      "\n" + std::string(8 * 1024 - 48, 'S');
  if (!rommsync::io::WriteAtomically(path, contents).ok()) return false;
  const rommsync::state::HashOutcome hashed = rommsync::state::HashFile(path);
  if (hashed.content_hash.empty()) return false;

  state->rom_id = run.config.rom_id;
  state->file_name = name;
  state->content_hash = hashed.content_hash;
  state->updated_at = std::chrono::system_clock::now();
  state->file_size_bytes = static_cast<std::int64_t>(contents.size());
  target->rom_id = run.config.rom_id;
  target->sd_path = sd_path;
  target->file_name = name;
  return true;
}

void Download(Run& run) {
  Report& report = *run.report;
  rommsync::http::HttpClient& http = *run.http;
  const std::string bearer = "Bearer " + run.token->access_token;

  rommsync::http::Request detail_request;
  detail_request.url = run.config.url + "/api/roms/" + Number(run.config.rom_id);
  detail_request.headers.push_back({"Authorization", bearer});
  const rommsync::http::Result detail = http.Send(detail_request);
  rommsync::download::RomDetail rom;
  if (!detail.successful() ||
      !rommsync::download::ParseRomDetail(detail.response.body, &rom).ok()) {
    report.Row("proto.download_resume", Outcome::kFail,
               "rom " + Number(run.config.rom_id) + ": " + Describe(detail));
    return;
  }
  if (rom.has_multiple_files || rom.size_bytes < 2 ||
      static_cast<std::size_t>(rom.size_bytes) > kMaxCompareBytes) {
    report.Row("proto.download_resume", Outcome::kFail,
               "rom " + Number(rom.id) + " must be one file of 2 bytes to 8 MiB; it is " +
                   Number(rom.size_bytes) + " bytes" +
                   (rom.has_multiple_files ? " in several files" : ""));
    return;
  }

  const std::string dir_sd = run.scratch_sd + "/download";
  run.card->CreateDirectory(dir_sd);
  rommsync::http::Request get;
  get.url = rommsync::download::ContentUrl(run.config.url, rom.id, rom.fs_name);
  get.headers.push_back({"Authorization", bearer});
  get.timeout = std::chrono::milliseconds(0);  // a rom: the stall timeout decides
  const auto size = static_cast<std::uint64_t>(rom.size_bytes);

  // Whole, first: the bytes the resumed copy has to match.
  rommsync::http::DownloadTarget whole;
  whole.path = run.card->Resolve(dir_sd + "/whole.bin");
  whole.expected_size = size;
  const rommsync::http::Result fetched = http.Download(get, whole);
  const rommsync::io::ReadResult reference = rommsync::io::ReadFile(whole.path);
  if (!fetched.successful() || !reference.ok() || reference.contents.size() != size) {
    report.Row("proto.download_resume", Outcome::kFail, "the whole download: " + Describe(fetched));
    return;
  }

  // Then an interrupted one: the first half staged as the `.part` a dropped
  // connection leaves, and the same request resumed from it with Range. Staged
  // by hand rather than by cutting the network, so the row means the same on
  // every run; a real drop is NET's scenarios in the plan.
  rommsync::http::DownloadTarget resumed;
  resumed.path = run.card->Resolve(dir_sd + "/resumed.bin");
  resumed.resume = true;
  resumed.expected_size = size;
  const std::size_t half = static_cast<std::size_t>(size / 2);
  const bool staged =
      rommsync::io::WriteAtomically(rommsync::http::PartialPathFor(resumed.path),
                                    std::string_view(reference.contents).substr(0, half))
          .ok();
  const rommsync::http::Result rest = staged ? http.Download(get, resumed)
                                             : rommsync::http::Result{};
  const rommsync::io::ReadResult joined = rommsync::io::ReadFile(resumed.path);
  const bool ranged = rest.successful() && rest.response.status == 206 &&
                      rest.response.bytes_received == size - half;
  const bool same = joined.ok() && joined.contents == reference.contents;
  report.Row("proto.download_resume", staged && ranged && same ? Outcome::kPass : Outcome::kFail,
             !staged   ? std::string("the half could not be staged")
             : !ranged ? "the resume answered " + Describe(rest) + " with " +
                             Number(static_cast<long long>(rest.response.bytes_received)) +
                             " bytes; a 206 with " + Number(static_cast<long long>(size - half)) +
                             " was wanted"
             : !same   ? std::string("the resumed file differs from the whole one")
                       : "resumed at byte " + Number(static_cast<long long>(half)) + " of " +
                             Number(static_cast<long long>(size)) + ": 206, identical");
}

}  // namespace

void RunProtocol(Run& run) {
  Report& report = *run.report;
  if (!Pair(run)) {
    for (const char* id : {"proto.negotiate", "proto.tick", "proto.download_resume"}) {
      NotRun(report, id, "proto.pair did not end with a token");
    }
    return;
  }

  rommsync::sync::ClientSaveState save;
  rommsync::sync::SaveTarget target;
  if (!FixtureSave(run, &save, &target)) {
    NotRun(report, "proto.negotiate", "the fixture save could not be written");
    NotRun(report, "proto.tick", "the fixture save could not be written");
  } else {
    const rommsync::server::Probe server =
        rommsync::server::ReadServer(*run.http, run.config.url);

    // A negotiation alone, which opens a session the tick below does not
    // complete -- RomM lets it lapse, as it does any session a client drops.
    rommsync::sync::NegotiateOptions negotiate;
    const rommsync::sync::Negotiation negotiated =
        rommsync::sync::Negotiate(*run.http, *run.token, {save}, negotiate);
    report.Row("proto.negotiate", negotiated.ok() ? Outcome::kPass : Outcome::kFail,
               negotiated.ok()
                   ? Number(static_cast<long long>(negotiated.plan.operations.size())) +
                         " operation(s), session " + Number(negotiated.plan.session_id)
                   : std::string(rommsync::sync::ToString(negotiated.error)) + ": " +
                         negotiated.message);

    rommsync::sync::TickOptions tick;
    tick.server = server.capabilities;
    tick.rom_scope = std::vector<std::int64_t>{run.config.rom_id};
    tick.execute.backup_dir = run.scratch_sd + "/backup";
    tick.finish.state_sd_path = run.scratch_sd + "/state.db";
    tick.network_clock = &NetworkClock;
    const rommsync::sync::TickResult ticked = rommsync::sync::RunTick(
        *run.http, *run.card, *run.token, {save}, {target}, rommsync::state::Baseline{}, tick);
    report.Row("proto.tick", ticked.ok() && ticked.totals.failed == 0 ? Outcome::kPass
                                                                      : Outcome::kFail,
               std::string("tick ") + rommsync::sync::ToString(ticked.outcome) + ": " +
                   Number(ticked.totals.completed) + " completed, " +
                   Number(ticked.totals.failed) + " failed, " + Number(ticked.totals.declined) +
                   " declined" +
                   (ticked.negotiated.message.empty() ? "" : "; " + ticked.negotiated.message));
  }

  Download(run);
}

void RunFootprint(Run& run) {
  // The peak of every C++ allocation since the network came up, plus the bsd
  // transfer memory -- which libnx takes from this same heap, and which the
  // sysmodule's table counts (main.cpp) -- against the sysmodule's whole heap.
  // An NRO's heap is the loader's and far larger, so this is the number that
  // says whether the same work would have fitted in the sysmodule.
  const HeapCount heap = HeapNow();
  const std::size_t bsd =
      rommsync::sysmodule::ExpectedBsdTransferMemory(rommsync::sysmodule::SocketBudget{});
  const std::size_t total = heap.peak_bytes + bsd;
  run.report->Row("footprint.heap", total <= kSysmoduleHeap ? Outcome::kPass : Outcome::kFail,
                  "peak " + Number(static_cast<long long>(heap.peak_bytes)) + " B of C++ heap + " +
                      Number(static_cast<long long>(bsd)) + " B bsd = " +
                      Number(static_cast<long long>(total)) + " B, against the sysmodule's " +
                      Number(static_cast<long long>(kSysmoduleHeap)),
                  "{\"peak_bytes\": " + Number(static_cast<long long>(heap.peak_bytes)) +
                      ", \"bsd_bytes\": " + Number(static_cast<long long>(bsd)) +
                      ", \"allocations\": " + Number(static_cast<long long>(heap.allocations)) +
                      "}");
}

}  // namespace selftest

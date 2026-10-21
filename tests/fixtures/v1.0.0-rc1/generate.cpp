// What wrote the files beside this one: v1.0.0-rc1's own writers, not ours.
//
// The fixtures are only worth anything if they are what that release put on a
// card, so they are not typed by hand and not written by the current build --
// a format that drifted would then be "verified" against a copy of itself.
// This program is compiled against the tag's `core/` and run once:
//
//   git archive -o rc1.tar v1.0.0-rc1 && mkdir rc1 && tar -xf rc1.tar -C rc1
//   cmake -S rc1 -B rc1/build && cmake --build rc1/build --target rommsync_core
//   c++ -std=c++20 -Irc1/core/include -Irc1/build/core/include \
//       tests/fixtures/v1.0.0-rc1/generate.cpp rc1/build/core/librommsync_core.a \
//       -o generate && ./generate rc1/packaging/config.ini.example tests/fixtures/v1.0.0-rc1
//
// It is not built by CMake and never will compile against `main` for long:
// it speaks the tag's API. The next release that changes a format copies it to
// that release's directory and points it at that tag (docs/ARCHITECTURE.md,
// "Formats across releases"). `compat.fixtures` is what reads the output.
//
// Every value below is fixed -- no clock, no entropy -- so running it again
// against the same tag reproduces the same bytes. Each file is named for the
// one it stands for with its dot as an underscore (`token_dat`), because the
// real names are ignored and guarded as secrets (.gitignore, CLAUDE.md rule 5).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rommsync/auth_gate.hpp"
#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/download.hpp"
#include "rommsync/play_sessions.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace rs = rommsync;

namespace {

rs::sync::Timestamp At(std::int64_t unix_seconds) {
  return rs::sync::Timestamp{std::chrono::seconds{unix_seconds}};
}

int Fail(const std::string& what) {
  std::fprintf(stderr, "generate: %s\n", what.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    return Fail("usage: generate <rc1 config.ini.example> <output directory>");
  }
  const std::string out = std::string(argv[2]) + "/";

  // config.ini: the shipped example, copied and then edited from the overlay --
  // which is how a real one is made. A `[platform.snes]` edit writes the whole
  // built-in mapping out with it, so that rule is in the fixture too.
  std::ifstream example_in(argv[1]);
  const std::string example((std::istreambuf_iterator<char>(example_in)),
                            std::istreambuf_iterator<char>());
  if (example.empty()) {
    return Fail("could not read the example");
  }
  rs::config::Edit edit;
  edit.assignments = {
      {"server", "url", "https://romm.lan:8443/romm/", false},
      {"sync", "interval_min", "60", false},
      {"sync", "states", "yes", false},
      {"downloads", "resume", "false", false},
      {"platform.snes", "saves", "/retroarch/saves, /tico/saves/snes, /saves/snes", false},
      {"platform.ps2", "roms", "/roms/ps2", false},
  };
  std::string config_text;
  std::vector<rs::config::Diagnostic> refused;
  if (!rs::config::ApplyEdit(example, edit, &config_text, &refused)) {
    return Fail("the edit was refused: " + (refused.empty() ? "" : refused.front().Describe()));
  }
  std::ofstream(out + "config_ini", std::ios::binary) << config_text;

  // state.db: one save with a server side, one without, one state.
  rs::state::Baseline baseline;
  baseline.Set({101, std::string("retroarch-srm"), "0123456789abcdef0123456789abcdef", At(1757000000),
                32768, At(1757000100), std::string("fedcba9876543210fedcba9876543210")});
  baseline.Set({202, std::nullopt, "00112233445566778899aabbccddeeff", At(1757001000), 8192,
                std::nullopt, std::nullopt});
  baseline.SetState({101, "Super Game.state1", "retroarch", "ffeeddccbbaa99887766554433221100",
                     At(1757002000), 1048576, 77, At(1757002100), 1048576});
  if (!rs::state::SaveBaseline(out + "state_db", baseline).ok()) {
    return Fail("state.db");
  }

  // queue.json: one row in each state a boot can find.
  rs::download::Queue queue;
  queue.Reset({
      {301, "gba", "Pocket Game.gba", 4194304, "da39a3ee5e6b4b0d3255bfef95601890afd80709",
       "/tico/roms/gba/Pocket Game.gba", rs::download::QueueState::kQueued, 0, 0, "", 1757003000},
      {302, "nes", "Tiny.nes", 40976, "", "/tico/roms/nes/Tiny.nes",
       rs::download::QueueState::kActive, 16384, 1, "", 1757003001},
      {303, "snes", "Done.sfc", 524288, "", "/tico/roms/snes/Done.sfc",
       rs::download::QueueState::kDone, 524288, 1, "", 1757003002},
      {304, "psx", "Disc.cue", 0, "", "", rs::download::QueueState::kSkipped, 0, 0,
       "a multi-disc set is not downloaded", 1757003003},
  });
  if (!rs::download::SaveQueue(out + "queue_json", queue).ok()) {
    return Fail("queue.json");
  }

  // conflicts.db: a save conflict kept both ways, then a state replaced.
  rs::conflicts::History history(out + "conflicts_db");
  rs::conflicts::Entry save;
  save.kind = rs::conflicts::EntryKind::kSave;
  save.event = rs::conflicts::Event::kConflict;
  save.rom_id = 101;
  save.rom_name = "Super Game";
  save.file_name = "Super Game.srm";
  save.slot = "retroarch-srm";
  save.emulator = "snes9x";
  save.when = 1757004000;
  save.reason = "both sides changed since the last sync";
  save.sd_path = "/retroarch/saves/Super Game.srm";
  save.local_size_bytes = 32768;
  save.local_content_hash = "0123456789abcdef0123456789abcdef";
  save.local_modified = 1757003900;
  save.server_content_hash = "fedcba9876543210fedcba9876543210";
  save.server_updated_at = "2025-09-04T16:00:00+00:00";
  save.server_size_bytes = 32768;
  save.backup_sd_path = "/config/rommsync/.backup/101-retroarch-srm-1757004000.srm";
  if (!history.Append(save).ok()) {
    return Fail("conflicts.db (save)");
  }
  rs::conflicts::Entry state;
  state.kind = rs::conflicts::EntryKind::kState;
  state.event = rs::conflicts::Event::kReplaced;
  state.rom_id = 101;
  state.rom_name = "Super Game";
  state.file_name = "Super Game.state1";
  state.emulator = "retroarch";
  state.when = 1757004100;
  state.reason = "the server's state is newer";
  state.sd_path = "/retroarch/states/Super Game.state1";
  state.local_size_bytes = 1048576;
  state.local_content_hash = "ffeeddccbbaa99887766554433221100";
  state.local_modified = 1757004050;
  state.server_updated_at = "2025-09-04T16:01:00+00:00";
  state.server_size_bytes = 1048577;
  state.backup_sd_path = "/config/rommsync/.backup/101-state-Super Game.state1-1757004100.state1";
  if (!history.Append(state).ok()) {
    return Fail("conflicts.db (state)");
  }

  // play.db: two sessions waiting for the server.
  rs::play::Buffer buffer(out + "play_db");
  rs::sync::PlaySession first;
  first.rom_id = 101;
  first.save_slot = "retroarch-srm";
  first.start_time = At(1757005000);
  first.end_time = At(1757006800);
  first.duration_ms = 1800000;
  rs::sync::PlaySession second;
  second.rom_id = 202;
  second.start_time = At(1757007000);
  second.end_time = At(1757007600);
  second.duration_ms = 600000;
  if (!buffer.Record({first, second}, At(1757007700)).ok()) {
    return Fail("play.db");
  }

  // auth.json: the one verdict that outlives a boot.
  if (!rs::auth::SaveBlock(out + "auth_json", rs::auth::Block::kRevoked).ok()) {
    return Fail("auth.json");
  }

  // token.dat: a credential no server ever issued.
  rs::auth::StoredToken token;
  token.server_url = "https://romm.lan:8443/romm";
  token.access_token = "fixture-token-not-a-secret";
  token.device_id = "0f0e0d0c-0b0a-4908-8706-050403020100";
  token.scopes = {"assets.read", "assets.write", "devices.read", "devices.write",
                  "platforms.read", "roms.read", "roms.user.read", "roms.user.write"};
  if (!rs::auth::SaveToken(out + "token_dat", token).ok()) {
    return Fail("token.dat");
  }

  // device.dat: derived from a fixed stand-in for a serial, never a real one.
  rs::auth::IdentitySeed seed;
  seed.stable = "XAW00000000000";
  if (!rs::auth::LoadOrCreateDeviceIdentity(out + "device_dat", seed).ok()) {
    return Fail("device.dat");
  }
  return 0;
}

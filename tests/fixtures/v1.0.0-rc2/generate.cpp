// What wrote the files beside this one: v1.0.0-rc2's own writers, not ours.
//
// rc1's directory holds every record rc1 wrote; this one holds only the record
// whose format moved after rc2 -- `state.db`, which #294 bound to a server --
// so the upgrade from rc2's version 3 is read from rc2's own bytes rather than
// from a copy of what this build thinks they were. The rest of rc2's records are
// rc1's formats or test_compat's newer-direction cases.
//
// Compiled against the tag's `core/` and run once, as rc1's is:
//
//   git archive -o rc2.tar v1.0.0-rc2 && mkdir rc2 && tar -xf rc2.tar -C rc2
//   cmake -S rc2 -B rc2/build && cmake --build rc2/build --target rommsync_core
//   c++ -std=c++20 -Irc2/core/include -Irc2/build/core/include \
//       tests/fixtures/v1.0.0-rc2/generate.cpp rc2/build/core/librommsync_core.a \
//       -o generate && ./generate tests/fixtures/v1.0.0-rc2
//
// Every value is fixed, so running it again reproduces the same bytes. The file
// is named with its dot as an underscore, as rc1's are.
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>

#include "rommsync/state_db.hpp"
#include "rommsync/sync.hpp"

namespace rs = rommsync;

namespace {

rs::sync::Timestamp At(std::int64_t unix_seconds) {
  return rs::sync::Timestamp{std::chrono::seconds{unix_seconds}};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: generate <out-dir>\n");
    return 2;
  }
  const std::string out = std::string(argv[1]) + "/";

  // state.db: a synced save, one RomM deleted and the card kept
  // (`on_server_delete = keep`, #233), an archival save, and a state.
  rs::state::Baseline baseline;
  rs::state::SaveRecord synced;
  synced.rom_id = 101;
  synced.slot = "autosave";
  synced.content_hash = "0123456789abcdef0123456789abcdef";
  synced.mtime = At(1760000000);
  synced.file_size_bytes = 32768;
  synced.server_updated_at = At(1760000100);
  synced.server_content_hash = "0123456789abcdef0123456789abcdef";
  baseline.Set(synced);
  rs::state::SaveRecord kept;
  kept.rom_id = 303;
  kept.slot = "autosave";
  kept.content_hash = "aabbccddeeff00112233445566778899";
  kept.mtime = At(1760001000);
  kept.file_size_bytes = 8192;
  kept.deleted_on_server = true;
  baseline.Set(kept);
  rs::state::SaveRecord archival;
  archival.rom_id = 202;
  archival.content_hash = "00112233445566778899aabbccddeeff";
  archival.mtime = At(1760002000);
  archival.file_size_bytes = 8192;
  baseline.Set(archival);
  baseline.SetState({101, "Super Game.state1", "retroarch", "ffeeddccbbaa99887766554433221100",
                     At(1760003000), 1048576, 77, At(1760003100), 1048576});
  if (!rs::state::SaveBaseline(out + "state_db", baseline).ok()) {
    std::fprintf(stderr, "generate: state.db\n");
    return 1;
  }
  return 0;
}

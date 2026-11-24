// M11-8 (#292): `.backup/` is bounded, and nothing pruned is the only copy.
//
//   names      a backup name reads back as the parts `BackupNameFor` wrote, and
//              anything else is not a backup
//   count      the newest `keep` of each group stay; older copies the server
//              holds go, oldest first
//   age        anything younger than `keep_days` stays whatever the count says
//   cap        over `max_bytes`, retained copies go oldest first across the
//              tree -- never the newest of a group
//   unique     bytes the server does not hold and no newer backup holds are
//              never pruned, by count, age or cap -- nor when the server
//              cannot be asked
//   duplicate  an older identical copy goes when a newer one stays
//   truncated  a directory past one listing: the rom it cuts sits the prune out
//   crash      a power cut at every call of a prune leaves every backup's
//              bytes somewhere
//   states     #330: state backups over the cap whose bytes `state.db` pairs
//              with a server row that has not moved are pruned to the cap, and
//              one whose bytes are nowhere else stays
//   state_moved  a state row the server has moved, replaced or dropped since,
//              or one it could not list, proves nothing
//   state_crash  `crash`, over state backups proven by `state.db`
//   state_proven only the rows this tick's states half confirmed are a proof:
//              never one an upload just wrote
//   state_budget  #340: two consoles trading a state, its backups over the
//              default state budget -- each the only copy of its bytes. The
//              oldest go down to the budget, the newest `keep_states` of each
//              state stay, and no save backup is touched
//   state_budget_off  with the budget at 0, or nothing known to be a state,
//              nothing unique goes (#330's behaviour)
//   state_budget_names  only what `state.db` names a state's backup counts: a
//              save's slot spelled `state-...`, an unknown state, another
//              server's backup, never
//   state_budget_crash  a power cut at every call of a budget prune removes a
//              subset of what the whole prune removes, and nothing else
//   server     against the real RomM: a save's digest is listed for its rom,
//              and a state's is while its row has not moved
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "crash_disk.hpp"
#include "harness.hpp"
#include "rommsync/backup_prune.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/md5.hpp"

namespace {

using namespace rommsync;
using harness::kBackupDir;
using harness::Sandbox;

constexpr std::int64_t kDay = 24 * 60 * 60;
constexpr std::int64_t kNow = 1'760'000'000;

sync::Timestamp Moment(std::int64_t unix_seconds) {
  return sync::Timestamp{std::chrono::seconds{unix_seconds}};
}

/// One backup on the card: where, and what it holds.
struct Seeded {
  std::string sd_path;
  std::string bytes;
};

Seeded Seed(Sandbox& sandbox, std::int64_t rom_id, const std::string& discriminator,
            std::int64_t unix_seconds, const std::string& bytes, int uniquifier = 0,
            const std::string& file_name = "game.srm") {
  Seeded seeded{std::string(kBackupDir) + "/" +
                    sync::BackupNameFor(rom_id, discriminator, file_name, unix_seconds, uniquifier),
                bytes};
  sandbox.Write(seeded.sd_path, bytes);
  return seeded;
}

/// A server that holds exactly these digests per rom; a rom it has no entry for
/// answers "could not be read".
struct FakeServer {
  std::map<std::int64_t, std::vector<std::string>> digests;
  std::set<std::int64_t> asked;

  backup::ServerDigests Callback() {
    return [this](std::int64_t rom_id) -> std::optional<std::vector<std::string>> {
      asked.insert(rom_id);
      const auto found = digests.find(rom_id);
      if (found == digests.end()) return std::nullopt;
      return found->second;
    };
  }
  void Holds(std::int64_t rom_id, const std::string& bytes) {
    digests[rom_id].push_back(crypto::Md5Hex(bytes));
  }
};

backup::PruneOptions Options(FakeServer* server, int keep, int keep_days, std::int64_t max_bytes) {
  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention.keep = keep;
  options.retention.keep_days = keep_days;
  options.retention.max_bytes = max_bytes;
  options.now = []() { return Moment(kNow); };
  if (server != nullptr) options.server_digests = server->Callback();
  return options;
}

std::string Bytes(int n, std::size_t size = 64) {
  std::string out = "backup " + std::to_string(n) + " ";
  out.resize(size, static_cast<char>('a' + n % 26));
  return out;
}

/// A row of `GET /api/states?rom_id=`, as the fake server lists it.
sync::ServerState Listed(std::int64_t id, std::int64_t rom_id, const std::string& file_name,
                         std::int64_t size, std::int64_t updated_unix) {
  sync::ServerState row;
  row.id = id;
  row.rom_id = rom_id;
  row.file_name = file_name;
  row.emulator = "retroarch";
  row.file_size_bytes = size;
  row.updated_at = Moment(updated_unix);
  return row;
}

/// The `state.db` row a transfer of `bytes` to or from `server` leaves.
state::StateRecord Paired(const sync::ServerState& server, const std::string& bytes) {
  state::StateRecord row;
  row.rom_id = server.rom_id;
  row.file_name = server.file_name;
  row.emulator = server.emulator;
  row.content_hash = crypto::Md5Hex(bytes);
  row.mtime = Moment(kNow - kDay);
  row.file_size_bytes = static_cast<std::int64_t>(bytes.size());
  row.server_state_id = server.id;
  row.server_updated_at = server.updated_at;
  row.server_file_size_bytes = server.file_size_bytes;
  return row;
}

/// A RomM that lists these state rows per rom, proven against `baseline` the
/// way the engine proves them; a rom it has no entry for answers "could not be
/// read".
struct FakeStates {
  state::Baseline baseline;
  std::map<std::int64_t, std::vector<sync::ServerState>> listed;

  backup::ServerDigests Callback() {
    return [this](std::int64_t rom_id) -> std::optional<std::vector<std::string>> {
      const auto found = listed.find(rom_id);
      if (found == listed.end()) return std::nullopt;
      return backup::StateDigestsOnServer(baseline, rom_id, found->second);
    };
  }
  /// The server holds `bytes` under `server`, and this console's last sync of
  /// it says so.
  void Holds(const sync::ServerState& server, const std::string& bytes) {
    listed[server.rom_id].push_back(server);
    baseline.SetState(Paired(server, bytes));
  }
};

void Names(checks::Checks& c) {
  const std::optional<backup::BackupName> plain =
      backup::ParseBackupName("4-autosave-rtc-1757000000.srm");
  c.Expect(plain.has_value(), "a slot with a dash in it reads back");
  if (plain) {
    c.ExpectEq(plain->rom_id, std::int64_t{4}, "its rom");
    c.ExpectEq(plain->discriminator, std::string("autosave-rtc"), "its slot");
    c.ExpectEq(plain->unix_seconds, std::int64_t{1757000000}, "its second");
    c.ExpectEq(plain->uniquifier, 0, "and no uniquifier");
  }
  const std::optional<backup::BackupName> walked =
      backup::ParseBackupName("12-state-Game-1757000000-3.state");
  c.Expect(walked.has_value(), "a uniquified state backup reads back");
  if (walked) {
    c.ExpectEq(walked->discriminator, std::string("state-Game"), "its discriminator");
    c.ExpectEq(walked->unix_seconds, std::int64_t{1757000000}, "its second");
    c.ExpectEq(walked->uniquifier, 3, "and its uniquifier");
  }
  const std::optional<backup::BackupName> digits =
      backup::ParseBackupName("7-slot-5-1757000000.srm");
  c.Expect(digits && digits->discriminator == "slot-5" && digits->uniquifier == 0,
           "a slot ending in digits is not read as a uniquifier");
  const std::optional<backup::BackupName> no_ext = backup::ParseBackupName("7-archival-1757000000");
  c.Expect(no_ext && no_ext->discriminator == "archival", "a save with no extension reads back");

  for (const char* stranger : {"4-autosave-1757000000.srm.tmp", "notes.txt", "-x-1757000000.srm",
                               "4-1757000000.srm", "4--1757000000.srm", "abc-x-1757000000.srm",
                               "4-x-.srm"}) {
    c.Expect(!backup::ParseBackupName(stranger).has_value(),
             std::string("not a backup: ") + stranger);
  }
}

void Count(checks::Checks& c) {
  Sandbox sandbox(c, "prune-count");
  FakeServer server;
  std::vector<Seeded> seeded;
  // Five of one save, a year old, every one also on the server; and one of
  // another rom's, alone in its group.
  for (int i = 0; i < 5; ++i) {
    seeded.push_back(Seed(sandbox, 4, "autosave", kNow - 365 * kDay + i, Bytes(i)));
    server.Holds(4, Bytes(i));
  }
  const Seeded other = Seed(sandbox, 9, "autosave", kNow - 365 * kDay, Bytes(9));
  server.Holds(9, Bytes(9));

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, Options(&server, 2, 0, 0));

  c.ExpectEq(report.backups, std::size_t{6}, "six backups were found");
  c.ExpectEq(report.pruned, std::size_t{3}, "three were pruned");
  for (int i = 0; i < 3; ++i) {
    c.Expect(!sandbox.Exists(seeded[i].sd_path), "an older copy went: " + seeded[i].sd_path);
  }
  for (int i = 3; i < 5; ++i) {
    c.Expect(sandbox.Exists(seeded[i].sd_path), "the newest two stayed: " + seeded[i].sd_path);
  }
  c.Expect(sandbox.Exists(other.sd_path), "a group under the count is untouched");
  c.Expect(server.asked.count(9) == 0, "a rom with no candidate is never asked about");
  c.ExpectEq(report.bytes_after, report.bytes_before - 3 * 64, "and the bytes add up");
  c.Expect(!backup::Describe(report).empty(), "the log line says something");
}

/// #294: after the console moved to another RomM, a backup taken under the old
/// one names a rom id the new one may give to another game. Even when the new
/// server happens to hold the same digest under that id, it is not the copy the
/// backup is a backup of -- and a server the user only tried is not one they
/// will go back to for it.
void OtherServer(checks::Checks& c) {
  Sandbox sandbox(c, "prune-other-server");
  FakeServer server;
  const std::int64_t switched = kNow - 30 * kDay;
  std::vector<Seeded> before;
  for (int i = 0; i < 4; ++i) {
    before.push_back(Seed(sandbox, 4, "autosave", switched - 100 * kDay + i, Bytes(i)));
    server.Holds(4, Bytes(i));
  }
  // Two identical copies from before: the older is a duplicate of the newer,
  // which stays, so it still goes -- that proof is the card's, not a server's.
  const Seeded twin_old = Seed(sandbox, 4, "autosave", switched - 200 * kDay, Bytes(3));
  // And after the switch, the new server's own backups, which it does prove.
  std::vector<Seeded> after;
  for (int i = 10; i < 14; ++i) {
    after.push_back(Seed(sandbox, 6, "autosave", switched + 1 + i, Bytes(i)));
    server.Holds(6, Bytes(i));
  }

  backup::PruneOptions options = Options(&server, 1, 0, 0);
  options.server_since = switched;
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, options);

  for (const Seeded& kept : before) {
    c.Expect(sandbox.Exists(kept.sd_path),
             "a backup from before the switch is not pruned on the new server's word: " +
                 kept.sd_path);
  }
  c.Expect(!sandbox.Exists(twin_old.sd_path), "but an older duplicate of one that stays goes");
  c.Expect(server.asked.count(4) == 0, "and the new server is never asked about the old rom id");
  for (std::size_t i = 0; i + 1 < after.size(); ++i) {
    c.Expect(!sandbox.Exists(after[i].sd_path),
             "a backup taken since is pruned as before: " + after[i].sd_path);
  }
  c.Expect(sandbox.Exists(after.back().sd_path), "the newest of its group stays");
  c.ExpectEq(report.pruned, std::size_t{4}, "four went: the duplicate and three of the new");
}

void Age(checks::Checks& c) {
  Sandbox sandbox(c, "prune-age");
  FakeServer server;
  const Seeded old_one = Seed(sandbox, 4, "autosave", kNow - 40 * kDay, Bytes(1));
  const Seeded recent = Seed(sandbox, 4, "autosave", kNow - 10 * kDay, Bytes(2));
  const Seeded newest = Seed(sandbox, 4, "autosave", kNow - 1 * kDay, Bytes(3));
  for (int i = 1; i <= 3; ++i) server.Holds(4, Bytes(i));

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, Options(&server, 1, 30, 0));
  c.ExpectEq(report.pruned, std::size_t{1}, "only the copy past thirty days went");
  c.Expect(!sandbox.Exists(old_one.sd_path), "the forty-day-old copy went");
  c.Expect(sandbox.Exists(recent.sd_path), "a ten-day-old copy past the count stays by age");
  c.Expect(sandbox.Exists(newest.sd_path), "and the newest stays");
}

void Cap(checks::Checks& c) {
  Sandbox sandbox(c, "prune-cap");
  FakeServer server;
  // Two groups, everything recent and inside the count, 6 x 100 bytes.
  const Seeded a1 = Seed(sandbox, 4, "autosave", kNow - 50, Bytes(1, 100));
  const Seeded b1 = Seed(sandbox, 5, "autosave", kNow - 40, Bytes(2, 100));
  const Seeded a2 = Seed(sandbox, 4, "autosave", kNow - 30, Bytes(3, 100));
  const Seeded b2 = Seed(sandbox, 5, "autosave", kNow - 20, Bytes(4, 100));
  const Seeded a3 = Seed(sandbox, 4, "autosave", kNow - 10, Bytes(5, 100));
  const Seeded b3 = Seed(sandbox, 5, "autosave", kNow - 5, Bytes(6, 100));
  for (int i : {1, 3, 5}) server.Holds(4, Bytes(i, 100));
  for (int i : {2, 4, 6}) server.Holds(5, Bytes(i, 100));

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, Options(&server, 10, 30, 350));
  c.ExpectEq(report.pruned, std::size_t{3}, "three went to get under 350 bytes");
  c.Expect(!sandbox.Exists(a1.sd_path) && !sandbox.Exists(b1.sd_path) &&
               !sandbox.Exists(a2.sd_path),
           "the oldest three across the tree went");
  c.Expect(sandbox.Exists(b2.sd_path) && sandbox.Exists(a3.sd_path) && sandbox.Exists(b3.sd_path),
           "the newest three stayed");
  c.ExpectEq(report.bytes_after, std::int64_t{300}, "and the directory is under the cap");
  c.Expect(!report.over_cap, "which the report says");

  // Below the newest of each group the cap cannot go, whatever it says.
  const backup::PruneReport floor = backup::Prune(*files, Options(&server, 10, 30, 1));
  c.Expect(sandbox.Exists(a3.sd_path) && sandbox.Exists(b3.sd_path),
           "the newest of each group is never a cap candidate");
  c.Expect(floor.over_cap, "and a cap it cannot reach is reported, not chased");
}

void Unique(checks::Checks& c) {
  Sandbox sandbox(c, "prune-unique");
  FakeServer server;
  // A save whose old bytes the server no longer holds, a state (no digest on
  // the server, ever), and a rom the server will not answer for.
  const Seeded lost = Seed(sandbox, 4, "autosave", kNow - 400 * kDay, Bytes(1));
  const Seeded held = Seed(sandbox, 4, "autosave", kNow - 300 * kDay, Bytes(2));
  Seed(sandbox, 4, "autosave", kNow - 200 * kDay, Bytes(3));
  server.Holds(4, Bytes(2));
  server.Holds(4, Bytes(3));
  const Seeded state = Seed(sandbox, 4, "state-Game", kNow - 400 * kDay, Bytes(4, 4096), 0,
                            "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 300 * kDay, Bytes(5, 4096), 0, "Game.state");
  const Seeded unanswered = Seed(sandbox, 8, "autosave", kNow - 400 * kDay, Bytes(6));
  Seed(sandbox, 8, "autosave", kNow - 300 * kDay, Bytes(7));

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, Options(&server, 1, 0, 1));
  c.Expect(sandbox.Exists(lost.sd_path), "bytes the server no longer holds are kept");
  c.Expect(!sandbox.Exists(held.sd_path), "bytes it does hold go");
  c.Expect(sandbox.Exists(state.sd_path), "a state backup with no other copy is kept");
  c.Expect(sandbox.Exists(unanswered.sd_path), "a rom the server could not answer for keeps all");
  c.ExpectEq(report.pruned, std::size_t{1}, "one pruned");
  c.ExpectEq(report.kept_unique, std::size_t{3}, "three kept as the only copy");
  c.ExpectEq(sandbox.Read(lost.sd_path), lost.bytes, "and a kept one is untouched");

  // No server at all: nothing but a duplicate can go.
  const backup::PruneReport blind = backup::Prune(*files, Options(nullptr, 1, 0, 1));
  c.ExpectEq(blind.pruned, std::size_t{0}, "with no server to ask, nothing unique is pruned");
}

void Duplicate(checks::Checks& c) {
  Sandbox sandbox(c, "prune-duplicate");
  // A state the client backed up twice with the same bytes -- an overwrite that
  // stood down and was tried again -- and a different one in between.
  const Seeded first =
      Seed(sandbox, 4, "state-Game", kNow - 400 * kDay, Bytes(1, 4096), 0, "Game.state");
  const Seeded middle =
      Seed(sandbox, 4, "state-Game", kNow - 300 * kDay, Bytes(2, 4096), 0, "Game.state");
  const Seeded again =
      Seed(sandbox, 4, "state-Game", kNow - 200 * kDay, Bytes(1, 4096), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 100 * kDay, Bytes(3, 4096), 0, "Game.state");
  // The same bytes under another group are not a copy this group can lean on.
  const Seeded elsewhere = Seed(sandbox, 5, "autosave", kNow - 400 * kDay, Bytes(1, 4096));
  Seed(sandbox, 5, "autosave", kNow - 100 * kDay, Bytes(9, 4096));

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, Options(nullptr, 1, 0, 0));
  c.Expect(!sandbox.Exists(first.sd_path), "the older of two identical copies went");
  c.Expect(sandbox.Exists(again.sd_path), "the newer stayed, though it too is past the count");
  c.Expect(sandbox.Exists(middle.sd_path), "a different one in between stayed");
  c.Expect(sandbox.Exists(elsewhere.sd_path), "another group's identical copy is no excuse");
  c.ExpectEq(report.pruned, std::size_t{1}, "one pruned");
}

void Truncated(checks::Checks& c) {
  Sandbox sandbox(c, "prune-truncated");
  FakeServer server;
  // More than one listing holds: rom 1's backups first by name, then rom 9's,
  // which the listing cuts part way -- across two groups whose names
  // interleave, `slot` and `slot-5`.
  for (int i = 0; i < 400; ++i) {
    Seed(sandbox, 1, "autosave", kNow - 400 * kDay + i, Bytes(1));
  }
  server.Holds(1, Bytes(1));
  std::vector<Seeded> cut;
  for (int i = 0; i < 100; ++i) {
    cut.push_back(Seed(sandbox, 9, "slot", kNow - 400 * kDay + i, Bytes(2)));
    cut.push_back(Seed(sandbox, 9, "slot-5", kNow - 400 * kDay + i, Bytes(3)));
  }
  server.Holds(9, Bytes(2));
  server.Holds(9, Bytes(3));

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, Options(&server, 1, 0, 0));
  c.Expect(report.truncated, "the report says the listing was cut");
  c.ExpectEq(report.pruned, std::size_t{399}, "every listed rom but the cut one was pruned");
  std::size_t left = 0;
  for (const Seeded& seeded : cut) left += sandbox.Exists(seeded.sd_path) ? 1 : 0;
  c.ExpectEq(left, cut.size(), "the rom the listing cut sat the prune out, every group of it");
}

/// Every backup the card held before still has its bytes somewhere: on the
/// card under `.backup/`, or on the server.
crash::Judge NothingLost(const std::set<std::string>& on_server) {
  return [on_server](const crash::Tree& after, const crash::Snapshot& before) -> std::string {
    const std::string dir = crash::RelativeOf(kBackupDir) + "/";
    std::set<std::string> held;
    for (const auto& [path, bytes] : after) {
      if (path.rfind(dir, 0) == 0) held.insert(bytes);
    }
    for (const auto& [path, bytes] : before.files) {
      if (path.rfind(dir, 0) != 0) continue;
      if (held.count(bytes) == 0 && on_server.count(crypto::Md5Hex(bytes)) == 0) {
        return path + "'s bytes are gone from the card and are not on the server";
      }
    }
    return {};
  };
}

void Crash(checks::Checks& c) {
  Sandbox sandbox(c, "prune-crash");
  FakeServer server;
  std::set<std::string> on_server;
  for (int i = 0; i < 4; ++i) {
    Seed(sandbox, 4, "autosave", kNow - (400 - i) * kDay, Bytes(i));
    if (i != 1) {
      server.Holds(4, Bytes(i));
      on_server.insert(crypto::Md5Hex(Bytes(i)));
    }
  }
  Seed(sandbox, 4, "state-Game", kNow - 400 * kDay, Bytes(7, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 300 * kDay, Bytes(7, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 200 * kDay, Bytes(8, 512), 0, "Game.state");

  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  crash::Recorder recorder(sandbox.root(), {});
  const backup::PruneReport report = backup::Prune(*files, Options(&server, 1, 0, 0));
  c.ExpectEq(report.pruned, std::size_t{3}, "the prune removed what it could prove");

  const crash::Verdict verdict =
      crash::Replay(recorder, {kBackupDir}, {kBackupDir}, rig::ScratchDir() + "/crash-prune",
                    NothingLost(on_server));
  c.Expect(verdict.unmodelled.empty(), "the model followed every call: " + verdict.unmodelled);
  c.Expect(verdict.points > 1, "the prune made calls to cut");
  c.ExpectEq(verdict.failures, std::size_t{0}, "no power cut loses a backup's bytes: " +
                                                   verdict.first);
}

/// #330's acceptance: state backups over the cap, every one also on the server
/// as far as `state.db` and a fresh listing can show. On #292's rule nothing
/// went -- RomM keeps no digest of a state -- and the card stayed over the cap.
void States(checks::Checks& c) {
  Sandbox sandbox(c, "prune-states");
  FakeStates server;
  constexpr std::size_t kSize = 4096;
  const sync::ServerState a = Listed(11, 4, "A.state", kSize, kNow - 2 * kDay);
  const sync::ServerState b = Listed(12, 4, "B.state", kSize, kNow - 3 * kDay);
  const sync::ServerState other = Listed(13, 4, "C.state", kSize, kNow - 4 * kDay);
  server.Holds(a, Bytes(1, kSize));
  server.Holds(b, Bytes(2, kSize));
  server.Holds(other, Bytes(3, kSize));

  // Group A: two copies of bytes the server holds now (B's and C's), progress
  // held nowhere else, and the newest.
  const Seeded a_held =
      Seed(sandbox, 4, "state-A", kNow - 400 * kDay, Bytes(2, kSize), 0, "A.state");
  const Seeded a_unique =
      Seed(sandbox, 4, "state-A", kNow - 300 * kDay, Bytes(7, kSize), 0, "A.state");
  const Seeded a_twin =
      Seed(sandbox, 4, "state-A", kNow - 200 * kDay, Bytes(3, kSize), 0, "A.state");
  const Seeded a_newest =
      Seed(sandbox, 4, "state-A", kNow - 100 * kDay, Bytes(8, kSize), 0, "A.state");
  // Group B: the bytes the server holds as A -- the live state a restore set
  // aside -- and the newest.
  const Seeded b_held =
      Seed(sandbox, 4, "state-B", kNow - 350 * kDay, Bytes(1, kSize), 0, "B.state");
  const Seeded b_newest =
      Seed(sandbox, 4, "state-B", kNow - 50 * kDay, Bytes(9, kSize), 0, "B.state");

  // Everything is inside the count; only the cap can make room, and it can --
  // three provable copies out of six is exactly down to it.
  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention.keep = 10;
  options.retention.keep_days = 0;
  options.retention.max_bytes = 3 * static_cast<std::int64_t>(kSize);
  options.now = []() { return Moment(kNow); };
  options.server_digests = server.Callback();
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, options);

  c.ExpectEq(report.pruned, std::size_t{3},
             "the three backups the server holds went: " + backup::Describe(report));
  c.Expect(!sandbox.Exists(a_held.sd_path), "a backup of bytes the server holds as B went");
  c.Expect(!sandbox.Exists(a_twin.sd_path), "a backup of bytes the server holds as C went");
  c.Expect(!sandbox.Exists(b_held.sd_path), "a backup of bytes the server holds as A went");
  c.ExpectEq(sandbox.Read(a_unique.sd_path), a_unique.bytes,
             "progress held nowhere else stayed, untouched");
  c.ExpectEq(sandbox.Read(a_newest.sd_path), a_newest.bytes, "the newest of A stayed");
  c.ExpectEq(sandbox.Read(b_newest.sd_path), b_newest.bytes, "the newest of B stayed");
  c.Expect(!report.over_cap, "and the directory reached the cap");
  c.ExpectEq(report.bytes_after, 3 * static_cast<std::int64_t>(kSize), "at exactly its bytes");
}

/// A row is a proof of the server's copy only while the server's copy is the
/// one it describes. Each rom here holds one backup past the count whose bytes
/// `state.db` once paired with a server row -- and only rom 20's row is still
/// the server's.
void StateMoved(checks::Checks& c) {
  Sandbox sandbox(c, "prune-state-moved");
  FakeStates server;
  constexpr std::size_t kSize = 512;
  const std::int64_t at = kNow - 5 * kDay;
  std::map<std::int64_t, Seeded> old_copies;
  const auto seed = [&](std::int64_t rom_id, int n) {
    old_copies.emplace(rom_id, Seed(sandbox, rom_id, "state-Game", kNow - 300 * kDay,
                                    Bytes(n, kSize), 0, "Game.state"));
    Seed(sandbox, rom_id, "state-Game", kNow - 100 * kDay, Bytes(n + 50, kSize), 0, "Game.state");
  };
  const auto paired = [&](const sync::ServerState& then, int n) {
    server.baseline.SetState(Paired(then, Bytes(n, kSize)));
  };

  // 20: unmoved -- the control, which goes.
  server.Holds(Listed(1, 20, "Game.state", kSize, at), Bytes(20, kSize));
  seed(20, 20);
  // 21: another console uploaded since, a second later.
  paired(Listed(2, 21, "Game.state", kSize, at), 21);
  server.listed[21].push_back(Listed(2, 21, "Game.state", kSize, at + 1));
  seed(21, 21);
  // 22: ...in the same second, at another size.
  paired(Listed(3, 22, "Game.state", kSize, at), 22);
  server.listed[22].push_back(Listed(3, 22, "Game.state", kSize + 1, at));
  seed(22, 22);
  // 23: the row was deleted and the name is another row now.
  paired(Listed(4, 23, "Game.state", kSize, at), 23);
  server.listed[23].push_back(Listed(99, 23, "Game.state", kSize, at));
  seed(23, 23);
  // 24: the row is gone.
  paired(Listed(5, 24, "Game.state", kSize, at), 24);
  server.listed[24];
  seed(24, 24);
  // 25: the listing could not be read.
  paired(Listed(6, 25, "Game.state", kSize, at), 25);
  seed(25, 25);
  // 26: the bytes that went up are not the ones hashed -- the state was
  // written between the scan and the upload, and the server's size says so.
  {
    const sync::ServerState then = Listed(7, 26, "Game.state", kSize + 8, at);
    paired(then, 26);
    server.listed[26].push_back(then);
    seed(26, 26);
  }
  // 27: rom 28's row proves rom 28's bytes, not rom 27's backup of the same.
  {
    const sync::ServerState elsewhere = Listed(8, 28, "Game.state", kSize, at);
    server.Holds(elsewhere, Bytes(27, kSize));
    server.listed[27];
    seed(27, 27);
  }

  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention.keep = 1;
  options.retention.keep_days = 0;
  options.retention.max_bytes = 1;
  options.now = []() { return Moment(kNow); };
  options.server_digests = server.Callback();
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, options);

  c.Expect(!sandbox.Exists(old_copies.at(20).sd_path), "the unmoved row proves its bytes");
  for (const std::int64_t rom_id : {21, 22, 23, 24, 25, 26, 27}) {
    c.ExpectEq(sandbox.Read(old_copies.at(rom_id).sd_path), old_copies.at(rom_id).bytes,
               "a row that no longer describes the server's copy proves nothing: rom " +
                   std::to_string(rom_id));
  }
  c.ExpectEq(report.pruned, std::size_t{1}, "one pruned: " + backup::Describe(report));
}

void StateCrash(checks::Checks& c) {
  Sandbox sandbox(c, "prune-state-crash");
  FakeStates server;
  std::set<std::string> on_server;
  for (int i = 0; i < 3; ++i) {
    const sync::ServerState row =
        Listed(30 + i, 4, "S" + std::to_string(i) + ".state", 512, kNow - kDay);
    server.Holds(row, Bytes(i, 512));
    on_server.insert(crypto::Md5Hex(Bytes(i, 512)));
  }
  // Backups the server holds, ones it does not, and a pair of duplicates.
  Seed(sandbox, 4, "state-Game", kNow - 400 * kDay, Bytes(0, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 350 * kDay, Bytes(5, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 300 * kDay, Bytes(1, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 250 * kDay, Bytes(6, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 200 * kDay, Bytes(6, 512), 0, "Game.state");
  Seed(sandbox, 4, "state-Game", kNow - 100 * kDay, Bytes(2, 512), 0, "Game.state");

  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention.keep = 1;
  options.retention.keep_days = 0;
  options.retention.max_bytes = 0;
  options.now = []() { return Moment(kNow); };
  options.server_digests = server.Callback();
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  crash::Recorder recorder(sandbox.root(), {});
  const backup::PruneReport report = backup::Prune(*files, options);
  c.ExpectEq(report.pruned, std::size_t{3},
             "the prune removed what it could prove: " + backup::Describe(report));

  const crash::Verdict verdict =
      crash::Replay(recorder, {kBackupDir}, {kBackupDir},
                    rig::ScratchDir() + "/crash-prune-states", NothingLost(on_server));
  c.Expect(verdict.unmodelled.empty(), "the model followed every call: " + verdict.unmodelled);
  c.Expect(verdict.points > 1, "the prune made calls to cut");
  c.ExpectEq(verdict.failures, std::size_t{0},
             "no power cut loses a state backup's bytes: " + verdict.first);
}

// --- the state budget (#340) -------------------------------------------------

constexpr std::int64_t kMiB = 1024 * 1024;

/// A state backup of `size` bytes whose content is unique to `n`: a tag up
/// front and the rest a hole, so a test can put hundreds of MiB of realistic
/// states on the card without writing them.
Seeded SeedLarge(Sandbox& sandbox, std::int64_t rom_id, const std::string& file_name,
                 std::int64_t unix_seconds, int n, std::int64_t size) {
  const Seeded seeded =
      Seed(sandbox, rom_id, sync::StateBackupDiscriminator(file_name), unix_seconds,
           "state " + std::to_string(rom_id) + " " + std::to_string(n), 0, file_name);
  std::error_code error;
  std::filesystem::resize_file(sandbox.Host(seeded.sd_path), static_cast<std::uintmax_t>(size),
                               error);
  return seeded;
}

/// A `state.db` row for a state, as the last trade left it. Where the server
/// row is now does not matter: the budget asks the server nothing.
void KnowState(state::Baseline* baseline, std::int64_t rom_id, const std::string& file_name) {
  baseline->SetState(
      Paired(Listed(rom_id * 100, rom_id, file_name, 1, kNow - kDay), "the live " + file_name));
}

void KnowSave(state::Baseline* baseline, std::int64_t rom_id,
              const std::optional<std::string>& slot) {
  state::SaveRecord row;
  row.rom_id = rom_id;
  row.slot = slot;
  row.content_hash = crypto::Md5Hex("live save " + std::to_string(rom_id));
  row.mtime = Moment(kNow - kDay);
  row.file_size_bytes = 64;
  baseline->Set(row);
}

/// The acceptance test. Two consoles trade one game's state through RomM -- a
/// heavy core's 16 MiB -- every half hour, and each download on this console
/// backed up the live state it replaced: bytes `POST /api/states` had already
/// replaced on the server, so each backup is the only copy. Twenty trades is
/// 320 MiB of them, over the default budget, and all young enough that
/// `keep_days` keeps every one for a month: on #330's rule nothing goes, and
/// the card fills one trade at a time.
void StateBudget(checks::Checks& c) {
  Sandbox sandbox(c, "prune-state-budget");
  state::Baseline baseline;
  KnowState(&baseline, 4, "Game.state");
  KnowState(&baseline, 5, "Other.state");
  KnowSave(&baseline, 4, "autosave");

  std::vector<Seeded> traded;
  for (int i = 0; i < 20; ++i) {
    traded.push_back(
        SeedLarge(sandbox, 4, "Game.state", kNow - (20 - i) * 30 * 60, i, 16 * kMiB));
  }
  // Another game's state, traded twice long ago: older than every trade above,
  // and within its newest three, which the budget never takes.
  const Seeded other_old = SeedLarge(sandbox, 5, "Other.state", kNow - 90 * kDay, 1, 16 * kMiB);
  const Seeded other_new = SeedLarge(sandbox, 5, "Other.state", kNow - 80 * kDay, 2, 16 * kMiB);
  // Save backups: old, past the count, and on no server. Hard rule 2 holds for
  // them without exception, so the budget never even counts them.
  std::vector<Seeded> saves;
  for (int i = 0; i < 12; ++i) {
    saves.push_back(Seed(sandbox, 4, "autosave", kNow - (400 - i) * kDay, Bytes(i, 1024)));
  }

  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention = backup::RetentionFor(config::Defaults().backup);
  options.now = []() { return Moment(kNow); };
  FakeServer server;  // knows nothing: every backup here is the only copy
  options.server_digests = server.Callback();
  options.is_state_backup = backup::StateBackupsOf(baseline);
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, options);

  // 22 states of 16 MiB is 352; the budget is 256, so six go -- the six oldest
  // the budget may take, which are the first six trades.
  c.ExpectEq(report.budget_pruned, std::size_t{6},
             "six state backups went to the budget: " + backup::Describe(report));
  c.ExpectEq(report.pruned, std::size_t{6}, "and nothing else went");
  for (std::size_t i = 0; i < traded.size(); ++i) {
    const bool gone = !sandbox.Exists(traded[i].sd_path);
    c.Expect(gone == (i < 6), "trade " + std::to_string(i) + (i < 6 ? " went" : " stayed"));
  }
  c.Expect(sandbox.Exists(other_old.sd_path) && sandbox.Exists(other_new.sd_path),
           "another state's newest three stay, however old");
  for (const Seeded& save : saves) {
    c.ExpectEq(sandbox.Read(save.sd_path), save.bytes, "no save backup is touched");
  }
  c.ExpectEq(report.state_bytes_after, 256 * kMiB, "the state backups are down to the budget");
  c.Expect(!report.over_state_budget, "which the report says");
  c.Expect(backup::DescribeStateBudget(report).rfind("6 ", 0) == 0,
           "and a log line of its own says so: " + backup::DescribeStateBudget(report));
  c.Expect(backup::Describe(report).find("budget") != std::string::npos,
           "as does the prune's own: " + backup::Describe(report));
}

/// With the budget off -- or nothing known to be a state -- a sole copy is
/// never removed: #330's behaviour, exactly.
void StateBudgetOff(checks::Checks& c) {
  for (const bool off_by_setting : {true, false}) {
    Sandbox sandbox(c, off_by_setting ? "prune-state-budget-zero" : "prune-state-budget-unknown");
    state::Baseline baseline;
    KnowState(&baseline, 4, "Game.state");
    std::vector<Seeded> traded;
    for (int i = 0; i < 8; ++i) {
      traded.push_back(SeedLarge(sandbox, 4, "Game.state", kNow - (400 - i) * kDay, i, 4096));
    }
    backup::PruneOptions options;
    options.backup_dir = kBackupDir;
    options.retention.keep = 1;
    options.retention.keep_days = 0;
    options.retention.max_bytes = 1;
    options.retention.max_state_bytes = off_by_setting ? 0 : 1;
    options.retention.keep_states = 1;
    options.now = []() { return Moment(kNow); };
    if (off_by_setting) options.is_state_backup = backup::StateBackupsOf(baseline);
    const std::unique_ptr<fs::FileSystem> files =
        host::MakeNativeFileSystem(sandbox.root().string());
    const backup::PruneReport report = backup::Prune(*files, options);
    const std::string why = off_by_setting ? "max_state_mib = 0" : "no state known";
    c.ExpectEq(report.pruned, std::size_t{0}, why + ": nothing unique went");
    c.ExpectEq(report.budget_pruned, std::size_t{0}, why + ": not by the budget either");
    for (const Seeded& seeded : traded) {
      c.Expect(sandbox.Exists(seeded.sd_path), why + ": every sole copy stays");
    }
  }
}

/// The budget is the one removal without a second copy, so what it counts is
/// what `state.db` says a state download or restore wrote -- never a name that
/// merely looks like one.
void StateBudgetNames(checks::Checks& c) {
  Sandbox sandbox(c, "prune-state-budget-names");
  state::Baseline baseline;
  KnowState(&baseline, 4, "Game.state");
  KnowState(&baseline, 4, "Game.state.auto");  // RetroArch's Auto: `.auto` is the extension
  // Rom 6: a state and, from another client, a save whose slot is spelled the
  // way that state's backups are. Their backups cannot be told apart, so the
  // budget takes neither.
  KnowState(&baseline, 6, "Clash.state");
  KnowSave(&baseline, 6, "state-Clash");

  const auto seed_run = [&sandbox](std::int64_t rom_id, const std::string& discriminator,
                                   const std::string& file_name, std::int64_t from) {
    std::vector<Seeded> run;
    for (int i = 0; i < 3; ++i) {
      run.push_back(Seed(sandbox, rom_id, discriminator, from + i * kDay,
                         Bytes(static_cast<int>(rom_id * 10 + i), 4096) + discriminator +
                             file_name + std::to_string(from),
                         0, file_name));
    }
    return run;
  };
  const std::int64_t since = kNow - 500 * kDay;
  const std::vector<Seeded> known = seed_run(4, "state-Game", "Game.state", since + 10 * kDay);
  const std::vector<Seeded> auto_slot =
      seed_run(4, sync::StateBackupDiscriminator("Game.state.auto"), "Game.state.auto",
               since + 20 * kDay);
  const std::vector<Seeded> clash = seed_run(6, "state-Clash", "Clash.state", since + 30 * kDay);
  // A save slot that happens to read `state-Game`, under a save's extension,
  // and newer than every state backup of its group.
  const std::vector<Seeded> lookalike = seed_run(4, "state-Game", "game.srm", since + 40 * kDay);
  // A state `state.db` has no row for -- deleted since, or never this server's.
  const std::vector<Seeded> unknown = seed_run(7, "state-Gone", "Gone.state", since + 50 * kDay);
  // `Game.state`'s backups from before this console was bound to this server,
  // where rom 4 was some other game.
  const std::vector<Seeded> before = seed_run(4, "state-Game", "Game.state", since - 10 * kDay);

  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention.keep = 1;
  options.retention.keep_days = 0;
  options.retention.max_bytes = 0;
  options.retention.max_state_bytes = 1;
  options.retention.keep_states = 1;
  options.now = []() { return Moment(kNow); };
  options.server_since = since;
  options.is_state_backup = backup::StateBackupsOf(baseline);
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  const backup::PruneReport report = backup::Prune(*files, options);

  const auto left = [&sandbox](const std::vector<Seeded>& run) {
    std::size_t count = 0;
    for (const Seeded& seeded : run) count += sandbox.Exists(seeded.sd_path) ? 1 : 0;
    return count;
  };
  c.ExpectEq(left(known), std::size_t{1},
             "a known state's backups go to its newest: " + backup::Describe(report));
  c.Expect(sandbox.Exists(known.back().sd_path), "which is the one that stays");
  c.ExpectEq(left(auto_slot), std::size_t{1}, "and so do RetroArch's Auto state's");
  c.ExpectEq(left(clash), clash.size(), "a state a save's slot shadows is not taken");
  c.ExpectEq(left(lookalike), lookalike.size(),
             "nor a save backup whose slot reads like a state's");
  c.ExpectEq(left(unknown), unknown.size(), "nor a state state.db does not know");
  c.ExpectEq(left(before), before.size(), "nor one from before this server");
  c.ExpectEq(report.budget_pruned, std::size_t{4}, "four in all");
  c.Expect(report.over_state_budget, "and what is left is still over a 1-byte budget");

  // The predicate itself, at its edges.
  const backup::IsStateBackup is_state = backup::StateBackupsOf(baseline);
  c.Expect(is_state != nullptr, "a state.db with states answers");
  if (is_state == nullptr) return;
  const auto parsed = [](const std::string& name) { return *backup::ParseBackupName(name); };
  c.Expect(is_state(parsed("4-state-Game-1757000000.state"), ".state"), "a state's backup is one");
  c.Expect(!is_state(parsed("4-state-Game-1757000000.srm"), ".srm"),
           "the same discriminator under a save's extension is not");
  c.Expect(!is_state(parsed("5-state-Game-1757000000.state"), ".state"), "nor under another rom");
  c.Expect(!is_state(parsed("4-autosave-1757000000.srm"), ".srm"), "nor a save's");
  const backup::IsStateBackup none = backup::StateBackupsOf(state::Baseline{});
  c.Expect(none == nullptr || !none(parsed("4-state-Game-1757000000.state"), ".state"),
           "and an empty state.db knows no state at all");
}

/// Every power cut of a budget prune leaves a card holding all it held before
/// but some of what the whole prune removed: never a backup the prune would
/// have kept, and never part of a file.
void StateBudgetCrash(checks::Checks& c) {
  Sandbox sandbox(c, "prune-state-budget-crash");
  state::Baseline baseline;
  KnowState(&baseline, 4, "Game.state");
  for (int i = 0; i < 5; ++i) {
    Seed(sandbox, 4, "state-Game", kNow - (10 - i) * kDay, Bytes(i, 512), 0, "Game.state");
  }
  Seed(sandbox, 4, "autosave", kNow - 300 * kDay, Bytes(20, 512));
  Seed(sandbox, 4, "autosave", kNow - 200 * kDay, Bytes(21, 512));

  backup::PruneOptions options;
  options.backup_dir = kBackupDir;
  options.retention.keep = 1;
  options.retention.keep_days = 0;
  options.retention.max_bytes = 0;
  options.retention.max_state_bytes = 2 * 512;
  options.retention.keep_states = 1;
  options.now = []() { return Moment(kNow); };
  options.is_state_backup = backup::StateBackupsOf(baseline);

  std::set<std::string> before;
  for (const auto& entry : std::filesystem::directory_iterator(sandbox.Host(kBackupDir))) {
    before.insert(entry.path().filename().string());
  }
  const std::unique_ptr<fs::FileSystem> files =
      host::MakeNativeFileSystem(sandbox.root().string());
  crash::Recorder recorder(sandbox.root(), {});
  const backup::PruneReport report = backup::Prune(*files, options);
  c.ExpectEq(report.budget_pruned, std::size_t{3},
             "the budget took three: " + backup::Describe(report));
  std::set<std::string> removed;
  for (const std::string& name : before) {
    if (!sandbox.Exists(std::string(kBackupDir) + "/" + name)) {
      removed.insert(crash::RelativeOf(kBackupDir) + "/" + name);
    }
  }
  c.ExpectEq(removed.size(), std::size_t{3}, "three files left the card");

  const crash::Judge subset = [removed](const crash::Tree& after,
                                        const crash::Snapshot& prior) -> std::string {
    for (const auto& [path, bytes] : prior.files) {
      if (removed.count(path) != 0) continue;
      const auto found = after.find(path);
      if (found == after.end()) return path + " is gone, and the prune would have kept it";
      if (found->second != bytes) return path + " holds other bytes than it did";
    }
    for (const auto& [path, bytes] : after) {
      if (prior.files.count(path) == 0) return path + " appeared from nowhere";
    }
    return {};
  };
  const crash::Verdict verdict = crash::Replay(recorder, {kBackupDir}, {kBackupDir},
                                               rig::ScratchDir() + "/crash-prune-budget", subset);
  c.Expect(verdict.unmodelled.empty(), "the model followed every call: " + verdict.unmodelled);
  c.Expect(verdict.points > 1, "the prune made calls to cut");
  c.ExpectEq(verdict.failures, std::size_t{0},
             "no power cut removes more than the prune would: " + verdict.first);
}

void StateProven(checks::Checks& c) {
  state::Baseline baseline;
  const auto row = [&baseline](std::int64_t rom_id, const std::string& name) {
    baseline.SetState(Paired(Listed(rom_id, rom_id, name, 64, kNow), Bytes(int(rom_id))));
  };
  const auto did = [](std::int64_t rom_id, const std::string& name, sync::StateOutcome outcome) {
    sync::StateOperationResult operation;
    operation.rom_id = rom_id;
    operation.file_name = name;
    operation.outcome = outcome;
    return operation;
  };
  row(1, "settled.state");
  row(2, "downloaded.state");
  row(3, "uploaded.state");
  row(4, "kept.state");
  row(5, "untouched.state");
  sync::StateSyncReport report;
  report.ran = true;
  report.operations = {did(1, "settled.state", sync::StateOutcome::kNoOp),
                       did(2, "downloaded.state", sync::StateOutcome::kDownloaded),
                       did(3, "uploaded.state", sync::StateOutcome::kUploaded),
                       did(4, "kept.state", sync::StateOutcome::kKeptBoth),
                       did(9, "never.state", sync::StateOutcome::kNoOp)};

  const state::Baseline proven = backup::ProvenStates(baseline, report);
  c.Expect(proven.FindState(1, "settled.state") != nullptr, "a row found settled is a proof");
  c.Expect(proven.FindState(2, "downloaded.state") != nullptr, "and one a download just wrote");
  c.Expect(proven.FindState(3, "uploaded.state") == nullptr,
           "an upload's row is not, until a tick finds it settled");
  c.Expect(proven.FindState(4, "kept.state") == nullptr, "nor a kept-both one");
  c.Expect(proven.FindState(5, "untouched.state") == nullptr, "nor one this tick never saw");
  c.ExpectEq(proven.size(), std::size_t{2}, "and nothing else");

  report.ran = false;
  c.Expect(backup::ProvenStates(baseline, report).empty(),
           "a states half that did not run confirms nothing");
}

void Server(rig::Checks& c, http::HttpClient& client, const std::string& base) {
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    c.Expect(false, "no fixture credentials");
    return;
  }
  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "gb240p.gb", &rom)) {
    c.Expect(false, "the fixture library has no gb240p.gb; re-seed it");
    return;
  }
  const std::string bytes = "a save the prune asks the server about " + harness::UniqueSlot("p");
  const std::string local = rig::ScratchDir() + "/prune-server.srm";
  rig::WriteFile(local, bytes);
  harness::Save saved;
  c.Expect(harness::UploadSave(client, base, fixture, rom.id, harness::UniqueSlot("prune"),
                               "prune-probe", local, "prune.srm", false, &saved),
           "the fixture save was uploaded");

  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  sync::CallPolicy policy;
  policy.max_attempts = 1;
  const std::optional<std::vector<std::string>> digests =
      backup::ListServerSaveDigests(client, token, rom.id, policy);
  c.Expect(digests.has_value(), "the rom's saves were listed");
  if (digests) {
    c.Expect(std::find(digests->begin(), digests->end(), crypto::Md5Hex(bytes)) != digests->end(),
             "and the uploaded save's MD5 is among them");
  }

  // A truncated listing proves nothing.
  // `base` is the fault proxy's (rig.hpp).
  rig::ArmFault(client, base, R"({"mode":"truncate","path":"/api/saves?rom_id=","bytes":40,"count":1})");
  c.Expect(!backup::ListServerSaveDigests(client, token, rom.id, policy).has_value(),
           "a truncated listing is not a listing");
  rig::DisarmFault(client, base);

  harness::DeleteSave(client, base, fixture, saved.id);
}


/// A state's digest is the client's own, and it proves the server's copy only
/// while the row has not moved -- against the real upsert, not a model of it.
void StateServer(rig::Checks& c, http::HttpClient& client, const std::string& base) {
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    c.Expect(false, "no fixture credentials");
    return;
  }
  harness::Rom rom;
  if (!harness::FindRom(client, base, fixture, "gb240p.gb", &rom)) {
    c.Expect(false, "the fixture library has no gb240p.gb; re-seed it");
    return;
  }
  const std::string name = harness::UniqueSlot("prune") + ".state";
  const std::string bytes = "a state the prune asks the server about " + name;
  const std::string local = rig::ScratchDir() + "/prune-server.state";
  rig::WriteFile(local, bytes);
  sync::ServerState row;
  if (!harness::UploadState(c, client, base, fixture, rom.id, "retroarch", local, name, &row)) {
    return;
  }

  auth::StoredToken token;
  token.server_url = base;
  token.access_token = fixture.token;
  token.device_id = fixture.device_id;
  sync::CallPolicy policy;
  policy.max_attempts = 1;
  const std::string digest = crypto::Md5Hex(bytes);
  const auto lists = [&digest](const std::optional<std::vector<std::string>>& digests) {
    return digests && std::find(digests->begin(), digests->end(), digest) != digests->end();
  };

  const state::Baseline none;
  const std::optional<std::vector<std::string>> nothing =
      backup::ListServerStateDigests(client, token, rom.id, none, policy);
  c.Expect(nothing && nothing->empty(), "a rom with no state row has nothing to prove");

  state::Baseline baseline;
  baseline.SetState(Paired(row, bytes));
  c.Expect(lists(backup::ListServerStateDigests(client, token, rom.id, baseline, policy)),
           "the uploaded state's MD5 is proven by its unmoved row");
  c.Expect(lists(backup::ListProvenServerDigests(client, token, rom.id, &baseline, policy)),
           "and is among the digests the engine hands the prune");
  c.Expect(!lists(backup::ListProvenServerDigests(client, token, rom.id, nullptr, policy)),
           "which the save listing alone does not prove");

  // A truncated listing proves nothing.
  rig::ArmFault(client, base,
                R"({"mode":"truncate","path":"/api/states?rom_id=","bytes":40,"count":1})");
  c.Expect(!backup::ListServerStateDigests(client, token, rom.id, baseline, policy).has_value(),
           "a truncated state listing is not a listing");
  rig::DisarmFault(client, base);

  // Another console's upload under the same name replaces the row in place.
  const std::string replaced = bytes + " and then more progress from another console";
  rig::WriteFile(local, replaced);
  sync::ServerState moved;
  if (harness::UploadState(c, client, base, fixture, rom.id, "retroarch", local, name, &moved)) {
    c.ExpectEq(moved.id, row.id, "the upsert kept the row");
    c.Expect(!lists(backup::ListServerStateDigests(client, token, rom.id, baseline, policy)),
             "and the row that moved no longer proves the bytes it replaced");
  }
  harness::DeleteState(client, base, fixture, row.id);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "count";
  if (scenario != "server") {
    checks::Checks checks;
    if (scenario == "names") {
      Names(checks);
    } else if (scenario == "count") {
      Count(checks);
    } else if (scenario == "age") {
      Age(checks);
    } else if (scenario == "cap") {
      Cap(checks);
    } else if (scenario == "unique") {
      Unique(checks);
    } else if (scenario == "duplicate") {
      Duplicate(checks);
    } else if (scenario == "truncated") {
      Truncated(checks);
    } else if (scenario == "crash") {
      Crash(checks);
    } else if (scenario == "other_server") {
      OtherServer(checks);
    } else if (scenario == "states") {
      States(checks);
    } else if (scenario == "state_moved") {
      StateMoved(checks);
    } else if (scenario == "state_crash") {
      StateCrash(checks);
    } else if (scenario == "state_proven") {
      StateProven(checks);
    } else if (scenario == "state_budget") {
      StateBudget(checks);
    } else if (scenario == "state_budget_off") {
      StateBudgetOff(checks);
    } else if (scenario == "state_budget_names") {
      StateBudgetNames(checks);
    } else if (scenario == "state_budget_crash") {
      StateBudgetCrash(checks);
    } else {
      std::cerr << "unknown scenario " << scenario << "\n";
      return 2;
    }
    if (checks.failures() == 0) std::cout << "prune." << scenario << " ok\n";
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::string base = rig::BaseUrl();
  rig::Checks checks;
  const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  Server(checks, *client, base);
  StateServer(checks, *client, base);
  if (checks.failures() == 0) std::cout << "prune.server ok\n";
  return checks.failures() == 0 ? 0 : 1;
}

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
//   server     against the real RomM: a save's digest is listed for its rom
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
  if (checks.failures() == 0) std::cout << "prune.server ok\n";
  return checks.failures() == 0 ? 0 : 1;
}

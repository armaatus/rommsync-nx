// The host test harness: the real engine, a real RomM, and an SD card made of
// a temp directory.
//
// `rig.hpp` is about *HTTP* against this worktree's fixture -- it logs in, it
// arms a fault, it scrapes a field out of a body. This file is the layer above
// it: the pieces a test needs to run engine code end to end and then prove it
// left the world as it found it.
//
// Three things live here, and each one exists because leaving it to every test
// to remember is how the guarantee quietly stops being checked:
//
//   `Sandbox`  -- a per-test directory standing in for the SD card, mapping the
//                 SD-root paths the engine actually names (`/config/rommsync`,
//                 `/retroarch/saves`) onto it. It is removed when the test ends,
//                 so ordering cannot matter. Crucially it also *audits* itself:
//                 a save whose bytes changed with no backup of the previous
//                 bytes beside it is a failure, whether or not the test thought
//                 to look. That is docs/SYNC_PROTOCOL.md's one hard rule, made
//                 structural rather than remembered.
//
//   `Fault`    -- an armed fault-proxy scenario that disarms itself. A test that
//                 returns early past a `DisarmFault` leaves the next test's
//                 first request damaged, which is a red run in a file nobody
//                 touched. It also *checks the arm was accepted*: a typo'd spec
//                 is a 400 the proxy never applies, and the test then passes
//                 having exercised nothing.
//
//   `Fixture`  -- the credentials and the library ids provision.py minted, plus
//                 the save/negotiate calls every scenario builds on.
//
// Everything under `harness::` speaks through `rommsync::http::HttpClient` and
// `rommsync::json`, never through a second copy of the field names -- the point
// of the harness is that the engine's own code is what runs.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>  // getpid: one sandbox per process, and the suite is POSIX

#include "rig.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"

namespace harness {

namespace http = rommsync::http;
namespace json = rommsync::json;
namespace sync = rommsync::sync;

// --- SD-card paths -----------------------------------------------------------
//
// The engine names these, not the harness: `config.hpp`'s folder map is
// SD-root-absolute (`/retroarch/saves`), and docs/ARCHITECTURE.md puts the
// client's own state under `/config/rommsync`. `Sandbox` is the platform layer's
// job done for a test -- turning one of those into a real path.

inline constexpr const char* kConfigDir = "/config/rommsync";
inline constexpr const char* kBackupDir = "/config/rommsync/.backup";
inline constexpr const char* kSavesDir = "/retroarch/saves";

// --- the sandbox --------------------------------------------------------------

/// A temp directory standing in for the SD card, for exactly one test.
///
/// Per-test and torn down, so no test can leak state into the next one and a
/// failure cannot be an artefact of what ran before it. Set
/// `ROMMSYNC_KEEP_SANDBOX=1` to keep it and have the path printed, which is the
/// only way to look at what a red run actually wrote.
class Sandbox {
 public:
  /// `checks` receives the teardown audit, so a test that overwrites a save
  /// without backing it up goes red without having to remember to look.
  ///
  /// **That only works if `checks` outlives the sandbox**, which is why every
  /// scenario in test_harness.cpp takes its `Checks` from `main` and returns
  /// void rather than `checks.failures()`. A return expression is evaluated
  /// before block-scope destructors run, so a scenario that returned its own
  /// count would copy the number out first and let this audit raise a failure
  /// into a value nobody reads -- printing FAIL and exiting 0.
  Sandbox(::checks::Checks& checks, std::string_view name) : checks_(&checks) {
    // The build tree, not /tmp: three worktrees run this suite at once and a
    // shared path would have them delete each other's sandboxes. The pid and a
    // counter separate two sandboxes inside one process, which
    // `ctest --repeat until-fail:N` produces.
    static int serial = 0;
    root_ = std::filesystem::path(rig::ScratchDir()) / "sandbox" /
            (std::string(name) + "-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(serial++));
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_, error);
    MakeDirs(kConfigDir);
    MakeDirs(kBackupDir);
    MakeDirs(kSavesDir);
  }

  ~Sandbox() {
    if (audit_) {
      Audit(*checks_);
    }
    if (const char* keep = std::getenv("ROMMSYNC_KEEP_SANDBOX"); keep != nullptr && *keep != '\0') {
      std::cerr << "  sandbox kept at " << root_.string() << "\n";
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;

  /// Do not audit on teardown. Only for the test of the audit itself, which
  /// deliberately leaves an unbacked overwrite behind and checks it is caught.
  void Detach() { audit_ = false; }

  const std::filesystem::path& root() const { return root_; }

  /// The real path an SD-root path resolves to. `/retroarch/saves/Game.srm`
  /// becomes `<sandbox>/retroarch/saves/Game.srm` -- the mapping the Horizon
  /// layer performs with `sdmc:`.
  std::string Host(std::string_view sd_path) const {
    std::string relative(sd_path);
    while (!relative.empty() && relative.front() == '/') {
      relative.erase(relative.begin());
    }
    return (root_ / relative).string();
  }

  void MakeDirs(std::string_view sd_path) const {
    std::error_code error;
    std::filesystem::create_directories(Host(sd_path), error);
  }

  bool Write(std::string_view sd_path, std::string_view bytes) const {
    return rig::WriteFile(Host(sd_path), bytes);
  }

  std::string Read(std::string_view sd_path) const { return rig::ReadFile(Host(sd_path)); }

  bool Exists(std::string_view sd_path) const {
    std::error_code error;
    return std::filesystem::exists(Host(sd_path), error);
  }

  std::uintmax_t SizeOf(std::string_view sd_path) const {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(Host(sd_path), error);
    return error ? 0 : size;
  }

  /// Put a save on the card and record its bytes as the baseline the
  /// backup-before-overwrite rule is judged against.
  ///
  /// A test that writes a save with `Write` instead is not audited, which is
  /// correct: the rule is about *overwriting a save that was already there*.
  bool SeedSave(std::string_view sd_path, std::string_view bytes) {
    baselines_.push_back({std::string(sd_path), std::string(bytes)});
    return Write(sd_path, bytes);
  }

  /// The hard rule from docs/SYNC_PROTOCOL.md, checked rather than assumed:
  /// **every seeded save whose bytes are no longer the bytes it started with
  /// must have those previous bytes under `.backup/`.**
  ///
  /// It is deliberately a check on the *state left behind*, not on a call that
  /// was made. A backup written after the overwrite, or to the wrong place, or
  /// holding the new bytes, all fail here -- and so does an overwrite that was
  /// interrupted half way, because a save that is gone is a save that changed.
  /// That last case is the whole reason the fault proxy exists: the guarantee
  /// only ever matters on the failure path.
  ///
  /// Returns the number of failures reported into `into`.
  int Audit(::checks::Checks& into) const {
    const int before = into.failures();
    for (const Baseline& save : baselines_) {
      if (Exists(save.sd_path) && Read(save.sd_path) == save.bytes) {
        continue;  // untouched; there was nothing to back up
      }
      if (!HasBackupOf(save.bytes)) {
        into.Expect(false, "the save " + save.sd_path +
                               " was overwritten with no backup of its previous bytes under " +
                               kBackupDir + " -- docs/SYNC_PROTOCOL.md's hard rule");
      }
    }
    return into.failures() - before;
  }

  /// True when some file under `.backup/` holds exactly `bytes`.
  bool HasBackupOf(std::string_view bytes) const {
    std::error_code error;
    const std::filesystem::path backups = Host(kBackupDir);
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(backups, error)) {
      if (entry.is_regular_file(error) && rig::ReadFile(entry.path().string()) == bytes) {
        return true;
      }
    }
    return false;
  }

  /// Where the backup of a save goes:
  /// `/config/rommsync/.backup/<rom_id>-<unix seconds>.<ext>`, the layout
  /// docs/SYNC_PROTOCOL.md specifies. Here rather than in each test so the
  /// audit and the code under test cannot disagree about where to look.
  ///
  /// Note what that layout does *not* contain: the save's name or its slot. Two
  /// saves of the same rom -- two slots, or a save and its state -- backed up in
  /// the same second produce the same path, and the second backup destroys the
  /// first. This helper is deliberately faithful to the documented scheme rather
  /// than quietly fixing it, because the fix is M2-5's to make; see the note in
  /// docs/SYNC_PROTOCOL.md.
  std::string BackupPathFor(std::int64_t rom_id, std::string_view file_name) const {
    const std::size_t dot = file_name.rfind('.');
    const std::string extension =
        dot == std::string_view::npos ? std::string() : std::string(file_name.substr(dot));
    const std::int64_t now = sync::UnixSeconds(std::chrono::system_clock::now());
    return std::string(kBackupDir) + "/" + std::to_string(rom_id) + "-" + std::to_string(now) +
           extension;
  }

 private:
  struct Baseline {
    std::string sd_path;
    std::string bytes;
  };

  std::filesystem::path root_;
  ::checks::Checks* checks_;
  std::vector<Baseline> baselines_;
  bool audit_ = true;
};

// --- the fault proxy, scoped --------------------------------------------------

/// One armed fault-proxy scenario, disarmed when the scope ends.
///
/// Acceptance criterion in its own right (issue M0-5): a test that returns early
/// past a manual disarm leaves the *next* test's first request damaged, and the
/// red run then names a file nobody touched.
class Fault {
 public:
  Fault(::checks::Checks& checks, http::HttpClient& client, const std::string& base,
        const std::string& spec)
      : client_(&client), base_(base) {
    const http::Result armed = rig::ArmFault(client, base, spec);
    // A refused spec is the failure worth being loud about: the proxy answers
    // 400 and forwards everything untouched, so the scenario runs clean and the
    // test passes having exercised nothing at all.
    checks.Expect(armed.successful(),
                  "the fault proxy accepted the scenario " + spec + " -- got status " +
                      std::to_string(armed.response.status) + " " + armed.response.body);
  }

  ~Fault() { rig::DisarmFault(*client_, base_); }

  Fault(const Fault&) = delete;
  Fault& operator=(const Fault&) = delete;

 private:
  http::HttpClient* client_;
  std::string base_;
};

/// Assert nothing is armed. `GET /__fault` answers the armed scenario as JSON,
/// or the bare literal `null`.
///
/// Compared exactly rather than searched for. A spec is echoed back verbatim, so
/// any armed scenario carrying a JSON `null` -- `{"mode":"status","body":null}`
/// is one -- would contain the word and satisfy a substring test with the fault
/// still live. That is the one thing this helper exists to rule out.
inline void ExpectDisarmed(::checks::Checks& checks, http::HttpClient& client,
                           const std::string& base, std::string_view what) {
  http::Request request;
  request.url = base + "/__fault";
  const http::Result result = client.Send(request);
  std::string_view body = result.response.body;
  while (!body.empty() && (body.front() == ' ' || body.front() == '\n')) {
    body.remove_prefix(1);
  }
  while (!body.empty() && (body.back() == ' ' || body.back() == '\n')) {
    body.remove_suffix(1);
  }
  checks.Expect(result.successful() && body == "null",
                std::string(what) + " -- the proxy holds: " + result.response.body);
}

// --- fixture credentials ------------------------------------------------------

/// What server/testing/provision.py minted for this worktree.
struct Fixture {
  std::string token;
  std::string device_id;
};

/// Load them, or return false with the reason on stderr. A caller skips.
inline bool LoadFixture(Fixture* out) {
  const std::string provisioned = rig::ReadFile(ROMMSYNC_FIXTURE_AUTH);
  out->token = rig::FixtureValue(provisioned, "ROMM_FIXTURE_TOKEN");
  out->device_id = rig::FixtureValue(provisioned, "ROMM_FIXTURE_DEVICE_ID");
  if (out->token.empty() || out->device_id.empty()) {
    std::cerr << "no fixture credentials in " ROMMSYNC_FIXTURE_AUTH
                 "\n  provision them with: ./.venv/bin/python server/testing/provision.py\n";
    return false;
  }
  return true;
}

// --- authenticated calls ------------------------------------------------------

inline http::Request Authed(http::Method method, const std::string& url, const Fixture& fixture) {
  http::Request request;
  request.method = method;
  request.url = url;
  request.headers.push_back({"Authorization", "Bearer " + fixture.token});
  return request;
}

inline http::Result PostJson(http::HttpClient& client, const std::string& url,
                             const Fixture& fixture, std::string body) {
  http::Request request = Authed(http::Method::kPost, url, fixture);
  request.headers.push_back({"Content-Type", "application/json"});
  request.body = std::move(body);
  return client.Send(request);
}

// --- reading a plan -----------------------------------------------------------

/// The operation the server planned for `slot`, or nullptr.
///
/// Negotiate also reports saves a test never mentioned -- anything this device
/// has no history for -- so every assertion has to be scoped to the slot the run
/// created rather than to `operations[0]`.
inline const json::Value* OperationFor(const json::Value& plan, const std::string& slot) {
  const json::Value* operations = plan.Find("operations");
  if (operations == nullptr) {
    return nullptr;
  }
  for (const json::Value& operation : operations->elements()) {
    const json::Value* value = operation.Find("slot");
    if (value != nullptr && value->is_string() && value->string() == slot) {
      return &operation;
    }
  }
  return nullptr;
}

inline std::string Field(const json::Value& object, const char* key) {
  const json::Value* value = object.Find(key);
  return value != nullptr && value->is_string() ? value->string() : std::string();
}

inline std::int64_t Number(const json::Value& object, const char* key) {
  const json::Value* value = object.Find(key);
  return value != nullptr && value->is_integer() ? value->integer() : 0;
}

/// A slot nobody else is using. RomM pairs saves on `(rom_id, slot)`, so a
/// constant would make one run's leftovers another run's sync history -- and a
/// run that failed before its cleanup does leave one behind. A timestamp alone
/// is not enough: several of these finish inside one second.
///
/// `label` is the caller's own, issue and scenario, so a slot left on the
/// fixture by a failed run says which test to go and look at.
inline std::string UniqueSlot(std::string_view label) {
  std::random_device entropy;
  const std::int64_t now = sync::UnixSeconds(std::chrono::system_clock::now());
  return std::string(label) + "-" + std::to_string(now) + "-" + std::to_string(entropy());
}

// --- the library --------------------------------------------------------------

/// Percent-encode one path segment. A rom's `fs_name` is a file name a human
/// chose -- `Synthetic Two Disc Game` -- and it goes into a URL path, so the
/// spaces have to be encoded or libcurl sends a request line no server parses.
inline std::string UrlEncode(std::string_view segment) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const char character : segment) {
    const unsigned char byte = static_cast<unsigned char>(character);
    const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (unreserved) {
      out.push_back(character);
    } else {
      out.push_back('%');
      out.push_back(kHex[byte >> 4]);
      out.push_back(kHex[byte & 0x0F]);
    }
  }
  return out;
}

struct RomFile {
  std::int64_t id = 0;
  std::string file_name;
  std::int64_t size = 0;
};

/// One rom, as the download worker will read it.
struct Rom {
  std::int64_t id = 0;
  std::string fs_name;
  std::int64_t size = 0;

  /// RomM's own multi-file signal. It is on the *list* schema as well as the
  /// detail one, so a client can skip without a second call per rom.
  bool has_multiple_files = false;

  std::vector<RomFile> files;

  /// `GET` path for the whole rom. For a multi-file rom this is a zip RomM
  /// builds on the fly -- see the `multifile` scenario.
  std::string ContentPath() const {
    return "/api/roms/" + std::to_string(id) + "/content/" + UrlEncode(fs_name);
  }
};

/// Find a rom by its `fs_name`, with its files. Returns false when the library
/// has no such rom, which means the fixture was not seeded.
inline bool FindRom(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                    std::string_view fs_name, Rom* out) {
  const http::Result listed =
      client.Send(Authed(http::Method::kGet, base + "/api/roms?limit=200", fixture));
  if (!listed.successful()) {
    return false;
  }
  const json::ParseResult document = json::Parse(listed.response.body);
  const json::Value* items = document.ok() ? document.value.Find("items") : nullptr;
  if (items == nullptr) {
    return false;
  }
  for (const json::Value& item : items->elements()) {
    if (Field(item, "fs_name") != fs_name) {
      continue;
    }
    out->id = Number(item, "id");
    out->fs_name = fs_name;
    out->size = Number(item, "fs_size_bytes");
    const json::Value* multi = item.Find("has_multiple_files");
    out->has_multiple_files = multi != nullptr && multi->boolean();
    if (out->id == 0) {
      return false;
    }
    // `files` is present on the list schema and always empty there; only the
    // detail endpoint fills it. `has_multiple_files` above is the exception and
    // is the one that matters, because it means a client can decide to skip a
    // multi-file rom without a second call per rom.
    const http::Result detailed = client.Send(
        Authed(http::Method::kGet, base + "/api/roms/" + std::to_string(out->id), fixture));
    const json::ParseResult rom = json::Parse(detailed.response.body);
    const json::Value* files = detailed.successful() && rom.ok() ? rom.value.Find("files") : nullptr;
    if (files != nullptr) {
      for (const json::Value& file : files->elements()) {
        out->files.push_back(
            {Number(file, "id"), Field(file, "file_name"), Number(file, "file_size_bytes")});
      }
    }
    return true;
  }
  return false;
}

// --- saves --------------------------------------------------------------------

/// One save as RomM holds it. `file_name` is the server's, which is not the
/// name that was uploaded: RomM stamps a slot upload with a datetime tag
/// (docs/SYNC_PROTOCOL.md step 2).
struct Save {
  std::int64_t id = 0;
  std::string file_name;
  std::string content_hash;
  std::string updated_at;
  std::int64_t file_size_bytes = 0;

  /// `GET` path for the bytes. No `device_id` on it: that parameter writes this
  /// device's sync history, which several scenarios below are arranging on
  /// purpose and must not have written for them.
  std::string ContentPath() const { return "/api/saves/" + std::to_string(id) + "/content"; }
};

inline bool ReadSave(const http::Result& result, Save* out) {
  if (!result.successful()) {
    return false;
  }
  const json::ParseResult document = json::Parse(result.response.body);
  if (!document.ok() || document.value.Find("id") == nullptr) {
    return false;
  }
  out->id = Number(document.value, "id");
  out->file_name = Field(document.value, "file_name");
  out->content_hash = Field(document.value, "content_hash");
  out->updated_at = Field(document.value, "updated_at");
  out->file_size_bytes = Number(document.value, "file_size_bytes");
  return true;
}

/// `POST /api/saves`, streaming `local_path` as the `saveFile` part.
///
/// `with_device` decides whether this upload writes the device's sync history,
/// which is the single input that separates the two conflict reasons a client
/// has to handle. Passing it is not cosmetic: without a sync row RomM compares
/// timestamps, with one it compares both sides against the row.
inline bool UploadSave(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                       std::int64_t rom_id, const std::string& slot, const std::string& emulator,
                       const std::string& local_path, const std::string& file_name,
                       bool with_device, Save* out) {
  std::string url = base + "/api/saves?rom_id=" + std::to_string(rom_id) + "&emulator=" + emulator +
                    "&slot=" + slot + "&overwrite=true";
  if (with_device) {
    url += "&device_id=" + fixture.device_id;
  }
  http::Request request = Authed(http::Method::kPost, url, fixture);
  http::FormPart part;
  part.name = "saveFile";
  part.file_path = local_path;
  part.file_name = file_name;
  part.content_type = "application/octet-stream";
  request.form.push_back(part);
  return ReadSave(client.Send(request), out);
}

/// `PUT /api/saves/{id}` -- replace the bytes of a save **in place**.
///
/// The difference from another `POST` matters and is not obvious: a slot upload
/// gets a datetime tag in its file name, so a second `POST` a second later is a
/// *new* save row with no sync history, whatever `overwrite` says. Only the PUT
/// moves the same row forward, which is the only way to arrange "the server's
/// copy changed since this device last synced it".
inline bool ReplaceSave(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                        std::int64_t save_id, const std::string& local_path,
                        const std::string& file_name, Save* out) {
  http::Request request =
      Authed(http::Method::kPut, base + "/api/saves/" + std::to_string(save_id), fixture);
  http::FormPart part;
  part.name = "saveFile";
  part.file_path = local_path;
  part.file_name = file_name;
  part.content_type = "application/octet-stream";
  request.form.push_back(part);
  return ReadSave(client.Send(request), out);
}

inline void DeleteSave(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                       std::int64_t save_id) {
  if (save_id != 0) {
    PostJson(client, base + "/api/saves/delete", fixture,
             "{\"saves\":[" + std::to_string(save_id) + "]}");
  }
}

/// RomM's own MD5 of `local_path`'s bytes.
///
/// The engine has had its own MD5 since M2-3 (`crypto::Md5`, `state::HashFile`),
/// so this is no longer a stand-in for a missing digest -- it is the independent
/// oracle that one is checked against. Upload the bytes under a throwaway slot,
/// read the digest the server computed, delete the save. What matters is the
/// string RomM will compare against on every later negotiation, and only RomM
/// can say what that is; `harness.content_hash` is where the two are compared.
inline bool ServerMd5(http::HttpClient& client, const std::string& base, const Fixture& fixture,
                      std::int64_t rom_id, const std::string& local_path, std::string* out) {
  Save scratch;
  if (!UploadSave(client, base, fixture, rom_id, UniqueSlot("harness-md5"), "harness-md5", local_path,
                  "harness-md5.srm", /*with_device=*/false, &scratch)) {
    return false;
  }
  *out = scratch.content_hash;
  DeleteSave(client, base, fixture, scratch.id);
  return out->size() == sync::kContentHashDigits;
}

/// `2026-09-04T22:45:33.512340+00:00` -> a `sync::Timestamp`, whole seconds.
///
/// Whole seconds because that is what a client may send: `FormatTimestamp`
/// drops sub-second precision downwards, so a test that wants "the same
/// timestamp the server holds" has to mean the same truncation the engine
/// performs. Returns false on anything that is not that shape.
inline bool ParseServerTimestamp(std::string_view text, sync::Timestamp* out) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (text.size() < 19 ||
      std::sscanf(std::string(text.substr(0, 19)).c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month,
                  &day, &hour, &minute, &second) != 6) {
    return false;
  }
  // Howard Hinnant's days_from_civil, the inverse of the civil_from_days in
  // sync.cpp. Spelled out here for the same reason it is spelled out there: the
  // C library's UTC conversion is not available in the same shape on both
  // targets, and this file is read alongside that one.
  const std::int64_t shifted_year = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
  const std::int64_t year_of_era = shifted_year - era * 400;
  const std::int64_t day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const std::int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const std::int64_t days = era * 146097 + day_of_era - 719468;
  const std::int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second;
  *out = sync::Timestamp{} + std::chrono::seconds{seconds};
  return true;
}

/// `POST /api/sync/negotiate` with a body `EncodeNegotiateRequest` built.
///
/// The encoder is the engine's, not the harness's: a payload this refuses is a
/// bug in the test's save, and saying so here beats sending a body the server
/// reads as a different save.
inline http::Result Negotiate(::checks::Checks& checks, http::HttpClient& client,
                              const std::string& base, const Fixture& fixture,
                              const sync::SyncNegotiatePayload& payload) {
  const sync::Encoded encoded = sync::EncodeNegotiateRequest(payload);
  if (!encoded.ok()) {
    checks.Expect(false, "the payload encodes: " + encoded.error.Describe());
    return {};
  }
  return PostJson(client, base + "/api/sync/negotiate", fixture, encoded.body);
}

/// `POST /api/sync/sessions/{id}/complete`, the accounting step.
/// Close a session this scenario opened, whatever came of it.
///
/// RomM keeps ONE active sync session per device, and a `negotiate` for a device
/// that already has one has to cancel that one first. Every scenario here shares
/// the fixture's device, so a scenario that negotiates and walks away leaves a
/// session for the next one to cancel -- and that cancel-the-previous work
/// races, under load, with the session the new negotiate just created. The
/// symptom is the new session coming back CANCELLED with none of its operations
/// recorded, which is how `harness.partial` failed intermittently in CI while
/// passing every time locally (issue #76). It was the only scenario that
/// completed its session, so it was the only one that could notice.
///
/// Best effort by design: this is cleanup, and a scenario must not fail because
/// tidying up did not work. Takes the negotiate response so a caller cannot
/// close a session it did not open.
inline void CloseSession(http::HttpClient& client, const std::string& base,
                         const Fixture& fixture, const std::string& negotiate_body) {
  const json::ParseResult parsed = json::Parse(negotiate_body);
  if (!parsed.ok()) {
    return;
  }
  const std::int64_t session_id = Number(parsed.value, "session_id");
  if (session_id == 0) {
    return;
  }
  PostJson(client, base + "/api/sync/sessions/" + std::to_string(session_id) + "/complete",
           fixture, R"({"operations_completed":0,"operations_failed":0,"play_sessions":[]})");
}

/// Complete every sync session this device has left open.
///
/// The sibling of `rig::DisarmFault`, and for the same reason: what an earlier
/// run left behind damages this one. RomM keeps ONE active session per device,
/// so a `negotiate` for a device that already has one has to cancel that one
/// first -- and under load that cancel lands on the session the negotiate just
/// created instead. The symptom is a brand-new session coming back CANCELLED
/// with none of its operations recorded, which is how `harness.partial` failed
/// intermittently in CI while passing every time locally (issue #76).
///
/// Every scenario across these binaries shares the fixture's device, and each
/// runs as its own process, so the leftovers accumulate across processes. This
/// clears them at the start of each one, which is the only place that cannot be
/// forgotten by a scenario that negotiates and walks away.
///
/// Best effort by design: this is cleanup, and a scenario must not fail because
/// tidying up did not work.
inline void CloseOpenSessions(http::HttpClient& client, const std::string& base,
                              const Fixture& fixture) {
  const http::Result listed =
      client.Send(Authed(http::Method::kGet, base + "/api/sync/sessions", fixture));
  if (!listed.successful()) {
    return;
  }
  const json::ParseResult parsed = json::Parse(listed.response.body);
  if (!parsed.ok()) {
    return;
  }
  // RomM has served this both as a bare array and wrapped in `items`; take
  // whichever is there rather than pinning a shape cleanup does not depend on.
  const json::Value* sessions = &parsed.value;
  if (parsed.value.is_object()) {
    sessions = parsed.value.Find("items");
  }
  if (sessions == nullptr || !sessions->is_array()) {
    return;
  }
  for (const json::Value& session : sessions->elements()) {
    if (Field(session, "status") != "IN_PROGRESS") {
      continue;
    }
    if (Field(session, "device_id") != fixture.device_id) {
      continue;
    }
    const std::int64_t id = Number(session, "id");
    if (id == 0) {
      continue;
    }
    PostJson(client, base + "/api/sync/sessions/" + std::to_string(id) + "/complete", fixture,
             R"({"operations_completed":0,"operations_failed":0,"play_sessions":[]})");
  }
}

inline http::Result Complete(http::HttpClient& client, const std::string& base,
                             const Fixture& fixture, std::int64_t session_id,
                             int completed, int failed) {
  return PostJson(client, base + "/api/sync/sessions/" + std::to_string(session_id) + "/complete",
                  fixture,
                  "{\"operations_completed\":" + std::to_string(completed) +
                      ",\"operations_failed\":" + std::to_string(failed) +
                      ",\"play_sessions\":[]}");
}

/// One local save, as the sysmodule will build it: the fields are the engine's
/// `ClientSaveState`, so a rename in the struct breaks the harness too.
inline sync::ClientSaveState LocalSave(std::int64_t rom_id, const std::string& file_name,
                                       const std::string& slot, const std::string& emulator,
                                       const std::string& content_hash, sync::Timestamp when,
                                       std::int64_t size) {
  sync::ClientSaveState save;
  save.rom_id = rom_id;
  save.file_name = file_name;
  save.slot = slot;
  save.emulator = emulator;
  save.content_hash = content_hash;
  save.updated_at = when;
  save.file_size_bytes = size;
  return save;
}

// --- files --------------------------------------------------------------------

/// Byte-for-byte equality of two files, streamed rather than loaded.
///
/// Used instead of a hash to check a 120 MiB download, and stronger than one: a
/// resumed transfer that stitched the wrong offset together differs from the
/// original somewhere, and this says where rather than that a digest did not
/// match. The fixture is generated with a per-block index for exactly that
/// (server/testing/make_fixtures.py).
inline bool SameBytes(const std::string& left, const std::string& right,
                      std::uint64_t* first_difference) {
  std::ifstream a(left, std::ios::binary);
  std::ifstream b(right, std::ios::binary);
  if (!a || !b) {
    *first_difference = 0;
    return false;
  }
  constexpr std::size_t kChunk = 64 * 1024;
  std::string left_chunk(kChunk, '\0');
  std::string right_chunk(kChunk, '\0');
  std::uint64_t at = 0;
  while (true) {
    a.read(left_chunk.data(), static_cast<std::streamsize>(kChunk));
    b.read(right_chunk.data(), static_cast<std::streamsize>(kChunk));
    const std::streamsize got_left = a.gcount();
    const std::streamsize got_right = b.gcount();
    const std::streamsize common = std::min(got_left, got_right);
    for (std::streamsize i = 0; i < common; ++i) {
      if (left_chunk[static_cast<std::size_t>(i)] != right_chunk[static_cast<std::size_t>(i)]) {
        *first_difference = at + static_cast<std::uint64_t>(i);
        return false;
      }
    }
    if (got_left != got_right) {
      *first_difference = at + static_cast<std::uint64_t>(common);
      return false;
    }
    if (got_left == 0) {
      return true;
    }
    at += static_cast<std::uint64_t>(got_left);
  }
}

}  // namespace harness

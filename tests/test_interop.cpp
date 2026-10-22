// The interop matrix (M10-18, #244): the Switch against the other clients a
// RomM 5.4 library is shared with, each driven through RomM's real API.
//
// The Switch is the whole engine (`sysmodule::SdEngine` over a sandboxed card,
// Sync now pressed over IPC), exactly as `engine.interop` runs it. The other
// clients are what they send, not code of ours:
//
//   retroarch -- RetroArch Cloud Sync: WebDAV under `/api/sync/retroarch/`, as
//                the fixture user over Basic auth (#226 R2). mGBA, 240pee_mb.gba.
//   grout     -- a Grout-like second device: its own device row, negotiate,
//                then upload / download / confirm in the session. Gambatte,
//                gb240p.gb.
//   web       -- RomM 5.4's web player, as its bundle does it
//                (`useDeviceSaveSync`, `browserDevice`): a `client: "web"`
//                device negotiating one rom with `restore_unlisted` and its
//                core as the `emulators` filter, uploading with `autocleanup`.
//                FCEUmm, 240pee.nes. The issue named `snes9x`; the fixture has
//                no SNES rom, so the web player plays a NES one.
//
// Each of them meets the Switch in five cells, one ctest each, named
// `interop.<client>.<cell>`:
//
//   from_switch -- the Switch's save is what the other client loads
//   to_switch   -- theirs reaches the card: not while the rom is not on it
//                  (#241), then placed under Tico's name (#237), then updated
//                  over with a backup
//   conflict    -- both moved: their save on the card, the Switch's under
//                  `.backup/` and archived on the server (#240), theirs untouched
//   delete      -- a save gone from the card is no delete on the server; theirs
//                  deleted is removed from the card, backed up, never re-uploaded
//   states      -- a PSX state of their core and the Switch's DuckStation state
//                  never meet (#238, #239), whichever was first and whatever the
//                  names
//
// Every cell runs on devices of its own and deletes what it left on the server:
// the rom's `autosave` rows, its archival copies, the PSX states it named, and
// the devices. Unique bytes per run, because RomM remembers what a slot lost
// and answers `delete` to a client holding those bytes again.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "engine.hpp"
#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/log.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/token_store.hpp"

namespace {

namespace auth = rommsync::auth;
namespace crypto = rommsync::crypto;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace ipc = rommsync::ipc;
namespace json = rommsync::json;
// `rlog`, not `log`: see test_engine.cpp.
namespace rlog = rommsync::log;
namespace sync = rommsync::sync;
namespace sysmodule = rommsync::sysmodule;

constexpr const char* kSlot = "autosave";

/// Where everything a cell talks to lives.
struct Rig {
  http::HttpClient& client;
  const std::string& base;
  const harness::Fixture& fixture;
};

// --- the server, read and cleaned -------------------------------------------

/// One save row RomM holds for a rom in the shared slot.
struct SlotRow {
  std::int64_t id = 0;
  std::string emulator;
  std::string file_name;
  std::string content_hash;
  std::string updated_at;
};

std::vector<SlotRow> SlotRows(const Rig& rig, std::int64_t rom_id) {
  const http::Result listed = rig.client.Send(harness::Authed(
      http::Method::kGet, rig.base + "/api/saves?rom_id=" + std::to_string(rom_id), rig.fixture));
  const json::ParseResult parsed = json::Parse(listed.response.body);
  std::vector<SlotRow> rows;
  if (!listed.successful() || !parsed.ok() || !parsed.value.is_array()) {
    return rows;
  }
  for (const json::Value& save : parsed.value.elements()) {
    if (harness::Field(save, "slot") != kSlot) {
      continue;
    }
    rows.push_back({harness::Number(save, "id"), harness::Field(save, "emulator"),
                    harness::Field(save, "file_name"), harness::Field(save, "content_hash"),
                    harness::Field(save, "updated_at")});
  }
  return rows;
}

/// The slot's versions as `<id>@<updated_at>`, so "nothing was uploaded" is a
/// claim about rows rather than a count: RomM files an upload landing in the
/// second an existing version was tagged with onto that row (#85), which a
/// count does not see.
std::vector<std::string> Versions(const Rig& rig, std::int64_t rom_id) {
  std::vector<std::string> versions;
  for (const SlotRow& row : SlotRows(rig, rom_id)) {
    versions.push_back(std::to_string(row.id) + "@" + row.updated_at);
  }
  return versions;
}

std::string Listed(const std::vector<std::string>& versions) {
  std::string out;
  for (const std::string& version : versions) {
    out += (out.empty() ? "" : ", ") + version;
  }
  return "[" + out + "]";
}

/// Delete what the shared slot and the archive hold for `rom_id`: before a cell,
/// an earlier run's leftovers; after it, its own.
void ClearRom(const Rig& rig, std::int64_t rom_id) {
  for (const SlotRow& row : SlotRows(rig, rom_id)) {
    harness::DeleteSave(rig.client, rig.base, rig.fixture, row.id);
  }
  harness::DeleteArchivalCopies(rig.client, rig.base, rig.fixture, rom_id);
}

/// A device row RomM holds for a cell, deleted when the cell is done with it.
class DeviceRow {
 public:
  DeviceRow(const Rig& rig, std::string id) : rig_(rig), id_(std::move(id)) {}
  ~DeviceRow() { harness::DeleteDevice(rig_.client, rig_.base, rig_.fixture, id_); }
  DeviceRow(const DeviceRow&) = delete;
  DeviceRow& operator=(const DeviceRow&) = delete;

  const std::string& id() const { return id_; }

 private:
  const Rig& rig_;
  std::string id_;
};

// --- the Switch ---------------------------------------------------------------

/// Which rom a client plays, and where the Switch keeps it and its save: Tico's
/// layout, Tico being what every one of these systems is played with (#237 D5).
struct Game {
  const char* fs_name;
  const char* slug;
  const char* emulator;     ///< the core both sides run: RomM's id
  const char* core_folder;  ///< RetroArch's folder for it
  std::int64_t rom_id = 0;

  std::string Stem() const {
    const std::string name(fs_name);
    return name.substr(0, name.rfind('.'));
  }
  std::string SwitchSave() const {
    return std::string("/tico/saves/") + slug + "/" + Stem() + ".srm";
  }
  std::string SwitchRom() const { return std::string("/tico/roms/") + slug + "/" + fs_name; }
};

/// How many `sync.tick` lines the log has written since this was installed --
/// counted at the sink, for `engine.interop`'s reason (`TickLines` there).
class TickLines final : public rlog::Sink {
 public:
  TickLines() : previous_(rlog::GetSink()) { rlog::SetSink(this); }
  ~TickLines() override { rlog::SetSink(previous_); }
  TickLines(const TickLines&) = delete;
  TickLines& operator=(const TickLines&) = delete;

  void Write(rlog::Level, std::string_view line) override {
    if (line.find(" sync.tick ") != std::string_view::npos) {
      ticks_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  int ticks() const { return ticks_.load(std::memory_order_relaxed); }

 private:
  rlog::Sink* previous_;
  std::atomic<int> ticks_{0};
};

/// A Switch: its card, its engine on a device of its own, and the service the
/// overlay talks to. No tick of its own -- each is a press, run to its end.
///
/// The members are declared in reverse teardown order: the worker stops before
/// the card it reads and the device it ticks as go, and the card's backup audit
/// runs last.
class Switch {
 public:
  Switch(rig::Checks& c, const Rig& rig, const std::string& name, bool states)
      : sandbox(c, name),
        checks_(c),
        device_(rig, harness::CreateDevice(c, rig.client, rig.base, rig.fixture, name)) {
    directory_ = sandbox.Host(harness::kConfigDir) + "/";
    sandbox.Write("/config/rommsync/config.ini",
                  "[server]\nurl = " + rig.base +
                      "\n\n[sync]\nenabled = true\ninterval_min = 0\non_boot = false\n"
                      "states = " +
                      (states ? "true" : "false") + "\n");
    auth::StoredToken token;
    token.server_url = rig.base;
    token.access_token = rig.fixture.token;
    token.device_id = device_.id();
    token.scopes = {"roms.read"};
    c.Expect(auth::SaveToken(directory_ + auth::kTokenFileName, token).ok(),
             name + ": the Switch is paired as its own device");
    card_ = rommsync::host::MakeNativeFileSystem(sandbox.root().string());
    engine_.Load(directory_);
    core_ = std::make_unique<ipc::ServiceCore>(engine_);
    engine_.UseServer(&rig.client, rig.fixture.token);
    engine_.UseCard(card_.get());
    engine_.StartWorker();
  }

  Switch(const Switch&) = delete;
  Switch& operator=(const Switch&) = delete;

  /// Press Sync now and wait for that tick to end, which must be clean.
  void Tick(const std::string& what) {
    const int before = ticks_.ticks();
    std::string response;
    Call(ipc::Command::kSyncNow, ipc::EncodeEmpty(), &response);
    if (ipc::DecodeSyncOutcome(response).value != ipc::SyncOutcome::kAccepted) {
      checks_.Expect(false, what + ": Sync now starts a tick");
      return;
    }
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while ((ticks_.ticks() <= before || Status().sync_in_progress) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    const ipc::Status status = Status();
    checks_.Expect(ticks_.ticks() > before && !status.sync_in_progress &&
                       status.last_sync_result == ipc::SyncResult::kOk,
                   what + ": the tick ran clean -- " +
                       ipc::ToString(status.last_sync_result) + Log());
  }

  /// The log tail, off the wire, one line per line.
  std::string Log() {
    std::string response;
    Call(ipc::Command::kGetLog, ipc::EncodeLogRequest(ipc::kMaxLogLines), &response);
    std::string out;
    for (const std::string& line : ipc::DecodeLogTail(response).value.lines) {
      out += "\n    " + line;
    }
    return out;
  }

  /// True when conflicts.db holds an `event` row for `rom_id`.
  bool Recorded(std::int64_t rom_id, std::string_view event) const {
    const std::string text = sandbox.Read("/config/rommsync/conflicts.db");
    const std::string rom = "\"rom_id\":" + std::to_string(rom_id) + ",";
    const std::string kind = "\"event\":\"" + std::string(event) + "\"";
    std::size_t at = 0;
    while (at < text.size()) {
      const std::size_t end = text.find('\n', at);
      const std::string line =
          text.substr(at, end == std::string::npos ? std::string::npos : end - at);
      if (line.find(rom) != std::string::npos && line.find(kind) != std::string::npos) {
        return true;
      }
      if (end == std::string::npos) {
        break;
      }
      at = end + 1;
    }
    return false;
  }

  /// The game is on the card, so negotiate names it (#241).
  void HoldRom(const Game& game) {
    sandbox.MakeDirs(std::string("/tico/roms/") + game.slug);
    sandbox.Write(game.SwitchRom(), "the rom\n");
  }

  /// The player's save, judged by the backup audit from here on.
  void Played(const std::string& sd_path, const std::string& bytes) {
    sandbox.MakeDirs(sd_path.substr(0, sd_path.rfind('/')));
    sandbox.Reseed(sd_path, bytes);
  }

  harness::Sandbox sandbox;

 private:
  ipc::Status Status() {
    std::string response;
    Call(ipc::Command::kGetStatus, ipc::EncodeEmpty(), &response);
    return ipc::DecodeStatus(response).value;
  }

  void Call(ipc::Command command, const std::string& request, std::string* response) {
    ipc::Dispatch(*core_, static_cast<std::uint32_t>(command), request, response);
  }

  rig::Checks& checks_;
  TickLines ticks_;
  DeviceRow device_;
  std::string directory_;
  std::unique_ptr<fs::FileSystem> card_;
  sysmodule::SdEngine engine_;
  std::unique_ptr<ipc::ServiceCore> core_;
};

// --- the other clients --------------------------------------------------------

/// The PSX states a client writes, and when (`kClients` says which).
struct PsxState {
  const char* emulator;
  const char* core_folder;
  bool before_switch;
  bool web_names;
};

/// One save state of a core's, as the client names it.
struct StateRef {
  std::int64_t rom_id = 0;
  std::string emulator;     ///< RomM's id for the core
  std::string core_folder;  ///< RetroArch's folder for it
  std::string file_name;
};

/// The rows RomM holds for `state`'s name -- every emulator's, since a name is
/// all `POST /api/states` keys on.
std::vector<sync::ServerState> StatesNamed(const Rig& rig, std::int64_t rom_id,
                                           const std::string& file_name) {
  const http::Result listed = rig.client.Send(harness::Authed(
      http::Method::kGet, rig.base + "/api/states?rom_id=" + std::to_string(rom_id), rig.fixture));
  std::vector<sync::ServerState> named;
  const auto parsed = sync::ParseStateList(listed.response.body);
  if (!listed.successful() || !parsed.ok()) {
    return named;
  }
  for (const sync::ServerState& state : parsed.value) {
    if (state.file_name == file_name) {
      named.push_back(state);
    }
  }
  return named;
}

std::string StateBytes(const Rig& rig, std::int64_t state_id) {
  const http::Result fetched = rig.client.Send(harness::Authed(
      http::Method::kGet, rig.base + "/api/states/" + std::to_string(state_id) + "/content",
      rig.fixture));
  return fetched.successful() ? fetched.response.body : std::string();
}

/// Another client of the same RomM user, seen only through what it sends.
class OtherClient {
 public:
  virtual ~OtherClient() = default;

  /// It saves `bytes` and syncs them, its own way.
  virtual bool Save(const std::string& bytes) = 0;

  /// It syncs, then answers what it would load: empty when it has nothing.
  virtual std::string Load() = 0;

  /// The user deletes the save from it.
  virtual bool Delete() = 0;

  /// It stores a save state of `state`'s core, its own way.
  virtual bool SaveState(const StateRef& state, const std::string& bytes) = 0;

  /// What it would load for that state: empty when it has none.
  virtual std::string LoadState(const StateRef& state) = 0;
};

/// RetroArch's Cloud Sync. It has no negotiation: a `PUT` is a new `autosave`
/// version, a `GET` serves the slot's head, a `DELETE` takes every version.
class RetroArch final : public OtherClient {
 public:
  RetroArch(const Rig& rig, const Game& game) : rig_(rig), game_(game) {}

  bool Save(const std::string& bytes) override {
    return Dav(http::Method::kPut, SavePath(), bytes).successful();
  }

  std::string Load() override {
    const http::Result served = Dav(http::Method::kGet, SavePath());
    return served.successful() ? served.response.body : std::string();
  }

  bool Delete() override { return Dav(http::Method::kDelete, SavePath()).successful(); }

  bool SaveState(const StateRef& state, const std::string& bytes) override {
    return Dav(http::Method::kPut, StatePath(state), bytes).successful();
  }

  std::string LoadState(const StateRef& state) override {
    const http::Result served = Dav(http::Method::kGet, StatePath(state));
    return served.successful() ? served.response.body : std::string();
  }


 private:
  /// A WebDAV request as Cloud Sync makes it.
  http::Result Dav(http::Method method, const std::string& path,
                   const std::string& body = std::string()) const {
    http::Request request;
    request.method = method;
    request.url = rig_.base + "/api/sync/retroarch/" + path;
    request.headers.push_back(
        {"Authorization", "Basic " + rig::Base64(std::string(rig::kUser) + ":" + rig::kPassword)});
    if (method == http::Method::kPut) {
      request.headers.push_back({"Content-Type", "application/octet-stream"});
      request.body = body;
    }
    return rig_.client.Send(request);
  }

  std::string SavePath() const {
    return std::string("saves/") + game_.core_folder + "/" + game_.Stem() + ".srm";
  }
  static std::string StatePath(const StateRef& state) {
    return "states/" + harness::UrlEncode(state.core_folder) + "/" +
           harness::UrlEncode(state.file_name);
  }

  const Rig& rig_;
  const Game& game_;
};

/// A device that syncs through negotiate, as Grout and the web player do: one
/// save of one rom, held in its own storage, with the hash it last synced.
class NegotiatingDevice final : public OtherClient {
 public:
  enum class Style { kGrout, kWeb };

  NegotiatingDevice(rig::Checks& c, const Rig& rig, const Game& game, Style style,
                    const std::string& name)
      : rig_(rig),
        game_(game),
        style_(style),
        storage_(c, name),
        checks_(c),
        device_(rig, Register(c, rig, style, name)) {}

  NegotiatingDevice(const NegotiatingDevice&) = delete;
  NegotiatingDevice& operator=(const NegotiatingDevice&) = delete;

  bool Save(const std::string& bytes) override {
    bytes_ = bytes;
    written_ = std::chrono::system_clock::now();
    return Sync() && synced_ == crypto::Md5Hex(bytes);
  }

  std::string Load() override {
    Sync();
    return bytes_.value_or(std::string());
  }

  /// The user deletes it in RomM: every version in the slot, so the slot is
  /// empty -- what RetroArch's DELETE does, and what RomM's `delete` answers.
  bool Delete() override {
    bool deleted = true;
    for (const SlotRow& row : SlotRows(rig_, game_.rom_id)) {
      const http::Result gone =
          harness::PostJson(rig_.client, rig_.base + "/api/saves/delete", rig_.fixture,
                            "{\"saves\":[" + std::to_string(row.id) + "]}");
      deleted = deleted && gone.successful();
    }
    return deleted;
  }

  /// `POST /api/states`, the only way either of them stores one: no slot, no
  /// device, keyed by RomM on the name alone.
  bool SaveState(const StateRef& state, const std::string& bytes) override {
    const std::string staged = storage_.Host("/upload.state");
    rig::WriteFile(staged, bytes);
    http::Request request = harness::Authed(
        http::Method::kPost,
        rig_.base + "/api/states?rom_id=" + std::to_string(state.rom_id) +
            "&emulator=" + state.emulator,
        rig_.fixture);
    request.form.push_back(FilePart("stateFile", staged, state.file_name));
    return rig_.client.Send(request).successful();
  }

  /// The row under its name that its own core wrote: one another core wrote is
  /// not a state it can load.
  std::string LoadState(const StateRef& state) override {
    for (const sync::ServerState& row : StatesNamed(rig_, state.rom_id, state.file_name)) {
      if (row.emulator == state.emulator) {
        return StateBytes(rig_, row.id);
      }
    }
    return {};
  }

 private:
  /// `POST /api/devices` as the client registers itself (the web player's
  /// `registerDevice`), never `allow_existing`: a device of the cell's own.
  static std::string Register(rig::Checks& c, const Rig& rig, Style style,
                              const std::string& name) {
    const bool web = style == Style::kWeb;
    const http::Result created = harness::PostJson(
        rig.client, rig.base + "/api/devices", rig.fixture,
        "{\"name\":" + json::Quote(name + "-" + harness::UniqueSlot("device")) +
            ",\"platform\":" + json::Quote(web ? "Web" : "Linux") +
            ",\"client\":" + json::Quote(web ? "web" : "grout") +
            ",\"sync_mode\":\"api\",\"allow_existing\":false}");
    const std::string id = rig::JsonString(created.response.body, "device_id");
    c.Expect(created.successful() && !id.empty(),
             name + ": RomM registers the other device: HTTP " +
                 std::to_string(created.response.status) + " " + created.response.body);
    return id;
  }

  static http::FormPart FilePart(const char* field, const std::string& path,
                                 const std::string& file_name) {
    http::FormPart part;
    part.name = field;
    part.file_path = path;
    part.file_name = file_name;
    part.content_type = "application/octet-stream";
    return part;
  }

  /// One sync, start to `complete`: negotiate, then carry out what it says for
  /// this device's rom. Operations for any other rom are not this device's.
  bool Sync() {
    std::string saves = "[]";
    if (bytes_.has_value()) {
      saves = "[{\"rom_id\":" + std::to_string(game_.rom_id) +
              ",\"file_name\":" + json::Quote(game_.Stem() + ".srm") +
              ",\"slot\":\"autosave\",\"emulator\":" + json::Quote(game_.emulator) +
              ",\"content_hash\":" + json::Quote(crypto::Md5Hex(*bytes_)) +
              ",\"updated_at\":" + json::Quote(sync::FormatTimestamp(written_)) +
              ",\"file_size_bytes\":" + std::to_string(bytes_->size()) + "}]";
    }
    std::string body = "{\"device_id\":" + json::Quote(device_.id()) +
                       ",\"rom_ids\":[" + std::to_string(game_.rom_id) + "],\"saves\":" + saves;
    if (style_ == Style::kWeb) {
      body += ",\"restore_unlisted\":true,\"emulators\":[" + json::Quote(game_.emulator) + "]";
    }
    body += "}";
    const http::Result negotiated =
        harness::PostJson(rig_.client, rig_.base + "/api/sync/negotiate", rig_.fixture, body);
    const json::ParseResult plan = json::Parse(negotiated.response.body);
    if (!negotiated.successful() || !plan.ok()) {
      checks_.Expect(false, "the other device negotiates: HTTP " +
                                std::to_string(negotiated.response.status) + " " +
                                negotiated.response.body);
      return false;
    }
    const std::int64_t session = harness::Number(plan.value, "session_id");
    int completed = 0;
    int failed = 0;
    if (const json::Value* operations = plan.value.Find("operations"); operations != nullptr) {
      for (const json::Value& operation : operations->elements()) {
        if (harness::Number(operation, "rom_id") != game_.rom_id ||
            harness::Field(operation, "slot") != kSlot) {
          continue;
        }
        const bool done = Apply(harness::Field(operation, "action"), operation, session);
        completed += done ? 1 : 0;
        failed += done ? 0 : 1;
      }
    }
    harness::Complete(rig_.client, rig_.base, rig_.fixture, session, completed, failed);
    return failed == 0;
  }

  bool Apply(const std::string& action, const json::Value& operation, std::int64_t session) {
    if (action == "upload" && bytes_.has_value()) {
      return Upload(session);
    }
    if (action == "download" || action == "conflict") {
      // A conflict keeps the local copy aside (the web player archives it) and
      // takes the server's, as RomM's keep-both guidance has every client do.
      return Download(harness::Number(operation, "save_id"), session);
    }
    if (action == "delete") {
      bytes_.reset();
      synced_.clear();
    }
    return true;
  }

  bool Upload(std::int64_t session) {
    const std::string staged = storage_.Host("/upload.srm");
    rig::WriteFile(staged, *bytes_);
    std::string url = rig_.base + "/api/saves?rom_id=" + std::to_string(game_.rom_id) +
                      "&emulator=" + game_.emulator + "&slot=autosave&overwrite=true" +
                      "&device_id=" + device_.id() + "&session_id=" + std::to_string(session) +
                      "&content_hash=" + crypto::Md5Hex(*bytes_);
    if (style_ == Style::kWeb) {
      url += "&autocleanup=true";
    }
    http::Request request = harness::Authed(http::Method::kPost, url, rig_.fixture);
    request.form.push_back(FilePart("saveFile", staged, game_.Stem() + ".srm"));
    const http::Result posted = rig_.client.Send(request);
    checks_.Expect(posted.successful(), "the other device uploads: HTTP " +
                                            std::to_string(posted.response.status) + " " +
                                            posted.response.body);
    if (posted.successful()) {
      synced_ = crypto::Md5Hex(*bytes_);
    }
    return posted.successful();
  }

  bool Download(std::int64_t save_id, std::int64_t session) {
    const http::Result fetched = rig_.client.Send(harness::Authed(
        http::Method::kGet,
        rig_.base + "/api/saves/" + std::to_string(save_id) + "/content?device_id=" + device_.id() +
            "&session_id=" + std::to_string(session) + "&optimistic=true",
        rig_.fixture));
    if (!fetched.successful()) {
      checks_.Expect(false, "the other device downloads save " + std::to_string(save_id) +
                                ": HTTP " + std::to_string(fetched.response.status));
      return false;
    }
    bytes_ = fetched.response.body;
    synced_ = crypto::Md5Hex(*bytes_);
    written_ = std::chrono::system_clock::now();
    harness::PostJson(rig_.client, rig_.base + "/api/saves/" + std::to_string(save_id) +
                                       "/downloaded",
                      rig_.fixture,
                      "{\"device_id\":" + json::Quote(device_.id()) +
                          ",\"content_hash\":" + json::Quote(synced_) + "}");
    return true;
  }

  const Rig& rig_;
  const Game& game_;
  Style style_;
  harness::Sandbox storage_;
  rig::Checks& checks_;
  DeviceRow device_;
  std::optional<std::string> bytes_;
  std::string synced_;
  sync::Timestamp written_{};
};

// --- the cells ----------------------------------------------------------------

/// One run of a cell: the client, its game, and a stamp unique to the run.
struct Cell {
  rig::Checks& c;
  const Rig& rig;
  const Game& game;
  OtherClient& other;
  const PsxState& psx;  ///< the other client's PSX states
  std::string name;    ///< `interop.<client>.<cell>`, every message's prefix
  std::string stamp;   ///< unique bytes per run

  std::string Bytes(const std::string& what) const { return what + " " + stamp + "\n"; }
  std::string Say(const std::string& what) const { return name + ": " + what; }
};

/// The Switch's save is what the other client loads, and the next tick sends
/// nothing more.
void FromSwitch(const Cell& cell) {
  Switch console(cell.c, cell.rig, cell.name, /*states=*/false);
  console.HoldRom(cell.game);
  const std::string played = cell.Bytes("the Switch's progress");
  console.Played(cell.game.SwitchSave(), played);

  console.Tick(cell.Say("the Switch syncs"));
  const std::vector<SlotRow> rows = SlotRows(cell.rig, cell.game.rom_id);
  cell.c.Expect(rows.size() == 1 && rows[0].content_hash == crypto::Md5Hex(played) &&
                    rows[0].emulator == cell.game.emulator,
                cell.Say("the shared slot holds the Switch's save, once, under " +
                         std::string(cell.game.emulator)));
  cell.c.ExpectEq(cell.other.Load(), played, cell.Say("and it is what the other client loads"));

  const std::vector<std::string> synced = Versions(cell.rig, cell.game.rom_id);
  console.Tick(cell.Say("the next tick"));
  cell.c.ExpectEq(Listed(Versions(cell.rig, cell.game.rom_id)), Listed(synced),
                  cell.Say("which uploads nothing more"));
  cell.c.ExpectEq(console.sandbox.Read(cell.game.SwitchSave()), played,
                  cell.Say("and leaves the card as it was"));
}

/// The other client's save reaches the card: not while the card does not hold
/// the game (#241), then placed where Tico loads it under its own name (#237),
/// then updated over with the Switch's bytes backed up first.
void ToSwitch(const Cell& cell) {
  const std::string first = cell.Bytes("the other client's progress");
  cell.c.Expect(cell.other.Save(first), cell.Say("the other client syncs its save"));
  const std::vector<std::string> theirs = Versions(cell.rig, cell.game.rom_id);
  Switch console(cell.c, cell.rig, cell.name, /*states=*/false);
  const std::string save = cell.game.SwitchSave();

  console.Tick(cell.Say("a tick with the game not on the card"));
  cell.c.Expect(!console.sandbox.Exists(save),
                cell.Say("places nothing: negotiate names only the roms on the card"));

  console.HoldRom(cell.game);
  console.Tick(cell.Say("a tick with the game on the card"));
  cell.c.ExpectEq(console.sandbox.Read(save), first,
                  cell.Say("places the save in Tico's folder under the rom's name") +
                      console.Log());
  cell.c.ExpectEq(Listed(Versions(cell.rig, cell.game.rom_id)), Listed(theirs),
                  cell.Say("and uploads nothing back"));

  harness::PassASecond();
  const std::string second = cell.Bytes("the other client played on");
  cell.c.Expect(cell.other.Save(second), cell.Say("the other client syncs again"));
  const std::vector<std::string> versions = Versions(cell.rig, cell.game.rom_id);
  console.Tick(cell.Say("the next tick"));
  cell.c.ExpectEq(console.sandbox.Read(save), second, cell.Say("brings the newer save down"));
  cell.c.Expect(console.sandbox.HasBackupOf(first),
                cell.Say("with the save it replaced under .backup/ first"));
  cell.c.Expect(!console.Recorded(cell.game.rom_id, "conflict"),
                cell.Say("a download, not a conflict"));

  console.Tick(cell.Say("and the one after"));
  cell.c.ExpectEq(Listed(Versions(cell.rig, cell.game.rom_id)), Listed(versions),
                  cell.Say("which sends nothing: the card holds the server's head"));
  cell.c.ExpectEq(cell.other.Load(), second, cell.Say("and the other client keeps its own"));
}

/// Both moved past the version they share. The server is the source of truth,
/// so the other client's save is on the card; the Switch's is under `.backup/`
/// and on the server as an archival copy (#240), and the other client's own
/// progress is untouched.
void Conflict(const Cell& cell) {
  Switch console(cell.c, cell.rig, cell.name, /*states=*/false);
  console.HoldRom(cell.game);
  const std::string save = cell.game.SwitchSave();
  const std::string shared = cell.Bytes("the version both sides synced");
  console.Played(save, shared);
  console.Tick(cell.Say("the Switch syncs"));
  cell.c.ExpectEq(cell.other.Load(), shared, cell.Say("and the other client has the same"));

  harness::PassASecond();
  const std::string theirs = cell.Bytes("the other client played on");
  cell.c.Expect(cell.other.Save(theirs), cell.Say("the other client syncs its progress"));
  harness::PassASecond();
  const std::string ours = cell.Bytes("the Switch played on too, offline");
  console.Played(save, ours);
  console.Tick(cell.Say("the Switch syncs its own"));

  cell.c.ExpectEq(console.sandbox.Read(save), theirs,
                  cell.Say("the server's head is on the card") + console.Log());
  cell.c.Expect(console.sandbox.HasBackupOf(ours), cell.Say("the Switch's under .backup/"));
  cell.c.Expect(console.Recorded(cell.game.rom_id, "conflict"),
                cell.Say("on the conflicts screen"));
  const harness::ArchivalRow archived = harness::ArchivalWith(
      cell.rig.client, cell.rig.base, cell.rig.fixture, cell.game.rom_id, crypto::Md5Hex(ours));
  cell.c.Expect(archived.id != 0 && archived.emulator == cell.game.emulator,
                cell.Say("and on the server as an archival copy, under its core"));
  cell.c.ExpectEq(cell.other.Load(), theirs, cell.Say("the other client's progress is kept"));

  console.Tick(cell.Say("the next tick"));
  cell.c.ExpectEq(console.sandbox.Read(save), theirs, cell.Say("changes nothing on the card"));
  std::size_t copies = 0;
  for (const harness::ArchivalRow& row : harness::ArchivalRows(
           cell.rig.client, cell.rig.base, cell.rig.fixture, cell.game.rom_id)) {
    copies += row.content_hash == crypto::Md5Hex(ours) ? 1 : 0;
  }
  cell.c.ExpectEq(copies, std::size_t{1}, cell.Say("and archives once, not once a tick"));
}

/// A delete, both ways. A save gone from the card is no deletion signal: the
/// server and the other client keep it. A save the other client deletes is
/// removed from the card with its bytes under `.backup/` (#233), listed, and
/// never uploaded again.
void Delete(const Cell& cell) {
  Switch console(cell.c, cell.rig, cell.name, /*states=*/false);
  console.HoldRom(cell.game);
  const std::string save = cell.game.SwitchSave();
  const std::string shared = cell.Bytes("the version both sides synced");
  console.Played(save, shared);
  console.Tick(cell.Say("the Switch syncs"));
  cell.c.ExpectEq(cell.other.Load(), shared, cell.Say("and the other client has the same"));

  // The player deletes it from the card, by hand.
  std::error_code error;
  std::filesystem::remove(console.sandbox.Host(save), error);
  console.Tick(cell.Say("a tick without the save on the card"));
  cell.c.ExpectEq(SlotRows(cell.rig, cell.game.rom_id).size(), std::size_t{1},
                  cell.Say("deletes nothing on the server"));
  cell.c.ExpectEq(cell.other.Load(), shared, cell.Say("and the other client still loads it"));
  cell.c.Expect(!console.sandbox.Exists(save),
                cell.Say("nor puts it back: a synced save the card lost is restored only for a "
                         "game just downloaded (#241's restore_unlisted)"));
  console.Played(save, shared);  // the player puts it back
  console.Tick(cell.Say("a tick with it back"));

  cell.c.Expect(cell.other.Delete(), cell.Say("the other client deletes the save"));
  cell.c.ExpectEq(SlotRows(cell.rig, cell.game.rom_id).size(), std::size_t{0},
                  cell.Say("which empties the shared slot"));
  console.Tick(cell.Say("the next tick"));
  cell.c.Expect(!console.sandbox.Exists(save),
                cell.Say("removes the save from the card") + console.Log());
  cell.c.Expect(console.sandbox.HasBackupOf(shared), cell.Say("with its bytes under .backup/"));
  cell.c.Expect(console.Recorded(cell.game.rom_id, "deleted"),
                cell.Say("listed on the conflicts screen, restorable"));

  console.Tick(cell.Say("and the one after"));
  cell.c.ExpectEq(SlotRows(cell.rig, cell.game.rom_id).size(), std::size_t{0},
                  cell.Say("sends nothing back up"));
  cell.c.ExpectEq(cell.other.Load(), std::string(), cell.Say("and the other client has nothing"));
}

/// The web player's state name: the rom's name and the local time to the
/// millisecond (its bundle's `sessionStateName`).
std::string WebStateName(const std::string& game) {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const long long millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm local{};
  localtime_r(&seconds, &local);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &local);
  std::string ms = std::to_string(millis);
  ms.insert(0, 3 - ms.size(), '0');
  return game + " [" + stamp + "-" + ms + "].state";
}

/// Every state of `rom_id` named for `game`: before a cell, an earlier run's;
/// after it, its own.
void ClearStates(const Rig& rig, std::int64_t rom_id, const std::string& game) {
  const http::Result listed = rig.client.Send(harness::Authed(
      http::Method::kGet, rig.base + "/api/states?rom_id=" + std::to_string(rom_id), rig.fixture));
  const auto parsed = sync::ParseStateList(listed.response.body);
  if (!listed.successful() || !parsed.ok()) {
    return;
  }
  for (const sync::ServerState& state : parsed.value) {
    if (state.file_name.rfind(game, 0) == 0) {
      harness::DeleteState(rig.client, rig.base, rig.fixture, state.id);
    }
  }
}

/// The regular files under `sd_path`, recursively, as SD paths' leaves.
std::vector<std::string> FilesUnder(const harness::Sandbox& sandbox, const std::string& sd_path) {
  std::vector<std::string> found;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(sandbox.Host(sd_path), error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
    if (it->is_regular_file()) {
      found.push_back(it->path().filename().string());
    }
  }
  return found;
}

/// A PSX state of the other client's core and the Switch's DuckStation state
/// of the same game never meet (#238, #239): neither is placed for the other,
/// neither is written over the other -- on the card or on the server.
void States(const Cell& cell) {
  harness::Rom rom;
  const std::string game = "Synthetic Nested Game";
  cell.c.Expect(harness::FindRom(cell.rig.client, cell.rig.base, cell.rig.fixture, game, &rom),
                cell.Say("the fixture library has the PSX game"));
  if (rom.id == 0) {
    return;
  }
  // The game is on the card, so its own slot is negotiated too.
  ClearRom(cell.rig, rom.id);
  ClearStates(cell.rig, rom.id, game);
  const PsxState& plan = cell.psx;
  std::vector<std::pair<StateRef, std::string>> theirs;
  for (const std::string& name :
       plan.web_names ? std::vector<std::string>{WebStateName(game)}
                      : std::vector<std::string>{game + ".state1", game + ".state2"}) {
    theirs.push_back({StateRef{rom.id, plan.emulator, plan.core_folder, name},
                      cell.Bytes(std::string(plan.emulator) + "'s " + name)});
  }
  const auto store = [&cell, &theirs](const std::string& when) {
    for (const auto& [state, bytes] : theirs) {
      cell.c.Expect(cell.other.SaveState(state, bytes),
                    cell.Say("the other client stores " + state.file_name + " " + when));
    }
  };

  {
    Switch console(cell.c, cell.rig, cell.name, /*states=*/true);
    console.sandbox.MakeDirs("/tico/roms/psx/" + game);
    console.sandbox.Write("/tico/roms/psx/" + game + "/" + game + ".bin", "the disc\n");
    const std::string ours = "/tico/states/psx/" + game + ".state1";
    const std::string first = cell.Bytes("DuckStation's state");
    console.Played(ours, first);

    if (plan.before_switch) {
      store("first");
    }
    console.Tick(cell.Say("the Switch syncs its state"));
    // Which meeting this is turns on what that tick did: the Switch's state
    // goes up unless its name is already another core's row.
    const bool name_taken = plan.before_switch && !plan.web_names;
    bool first_up = false;
    for (const sync::ServerState& row : StatesNamed(cell.rig, rom.id, game + ".state1")) {
      first_up = first_up ||
                 (row.emulator == "tico-duckstation" && StateBytes(cell.rig, row.id) == first);
    }
    cell.c.ExpectEq(first_up, !name_taken,
                    cell.Say("the Switch's state is on the server, as DuckStation's, unless "
                             "its name was another core's first"));
    if (!plan.before_switch) {
      store("after");
    }
    console.Tick(cell.Say("a tick with both on the server"));
    harness::PassASecond();
    const std::string second = cell.Bytes("DuckStation's state, played on");
    console.Played(ours, second);
    console.Tick(cell.Say("a tick with the Switch's moved"));

    cell.c.ExpectEq(console.sandbox.Read(ours), second,
                    cell.Say("the Switch's state is its own, never replaced") + console.Log());
    const std::vector<std::string> placed = FilesUnder(console.sandbox, "/tico/states/psx");
    cell.c.Expect(placed.size() == 1 && placed[0] == game + ".state1",
                  cell.Say("nothing of the other core's is placed for DuckStation"));
    for (const char* folder : {"/retroarch/states", "/retroarch/cores"}) {
      cell.c.Expect(FilesUnder(console.sandbox, folder).empty(),
                    cell.Say(std::string("nor anywhere under ") + folder));
    }

    for (const auto& [state, bytes] : theirs) {
      cell.c.ExpectEq(cell.other.LoadState(state), bytes,
                      cell.Say(state.file_name + " is still its core's, as it stored it"));
    }
    bool ours_up = false;
    bool ours_elsewhere = false;
    for (const sync::ServerState& row : StatesNamed(cell.rig, rom.id, game + ".state1")) {
      const std::string bytes = StateBytes(cell.rig, row.id);
      if (bytes == first || bytes == second) {
        ours_elsewhere = ours_elsewhere || row.emulator != "tico-duckstation";
        ours_up = ours_up || (bytes == second && row.emulator == "tico-duckstation");
      }
    }
    cell.c.Expect(!ours_elsewhere,
                  cell.Say("DuckStation's bytes are never on the server as another core's"));
    if (plan.web_names) {
      cell.c.Expect(ours_up, cell.Say("the Switch's progress is on the server, as DuckStation's"));
    } else {
      // Its name is a row another core holds, and a POST would be written over
      // that row: kept both, every tick, until the names part.
      cell.c.Expect(!ours_up && StatesNamed(cell.rig, rom.id, game + ".state1").size() == 1,
                    cell.Say("the Switch's is kept on the card, never sent over theirs"));
    }
  }
  ClearStates(cell.rig, rom.id, game);
  ClearRom(cell.rig, rom.id);
}

// --- the matrix ---------------------------------------------------------------

using CellFn = void (*)(const Cell&);

struct CellEntry {
  const char* name;
  CellFn run;
};

constexpr CellEntry kCells[] = {
    {"from_switch", FromSwitch},
    {"to_switch", ToSwitch},
    {"conflict", Conflict},
    {"delete", Delete},
    {"states", States},
};

using MakeClient = std::unique_ptr<OtherClient> (*)(rig::Checks&, const Rig&, const Game&,
                                                    const std::string&);

/// One column of the matrix: the client, the game it plays -- three systems,
/// three cores -- and its PSX states, and when. RetroArch and Grout write two:
/// `<game>.state2`, a name Tico's folder takes and the Switch holds nothing
/// under, so only the core keeps it off the card -- and `<game>.state1`, the
/// Switch's own, met the two ways `POST /api/states` keying on the name alone
/// makes states meet:
///
///   retroarch -- first: the Switch's state meets a row another core wrote;
///   grout     -- after, by POST: RomM writes it over the Switch's row and moves
///                that row to Grout's core, which the Switch must not download;
///   web       -- one, under the web player's `<game> [<local time>].state`:
///                a name that collides with nothing, and Tico's folder refuses.
struct ClientSpec {
  const char* name;
  Game game;
  PsxState psx;
  MakeClient make;
};

const ClientSpec kClients[] = {
    {"retroarch",
     {"240pee_mb.gba", "gba", "mgba", "mGBA", 0},
     {"pcsx_rearmed", "PCSX-ReARMed", true, false},
     [](rig::Checks&, const Rig& rig, const Game& game,
        const std::string&) -> std::unique_ptr<OtherClient> {
       return std::make_unique<RetroArch>(rig, game);
     }},
    {"grout",
     {"gb240p.gb", "gb", "gambatte", "Gambatte", 0},
     {"swanstation", "SwanStation", false, false},
     [](rig::Checks& c, const Rig& rig, const Game& game,
        const std::string& name) -> std::unique_ptr<OtherClient> {
       return std::make_unique<NegotiatingDevice>(c, rig, game,
                                                  NegotiatingDevice::Style::kGrout, name);
     }},
    {"web",
     {"240pee.nes", "nes", "fceumm", "FCEUmm", 0},
     {"mednafen_psx_hw", "Beetle PSX HW", true, true},
     [](rig::Checks& c, const Rig& rig, const Game& game,
        const std::string& name) -> std::unique_ptr<OtherClient> {
       return std::make_unique<NegotiatingDevice>(c, rig, game, NegotiatingDevice::Style::kWeb,
                                                  name);
     }},
};

void Run(rig::Checks& c, const Rig& rig, const ClientSpec& spec, const CellEntry& entry) {
  Game game = spec.game;
  harness::Rom rom;
  c.Expect(harness::FindRom(rig.client, rig.base, rig.fixture, game.fs_name, &rom),
           std::string("the fixture library has ") + game.fs_name);
  if (rom.id == 0) {
    return;
  }
  game.rom_id = rom.id;
  const std::string name = std::string("interop.") + spec.name + "." + entry.name;
  ClearRom(rig, game.rom_id);
  rlog::Reset();
  {
    const std::unique_ptr<OtherClient> other = spec.make(c, rig, game, name);
    const Cell cell{c, rig, game, *other, spec.psx, name, harness::UniqueSlot(name)};
    entry.run(cell);
  }
  ClearRom(rig, game.rom_id);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  const std::size_t dot = scenario.find('.');
  const std::string client_name = scenario.substr(0, dot);
  const std::string cell_name = dot == std::string::npos ? "" : scenario.substr(dot + 1);
  const CellEntry* entry = nullptr;
  for (const CellEntry& one : kCells) {
    entry = cell_name == one.name ? &one : entry;
  }
  const ClientSpec* spec = nullptr;
  for (const ClientSpec& one : kClients) {
    spec = client_name == one.name ? &one : spec;
  }
  if (entry == nullptr || spec == nullptr) {
    std::cerr << "unknown cell: " << scenario << "\n";
    return 2;
  }

  const std::string base = rig::BaseUrl();
  std::error_code error;
  std::filesystem::create_directories(rig::ScratchDir(), error);
  const std::unique_ptr<http::HttpClient> client = rig::MakeClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    std::cerr << "no fixture token; run ./.venv/bin/python server/testing/provision.py\n";
    return rig::kSkip;
  }
  // Above every sandbox, so each one's teardown audit lands in a count that is
  // still read (harness::Sandbox).
  rig::Checks checks;
  const Rig rig{*client, base, fixture};
  Run(checks, rig, *spec, *entry);
  if (checks.failures() == 0) {
    std::cout << "interop." << scenario << " ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

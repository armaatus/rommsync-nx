// Where a save or a state the console has never held is written (#237).
//
// A first-time restore is the one write this client makes with no local file
// to go by: no folder the scan found it in, no name the emulator already used.
// Get it wrong and the save is either somewhere no emulator loads -- RetroArch's
// `/retroarch/saves` under RomM's datetime-tagged name, which is what this
// replaced -- or somewhere the next tick reads back as a *different* save and
// uploads a second copy of. So every placement these scenarios accept is also
// scanned back, and has to come back as the same rom in the same slot.
//
//   config  -- `[platform.<slug>] play_with`, and the default (#226 D5)
//   names   -- `place::Place`, table-driven: folder, core folder, name, extension
//   saves   -- `place::Placer::Save` against a real card: Tico's layouts,
//              RetroArch's, and every reason it places nothing
//   states  -- the same rule for a state, which keeps the name it pairs on
//   rom_folder -- #258: a rom folder, cut or not, placed in exactly when the
//              scan reads the file back
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rig.hpp"
#include "rommsync/config.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/placement.hpp"
#include "rommsync/retroarch_cfg.hpp"
#include "rommsync/rom_index.hpp"
#include "rommsync/save_scan.hpp"
#include "rommsync/state_db.hpp"
#include "rommsync/state_names.hpp"
#include "rommsync/sync.hpp"

namespace {

namespace config = rommsync::config;
namespace fs = rommsync::fs;
namespace place = rommsync::place;
namespace retroarch = rommsync::retroarch;
namespace roms = rommsync::roms;
namespace scan = rommsync::scan;
namespace state = rommsync::state;
namespace sync = rommsync::sync;

void Config(checks::Checks& c) {
  const config::Config defaults = config::Defaults();
  c.Expect(defaults.PlayWithFor("snes") == config::PlayWith::kTico,
           "snes is played in Tico unless config.ini says otherwise (D5)");
  c.Expect(defaults.PlayWithFor("dreamcast") == config::PlayWith::kTico,
           "and so is every system Tico runs, in RomM's spelling of it");
  c.Expect(defaults.PlayWithFor("ngc") == config::PlayWith::kTico,
           "the off-by-default ones included");
  c.Expect(!defaults.PlayWithFor("ps2").has_value(),
           "a system Tico does not run has no emulator, so nothing is placed for it");

  const config::LoadResult loaded = config::ParseConfig(
      "[platform.snes]\nplay_with = retroarch\n\n"
      "[platform.ps2]\nplay_with = RetroArch\n\n"
      "[platform.gba]\nplay_with = mednafen\n");
  c.Expect(loaded.value.PlayWithFor("snes") == config::PlayWith::kRetroArch,
           "play_with = retroarch moves a platform to RetroArch");
  c.Expect(loaded.value.PlayWithFor("ps2") == config::PlayWith::kRetroArch,
           "case is not the user's problem, and any platform may be given one");
  c.Expect(loaded.value.PlayWithFor("gba") == config::PlayWith::kTico,
           "a value that is neither keeps the default");
  bool warned = false;
  for (const config::Diagnostic& note : loaded.diagnostics) {
    warned = warned || (note.section == "platform.gba" && note.key == "play_with" &&
                        note.severity == config::Severity::kWarning);
  }
  c.Expect(warned, "...and says so on its line: " + loaded.DescribeDiagnostics());

  const config::PlatformFolders* snes = loaded.value.Platform("snes");
  const config::PlatformFolders* built_in = defaults.Platform("snes");
  c.Expect(snes != nullptr && built_in != nullptr && snes->saves == built_in->saves &&
               snes->roms == built_in->roms,
           "a section that sets only play_with keeps the built-in folders");
}

roms::Rom MakeRom(std::int64_t id, std::string name, std::string platform) {
  roms::Rom rom;
  rom.id = id;
  rom.fs_name_no_ext = name;
  rom.fs_name_no_tags = std::move(name);
  rom.platform_fs_slug = std::move(platform);
  return rom;
}

place::Layout Tico(std::string folder) {
  place::Layout layout;
  layout.play_with = config::PlayWith::kTico;
  layout.folder = std::move(folder);
  return layout;
}

place::Layout RetroArch(std::string folder, bool by_core) {
  place::Layout layout;
  layout.play_with = config::PlayWith::kRetroArch;
  layout.folder = std::move(folder);
  layout.by_core = by_core;
  return layout;
}

const std::string kAccount = "0123456789abcdef0123456789abcdef";

void Names(checks::Checks& c) {
  struct Row {
    const char* what;
    std::string rom_name;
    std::string platform;
    std::string emulator;
    std::string server_name;
    place::Layout layout;
    std::string expected;  ///< "" -- nothing is placed
  };
  const std::string tagged = " [2026-10-01_12-00-00]";
  const std::vector<Row> rows = {
      // #237's acceptance row, word for word.
      {"the issue's example: Tico 0.8, account X, Snes9x", "Super Mario World (USA)", "snes",
       "snes9x", "Super Mario World (USA)" + tagged + ".srm",
       Tico("/tico/saves/users/" + kAccount + "/snes"),
       "/tico/saves/users/" + kAccount + "/snes/Super Mario World (USA).srm"},
      {"the rom's name, not whatever the uploader called its save", "Game (USA)", "snes", "snes9x",
       "my save from the web player" + tagged + ".srm", Tico("/tico/saves/snes"),
       "/tico/saves/snes/Game (USA).srm"},
      {"DuckStation's memory card is .mcd", "Psx Game", "psx", "tico-duckstation",
       "Psx Game" + tagged + ".mcd", Tico("/tico/saves/psx"), "/tico/saves/psx/Psx Game.mcd"},
      {"RetroArch sorted by core: the core's folder", "Game", "snes", "snes9x",
       "Game" + tagged + ".srm", RetroArch("/retroarch/cores/savefiles", true),
       "/retroarch/cores/savefiles/Snes9x/Game.srm"},
      {"...in RomM's spelling of the folder", "Game", "psx", "mednafen_psx_hw",
       "Game" + tagged + ".srm", RetroArch("/retroarch/cores/savefiles", true),
       "/retroarch/cores/savefiles/Beetle PSX HW/Game.srm"},
      {"RetroArch unsorted: the folder itself", "Game", "gba", "mgba", "Game" + tagged + ".srm",
       RetroArch("/retroarch/saves", false), "/retroarch/saves/Game.srm"},
      {"a .sav is SRAM under another client's name, written as .srm", "Game", "gba", "mgba",
       "Game" + tagged + ".sav", Tico("/tico/saves/gba"), "/tico/saves/gba/Game.srm"},
      {"Snes9x's clock keeps its own extension", "Game", "snes", "snes9x",
       "Game" + tagged + ".rtc", Tico("/tico/saves/snes"), "/tico/saves/snes/Game.rtc"},
      {"Mupen64Plus writes four, and the server's says which", "N64 Game", "n64",
       "tico-mupen64plus", "N64 Game" + tagged + ".EEP", Tico("/tico/saves/n64"),
       "/tico/saves/n64/N64 Game.eep"},
      {"an isolated module whose names are unverified keeps the one it uploaded", "Dc Game", "dc",
       "tico-flycast", "Dc Game" + tagged + ".bin", Tico("/tico/saves/dc"),
       "/tico/saves/dc/Dc Game.bin"},

      {"a RetroArch .srm is not a DuckStation card, whatever it is renamed to", "Psx Game", "psx",
       "tico-duckstation", "Psx Game" + tagged + ".srm", Tico("/tico/saves/psx"), ""},
      {"an extension a libretro core does not write", "Game", "gba", "mgba",
       "Game" + tagged + ".state", Tico("/tico/saves/gba"), ""},
      {"one Mupen64Plus does not write either", "N64 Game", "n64", "tico-mupen64plus",
       "N64 Game" + tagged + ".srm", Tico("/tico/saves/n64"), ""},
      {"PPSSPP's saves are folders, which are not placed (#238)", "Psp Game", "psp",
       "tico-ppsspp", "Psp Game" + tagged + ".zip", Tico("/tico/saves/psp"), ""},
      {"no core folder for an id RomM's table does not know", "Dc Game", "dc", "tico-flycast",
       "Dc Game" + tagged + ".bin", RetroArch("/retroarch/cores/savefiles", true), ""},
      {"no emulator, no format", "Game", "snes", "", "Game" + tagged + ".srm",
       Tico("/tico/saves/snes"), ""},
      {"a server name with no extension gives an unknown format nothing", "Dc Game", "dc",
       "tico-flycast", "Dc Game", Tico("/tico/saves/dc"), ""},
      {"a rom name that climbs out of the folder", "..", "snes", "snes9x", "x.srm",
       Tico("/tico/saves/snes"), ""},
      {"...or holds a separator", "../../config/rommsync/token", "snes", "snes9x", "x.srm",
       Tico("/tico/saves/snes"), ""},
      {"...or a backslash", "a\\b", "snes", "snes9x", "x.srm", Tico("/tico/saves/snes"), ""},
      {"...or is empty", "", "snes", "snes9x", "x.srm", Tico("/tico/saves/snes"), ""},
      {"a path past kMaxPathLength is refused, not truncated", std::string(800, 'a'), "snes",
       "snes9x", "x.srm", Tico("/tico/saves/snes"), ""},
      {"no folder, no path", "Game", "snes", "snes9x", "Game.srm", Tico(""), ""},
  };
  for (const Row& row : rows) {
    const place::Placement placed =
        place::Place(MakeRom(1, row.rom_name, row.platform), row.emulator, row.server_name,
                     row.layout);
    if (row.expected.empty()) {
      c.Expect(!placed.ok() && !placed.reason.empty(),
               std::string(row.what) + ": nothing, and a reason -- got '" + placed.sd_path + "'");
    } else {
      c.ExpectEq(placed.sd_path, row.expected,
                 std::string(row.what) + (placed.reason.empty() ? "" : ": " + placed.reason));
      c.Expect(placed.reason.empty(), std::string(row.what) + ": no reason beside a path");
    }
  }

  c.Expect(place::Compatible("snes9x", "snes9x"), "the same core reads its own save");
  c.Expect(place::Compatible("beetle_psx_hw", "mednafen_psx_hw"),
           "two ids RomM files under one core folder are one core");
  c.Expect(place::Compatible("", "gambatte"), "a save with no emulator is libretro SRAM");
  c.Expect(!place::Compatible("", "tico-duckstation"),
           "...and never goes to an isolated format");
  c.Expect(!place::Compatible("mednafen_psx_hw", "tico-duckstation"),
           "a RetroArch psx save is not a DuckStation card (#226 D3)");
  c.Expect(!place::Compatible("tico-duckstation", "pcsx_rearmed"), "nor the other way round");
  c.Expect(!place::Compatible("mgba", "gambatte"), "another core's GB save is another format");
}

// --- against a card -----------------------------------------------------------

/// A scratch SD card: real directories, so `tico::Resolve` lists real folders
/// and the scan reads back real files.
class Card {
 public:
  explicit Card(const std::string& label)
      : root_(std::filesystem::path(rig::ScratchDir()) / "placement" / label) {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_, error);
    files_ = rommsync::host::MakeNativeFileSystem(root_.string());
  }
  ~Card() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  Card(const Card&) = delete;
  Card& operator=(const Card&) = delete;

  void MakeDirs(const std::string& sd_path) {
    std::error_code error;
    std::filesystem::create_directories(root_ / sd_path.substr(1), error);
  }

  /// Writes a file at `sd_path`, as the executor would after a placement.
  void Write(const std::string& sd_path, const std::string& bytes) {
    MakeDirs(sd_path.substr(0, sd_path.rfind('/')));
    rig::WriteFile((root_ / sd_path.substr(1)).string(), bytes);
  }

  void Remove(const std::string& sd_path) {
    std::error_code error;
    std::filesystem::remove(root_ / sd_path.substr(1), error);
  }

  fs::FileSystem& files() { return *files_; }

 private:
  std::filesystem::path root_;
  std::unique_ptr<fs::FileSystem> files_;
};

roms::RomIndex Library() {
  roms::RomIndex index;
  index.Add(MakeRom(1, "Super Mario World (USA)", "snes"));
  index.Add(MakeRom(2, "Gb Game", "gb"));
  index.Add(MakeRom(3, "Psx Game", "psx"));
  index.Add(MakeRom(4, "Shared", "dc"));
  index.Add(MakeRom(5, "Shared", "dreamcast"));
  index.Add(MakeRom(6, "Ps2 Game", "ps2"));
  index.Add(MakeRom(7, "Game (USA)", "snes"));
  index.Add(MakeRom(8, "Gba Game", "gba"));
  return index;
}

sync::SyncOperation Download(std::int64_t rom_id, std::optional<std::string> slot,
                             std::string emulator, std::string file_name) {
  sync::SyncOperation operation;
  operation.action = sync::Action::kDownload;
  operation.rom_id = rom_id;
  operation.save_id = 100 + rom_id;
  operation.slot = std::move(slot);
  if (!emulator.empty()) {
    operation.emulator = std::move(emulator);
  }
  operation.file_name = std::move(file_name);
  return operation;
}

/// The slot the scan gives a file called `name` under the default `[sync]
/// slot`. Asked of the scan rather than written out, so these scenarios follow
/// the slot rule wherever it goes (#236).
std::string ScanSlot(const std::string& name) {
  return scan::SlotFor(config::Defaults().sync.slot, name);
}

/// Writes the placed file, scans the card, and says whether the scan read it
/// back as `rom_id` in `slot` -- then removes it again.
bool ReadsBack(Card& card, const config::Config& config, const retroarch::Settings& settings,
               const roms::RomIndex& index, const std::string& sd_path, std::int64_t rom_id,
               const std::string& slot, std::string* said) {
  if (sd_path.empty() || sd_path.front() != '/') {
    *said = "nothing was placed";
    return false;
  }
  card.Write(sd_path, "placed bytes\n");
  const scan::ScanResult result =
      scan::ScanSaves(config, settings, index, card.files(), state::Baseline{});
  card.Remove(sd_path);
  for (const scan::SaveFile& save : result.saves) {
    if (save.sd_path == sd_path) {
      *said = "read back as rom " + std::to_string(save.rom_id) + " in slot " + save.slot;
      return save.rom_id == rom_id && save.slot == slot;
    }
  }
  *said = "not read back at all: " + result.DescribeSkipped();
  return false;
}

void Saves(checks::Checks& c) {
  const roms::RomIndex index = Library();
  const retroarch::Settings ra_defaults;
  // One slot for a `.srm` whichever emulator's folder it is in (#236), so the
  // two names below are the same slot and a save crosses between them.
  const std::string tico_srm = ScanSlot("x.srm");
  const std::string ra_srm = ScanSlot("x.srm");
  const std::string tagged = " [2026-10-01_12-00-00]";
  const sync::SyncOperation smw =
      Download(1, tico_srm, "snes9x", "Super Mario World (USA)" + tagged + ".srm");

  // #237's acceptance: Tico 0.8, one account, Snes9x.
  {
    Card card("tico-account");
    const std::string account = "/tico/saves/users/" + kAccount;
    card.MakeDirs(account);
    const config::Config config = config::Defaults();
    place::Placer placer(config, ra_defaults, index, card.files());
    const place::Placement placed = placer.Save(smw);
    c.ExpectEq(placed.sd_path, account + "/snes/Super Mario World (USA).srm",
               "Tico 0.8's account folder, the rom's name: " + placed.reason);
    std::string said;
    c.Expect(ReadsBack(card, config, ra_defaults, index, placed.sd_path, 1, tico_srm, &said),
             "and the next scan reads it as the same save: " + said);
  }

  // Tico before 0.8.0: no users/.
  {
    Card card("tico-legacy");
    card.MakeDirs("/tico/saves/snes");
    const config::Config config = config::Defaults();
    place::Placer placer(config, ra_defaults, index, card.files());
    const place::Placement placed = placer.Save(smw);
    c.ExpectEq(placed.sd_path, std::string("/tico/saves/snes/Super Mario World (USA).srm"),
               "Tico before 0.8.0: its folder for the system: " + placed.reason);
    std::string said;
    c.Expect(ReadsBack(card, config, ra_defaults, index, placed.sd_path, 1, tico_srm, &said),
             "read back: " + said);

    // Whatever the server's copy was, nothing for a Tico platform lands in a
    // RetroArch folder: it is placed in Tico's, or nowhere.
    const std::vector<sync::SyncOperation> any = {
        Download(1, tico_srm, "snes9x", "x.srm"),     Download(1, ra_srm, "snes9x", "x.srm"),
        Download(1, tico_srm, "", "x.srm"),           Download(1, "autosave", "snes9x", "x.srm"),
        Download(7, tico_srm, "bsnes", "x.srm"),      Download(7, tico_srm, "snes9x", "x.sav"),
        Download(1, std::nullopt, "snes9x", "x.srm"), Download(1, tico_srm, "snes9x", "x.rtc"),
    };
    for (const sync::SyncOperation& operation : any) {
      const place::Placement where = placer.Save(operation);
      c.Expect(where.sd_path.rfind("/tico/saves/snes/", 0) == 0 || !where.ok(),
               "a snes save goes to Tico or nowhere, never to RetroArch: " + where.sd_path);
      c.Expect(where.ok() || !where.reason.empty(), "and nowhere comes with a reason");
    }
  }

  // Another client's save in the shared slot reaches Tico (#236 + #237): what
  // RetroArch's Cloud Sync uploads (`saves/Snes9x/<rom>.srm` -> `autosave`,
  // `snes9x`), what the web player uploads (`autosave`, no emulator), and an
  // mGBA `.sav` some other client filed under `autosave`.
  {
    Card card("tico-from-others");
    const config::Config config = config::Defaults();
    c.ExpectEq(ScanSlot("x.srm"), std::string("autosave"),
               "a .srm is in RomM's shared autosave slot, whichever folder it is in");
    place::Placer placer(config, ra_defaults, index, card.files());
    struct Other {
      const char* who;
      sync::SyncOperation operation;
      std::string expected;
      std::int64_t rom_id;
    };
    const std::vector<Other> others = {
        {"RetroArch's Cloud Sync",
         Download(1, "autosave", "snes9x", "Super Mario World (USA)" + tagged + ".srm"),
         "/tico/saves/snes/Super Mario World (USA).srm", 1},
        {"the web player, which names no emulator",
         Download(7, "autosave", "", "Game (USA)" + tagged + ".srm"),
         "/tico/saves/snes/Game (USA).srm", 7},
        {"a .sav under autosave", Download(8, "autosave", "mgba", "Gba Game" + tagged + ".sav"),
         "/tico/saves/gba/Gba Game.srm", 8},
    };
    for (const Other& other : others) {
      const place::Placement placed = placer.Save(other.operation);
      c.ExpectEq(placed.sd_path, other.expected,
                 std::string(other.who) + "'s save goes to Tico's folder: " + placed.reason);
      std::string said;
      c.Expect(ReadsBack(card, config, ra_defaults, index, placed.sd_path, other.rom_id,
                         "autosave", &said),
               std::string(other.who) + ": read back in the slot it came from: " + said);
    }
  }

  // Several accounts and none named: whose is not guessed.
  {
    Card card("tico-two");
    const std::string other = "fedcba9876543210fedcba9876543210";
    card.MakeDirs("/tico/saves/users/" + kAccount);
    card.MakeDirs("/tico/saves/users/" + other);
    const config::Config undecided = config::Defaults();
    place::Placer placer(undecided, ra_defaults, index, card.files());
    const place::Placement placed = placer.Save(smw);
    c.Expect(!placed.ok() && placed.reason.find("account") != std::string::npos,
             "two accounts and none named place nothing, and say so: " + placed.reason);

    const config::LoadResult named =
        config::ParseConfig("[tico]\naccount = " + other + "\n");
    place::Placer chosen(named.value, ra_defaults, index, card.files());
    c.ExpectEq(chosen.Save(smw).sd_path,
               "/tico/saves/users/" + other + "/snes/Super Mario World (USA).srm",
               "the account [tico] names is the one written to");
  }

  // play_with = retroarch, RetroArch's own defaults: sorted by core, under
  // /retroarch/cores/savefiles -- not the map's /retroarch/saves.
  {
    Card card("retroarch-defaults");
    const config::LoadResult loaded = config::ParseConfig(
        "[platform.snes]\nplay_with = retroarch\n\n[platform.gb]\nplay_with = retroarch\n");
    place::Placer placer(loaded.value, ra_defaults, index, card.files());
    const place::Placement placed = placer.Save(Download(1, ra_srm, "snes9x", "x.srm"));
    c.ExpectEq(placed.sd_path,
               std::string("/retroarch/cores/savefiles/Snes9x/Super Mario World (USA).srm"),
               "RetroArch's default folder, in the core's: " + placed.reason);
    std::string said;
    c.Expect(ReadsBack(card, loaded.value, ra_defaults, index, placed.sd_path, 1, ra_srm, &said),
             "read back: " + said);
    c.ExpectEq(placer.Save(Download(7, ra_srm, "bsnes", "x.srm")).sd_path,
               std::string("/retroarch/cores/savefiles/bsnes/Game (USA).srm"),
               "a save another core wrote goes to that core's folder");
    c.ExpectEq(placer.Save(Download(2, ra_srm, "", "x.srm")).sd_path,
               std::string("/retroarch/cores/savefiles/Gambatte/Gb Game.srm"),
               "one with no emulator goes to the platform's core");
    c.Expect(!placer.Save(Download(1, ScanSlot("x.rtc"), "snes9x", "x.srm")).ok(),
             "a slot the RetroArch folder would not read back is not placed there");

    const config::LoadResult overridden = config::ParseConfig(
        "[platform.gb]\nplay_with = retroarch\nemulator = mgba\n");
    place::Placer mgba(overridden.value, ra_defaults, index, card.files());
    c.ExpectEq(mgba.Save(Download(2, ra_srm, "", "x.srm")).sd_path,
               std::string("/retroarch/cores/savefiles/mGBA/Gb Game.srm"),
               "an emulator override names the platform's core");
  }

  // RetroArch's other layouts.
  {
    Card card("retroarch-layouts");
    const config::LoadResult loaded = config::ParseConfig(
        "[platform.snes]\nplay_with = retroarch\n\n[sources]\nretroarch_content_dir = true\n");
    retroarch::Settings flat;
    flat.savefile_directory = "/retroarch/saves";
    flat.sort_savefiles = false;
    card.MakeDirs("/retroarch/saves");
    place::Placer unsorted(loaded.value, flat, index, card.files());
    const place::Placement placed = unsorted.Save(Download(1, ra_srm, "snes9x", "x.srm"));
    c.ExpectEq(placed.sd_path, std::string("/retroarch/saves/Super Mario World (USA).srm"),
               "unsorted: the folder itself: " + placed.reason);
    std::string said;
    c.Expect(ReadsBack(card, loaded.value, flat, index, placed.sd_path, 1, ra_srm, &said),
             "read back: " + said);

    retroarch::Settings missing;
    missing.savefile_directory = "/nowhere/saves";
    place::Placer fallback(loaded.value, missing, index, card.files());
    c.ExpectEq(fallback.Save(Download(1, ra_srm, "snes9x", "x.srm")).sd_path,
               std::string("/retroarch/cores/savefiles/Snes9x/Super Mario World (USA).srm"),
               "a configured folder that is not there is RetroArch's default, as RetroArch "
               "itself decides");

    retroarch::Settings beside;
    beside.savefiles_in_content_dir = true;
    place::Placer content(loaded.value, beside, index, card.files());
    const place::Placement by_rom = content.Save(Download(1, ra_srm, "snes9x", "x.srm"));
    c.ExpectEq(by_rom.sd_path,
               std::string("/tico/roms/snes/Snes9x/Super Mario World (USA).srm"),
               "beside the content: the rom's folder, then the core's: " + by_rom.reason);
    c.Expect(ReadsBack(card, loaded.value, beside, index, by_rom.sd_path, 1, ra_srm, &said),
             "read back: " + said);

    const config::LoadResult not_opted =
        config::ParseConfig("[platform.snes]\nplay_with = retroarch\n");
    place::Placer closed(not_opted.value, beside, index, card.files());
    const place::Placement refused = closed.Save(Download(1, ra_srm, "snes9x", "x.srm"));
    c.Expect(!refused.ok() && !refused.reason.empty(),
             "...and not without retroarch_content_dir, which the scan needs to read it: " +
                 refused.reason);

    retroarch::Settings cloud;
    cloud.cloud_sync = true;
    place::Placer syncs(not_opted.value, cloud, index, card.files());
    const place::Placement itself = syncs.Save(Download(1, ra_srm, "snes9x", "x.srm"));
    c.Expect(!itself.ok() && itself.reason.find("itself") != std::string::npos,
             "while RetroArch syncs itself, nothing goes into its folders: " + itself.reason);

    retroarch::Settings by_content;
    by_content.sort_savefiles_by_content = true;
    place::Placer per_content(not_opted.value, by_content, index, card.files());
    c.Expect(!per_content.Save(Download(1, ra_srm, "snes9x", "x.srm")).ok(),
             "a layout the scan does not read is not written either");
  }

  // Every other reason to place nothing.
  {
    Card card("refusals");
    const config::Config defaults = config::Defaults();
    place::Placer placer(defaults, ra_defaults, index, card.files());
    const auto refused = [&c](const place::Placement& placement, const std::string& what) {
      c.Expect(!placement.ok() && !placement.reason.empty(),
               what + ": nothing, with a reason -- got '" + placement.sd_path + "' / " +
                   placement.reason);
    };
    refused(placer.Save(Download(99, tico_srm, "snes9x", "x.srm")), "a rom not in the index");
    refused(placer.Save(Download(1, std::nullopt, "snes9x", "x.srm")),
            "an archival save, which pairs with nothing");
    refused(placer.Save(Download(1, ScanSlot("x.rtc"), "snes9x", "x.srm")),
            "a slot the scan would not give it back in");
    refused(placer.Save(Download(1, ScanSlot("x.sav"), "snes9x", "x.sav")),
            "a .sav in a slot of its own, which renaming to .srm would move out of it");
    refused(placer.Save(Download(3, ScanSlot("x.mcd"), "mednafen_psx_hw", "Psx Game.srm")),
            "a RetroArch psx save is not a DuckStation card (#226 D3)");
    refused(placer.Save(Download(6, tico_srm, "pcsx2", "x.ps2")), "a platform nothing maps");
    refused(placer.Save(Download(4, ScanSlot("x.bin"), "tico-flycast", "Shared.bin")),
            "a name dc and dreamcast share in Tico's one dc folder, which the scan would "
            "skip as ambiguous");

    const config::LoadResult mapped = config::ParseConfig(
        "[platform.ps2]\nroms = /roms/ps2\nsaves = /saves/ps2\n\n"
        "[platform.snes]\nroms = /tico/roms/snes\nsaves = /retroarch/saves\n");
    place::Placer custom(mapped.value, ra_defaults, index, card.files());
    refused(custom.Save(Download(6, "ps2", "pcsx2", "x.ps2")),
            "a platform with no emulator to play it in");
    refused(custom.Save(smw), "a Tico platform whose map lists no Tico folder");

    const config::LoadResult gb = config::ParseConfig("[platform.gb]\nemulator = mgba\n");
    place::Placer mgba(gb.value, ra_defaults, index, card.files());
    refused(mgba.Save(Download(2, tico_srm, "gambatte", "x.srm")),
            "Gambatte's save, for a Tico told it runs GB on mGBA");
    c.ExpectEq(mgba.Save(Download(2, tico_srm, "mgba", "x.srm")).sd_path,
               std::string("/tico/saves/gb/Gb Game.srm"), "...and mGBA's goes there");
  }
}

/// #258: the placer and the scan agree on a rom folder -- one RetroArch keeps
/// saves beside, or one the map also lists as a save folder. Each case is
/// asked of both: a placement is accepted exactly when the scan reads the file
/// back as the same save, and a refused one is a file the scan would not read.
void RomFolder(checks::Checks& c) {
  const roms::RomIndex index = Library();
  const std::string slot = ScanSlot("x.srm");
  const sync::SyncOperation smw = Download(1, slot, "snes9x", "x.srm");
  const config::LoadResult opted = config::ParseConfig(
      "[platform.snes]\nplay_with = retroarch\n\n[sources]\nretroarch_content_dir = true\n");
  retroarch::Settings sorted;
  sorted.savefiles_in_content_dir = true;
  retroarch::Settings flat = sorted;
  flat.sort_savefiles = false;

  // A rom folder past `fs::kMaxDirectoryEntries`, `prefix` deciding where its
  // roms sort against `Snes9x/` and against the save's own name.
  const auto fill = [](Card& card, const std::string& folder, const std::string& prefix) {
    card.MakeDirs(folder);
    for (std::size_t at = 0; at < fs::kMaxDirectoryEntries + 8; ++at) {
      card.Write(folder + "/" + prefix + std::to_string(1000 + at) + ".sfc", "rom\n");
    }
  };
  // `expected` is where the placement goes when the scan reads it back, and the
  // path it is refused at when the scan does not.
  const auto agrees = [&](Card& card, const config::Config& config,
                          const retroarch::Settings& settings, const sync::SyncOperation& operation,
                          const std::string& expected, bool read_back, const std::string& what) {
    place::Placer placer(config, settings, index, card.files());
    const place::Placement placed = placer.Save(operation);
    std::string said;
    const bool reads = ReadsBack(card, config, settings, index, expected, operation.rom_id,
                                 *operation.slot, &said);
    c.Expect(reads == read_back, what + ": the scan " + (read_back ? "reads" : "does not read") +
                                     " " + expected + " back -- " + said);
    if (read_back) {
      c.ExpectEq(placed.sd_path, expected, what + ": placed where it is read back: " +
                                               placed.reason);
    } else {
      c.Expect(!placed.ok() && !placed.reason.empty(),
               what + ": placed nowhere, with a reason -- got '" + placed.sd_path + "'");
    }
  };

  {
    // The roms sort before `Snes9x/` and before the save: the cut listing keeps
    // only them, so neither the core folder nor a loose save past it is read.
    Card card("rom-folder-cut");
    fill(card, "/tico/roms/snes", "A Rom ");
    agrees(card, opted.value, sorted, smw,
           "/tico/roms/snes/Snes9x/Super Mario World (USA).srm", false,
           "sorted, a core folder past a rom folder's cut");
    agrees(card, opted.value, flat, smw, "/tico/roms/snes/Super Mario World (USA).srm", false,
           "unsorted, a save past a rom folder's cut");
  }
  {
    // The roms sort after both: the cut keeps the core folder and the save.
    Card card("rom-folder-reached");
    fill(card, "/tico/roms/snes", "z rom ");
    agrees(card, opted.value, sorted, smw,
           "/tico/roms/snes/Snes9x/Super Mario World (USA).srm", true,
           "sorted, a core folder the cut listing reaches");
    agrees(card, opted.value, flat, smw, "/tico/roms/snes/Super Mario World (USA).srm", true,
           "unsorted, a save the cut listing reaches");
  }
  {
    // RetroArch's own folder, cut: every core folder RomM's table names is read
    // whether the listing reached it or not, so the placement stands.
    Card card("rom-folder-own-cut");
    fill(card, "/retroarch/cores/savefiles", "A stale ");
    const config::LoadResult retro = config::ParseConfig("[platform.snes]\nplay_with = retroarch\n");
    agrees(card, retro.value, retroarch::Settings{}, smw,
           "/retroarch/cores/savefiles/Snes9x/Super Mario World (USA).srm", true,
           "a cut RetroArch folder's core folder RomM's table names");
  }
  {
    // A Tico folder the map also lists as a rom folder is read as one: only
    // RetroArch's own names, so DuckStation's card is not read back there.
    Card card("rom-folder-tico");
    card.MakeDirs("/tico/saves/psx");
    const config::LoadResult shared =
        config::ParseConfig("[platform.psx]\nroms = /tico/saves/psx\nsaves = /tico/saves/psx\n");
    agrees(card, shared.value, retroarch::Settings{},
           Download(3, ScanSlot("x.mcd"), "tico-duckstation", "Psx Game.mcd"),
           "/tico/saves/psx/Psx Game.mcd", false, "a card in a Tico folder that is a rom folder");
  }
}

/// The state scan's answer for `sd_path`, written and removed again: read back
/// as `rom_id` under the same name.
/// `as` is the name the state must pair on once read back: its own file name
/// unless the scan maps it to RetroArch's (#239).
bool StateReadsBack(Card& card, const config::Config& config,
                    const retroarch::Settings& settings, const roms::RomIndex& index,
                    const std::string& sd_path, std::int64_t rom_id, std::string* said,
                    const std::string& as = "") {
  if (sd_path.empty() || sd_path.front() != '/') {
    *said = "nothing was placed";
    return false;
  }
  card.Write(sd_path, "placed state\n");
  const scan::ScanResult result =
      scan::ScanStates(config, settings, index, card.files(), state::Baseline{});
  card.Remove(sd_path);
  const std::string name = sd_path.substr(sd_path.rfind('/') + 1);
  for (const scan::StateFile& file : result.states) {
    if (file.sd_path == sd_path) {
      *said = "read back as rom " + std::to_string(file.rom_id) + " named " + file.file_name;
      return file.rom_id == rom_id && file.file_name == (as.empty() ? name : as);
    }
  }
  *said = "not read back at all: " + result.DescribeSkipped();
  return false;
}

void States(checks::Checks& c) {
  const roms::RomIndex index = Library();
  config::Config config = config::Defaults();
  config.sync.states = true;
  const retroarch::Settings ra_defaults;

  {
    Card card("states-tico");
    card.MakeDirs("/tico/states/users/" + kAccount);
    place::Placer placer(config, ra_defaults, index, card.files());
    const place::Placement placed = placer.State(1, "Super Mario World (USA).state0", "snes9x");
    c.ExpectEq(placed.sd_path,
               "/tico/states/users/" + kAccount + "/snes/Super Mario World (USA).state0",
               "Tico's state folder for the account, under the name it pairs on: " +
                   placed.reason);
    std::string said;
    c.Expect(StateReadsBack(card, config, ra_defaults, index, placed.sd_path, 1, &said),
             "read back as the same state: " + said);
    c.Expect(!placer.State(1, "Super Mario World (USA).state0", "bsnes").ok(),
             "another core's state is not one Snes9x loads");
    c.Expect(!placer.State(1, "Super Mario World (USA).state.auto", "snes9x").ok(),
             "a name Tico does not load a state from is not placed in its folder (#239)");
    c.Expect(!placer.State(1, "../Super Mario World (USA).state", "snes9x").ok(),
             "nor one that is a path");
    c.Expect(!placer.State(99, "Game.state", "snes9x").ok(), "nor one for a rom not in the index");
  }

  {
    Card card("states-retroarch");
    config::LoadResult loaded = config::ParseConfig("[platform.snes]\nplay_with = retroarch\n");
    loaded.value.sync.states = true;
    place::Placer placer(loaded.value, ra_defaults, index, card.files());
    const place::Placement placed = placer.State(1, "Super Mario World (USA).state1", "snes9x");
    c.ExpectEq(placed.sd_path,
               std::string("/retroarch/cores/savestates/Snes9x/Super Mario World (USA).state1"),
               "RetroArch's state folder, in the core's: " + placed.reason);
    std::string said;
    c.Expect(StateReadsBack(card, loaded.value, ra_defaults, index, placed.sd_path, 1, &said),
             "read back: " + said);

    retroarch::Settings cloud;
    cloud.cloud_sync = true;
    place::Placer syncs(loaded.value, cloud, index, card.files());
    c.Expect(!syncs.State(1, "Super Mario World (USA).state1", "snes9x").ok(),
             "and nothing while RetroArch syncs itself");
  }
}

}  // namespace

/// #238: DuckStation's cards, placed under the names the scan reads back -- one
/// card for every disc, named for the game, and card 2 as `_2` in its own slot
/// -- and a card kept under a disc-2 rom placed nowhere, since the game's name
/// is disc 1's card. Every placement is read back by the scan.
void Cards(checks::Checks& c) {
  roms::RomIndex index;
  index.Add(MakeRom(1, "Psx Game", "psx"));
  index.Add(MakeRom(2, "Two Disc (USA) (Disc 1)", "psx"));
  index.Add(MakeRom(3, "Two Disc (USA) (Disc 2)", "psx"));
  const retroarch::Settings ra_defaults;
  const config::Config config = config::Defaults();
  const std::string card1 = ScanSlot("x.mcd");
  const std::string card2 = card1 + "-card2";
  const std::string tagged = " [2026-10-01_12-00-00].mcd";

  Card card("cards");
  card.MakeDirs("/tico/saves/psx");
  place::Placer placer(config, ra_defaults, index, card.files());

  struct Row {
    std::int64_t rom_id;
    const std::string& slot;
    const char* path;  ///< "" for placed nowhere
    int read_back_as;  ///< the rom the scan must read it back as
  };
  const Row kRows[] = {
      {1, card1, "/tico/saves/psx/Psx Game.mcd", 1},
      {1, card2, "/tico/saves/psx/Psx Game_2.mcd", 1},
      {2, card1, "/tico/saves/psx/Two Disc (USA).mcd", 2},
      {2, card2, "/tico/saves/psx/Two Disc (USA)_2.mcd", 2},
      {3, card1, "", 0},
      {3, card2, "", 0},
  };
  for (const Row& row : kRows) {
    const place::Placement placed =
        placer.Save(Download(row.rom_id, row.slot, "tico-duckstation", "Card" + tagged));
    const std::string what = "rom " + std::to_string(row.rom_id) + " in " + row.slot;
    if (*row.path == '\0') {
      c.Expect(!placed.ok() && !placed.reason.empty(),
               what + ": a disc-2 card is not placed over disc 1's: " + placed.sd_path);
      continue;
    }
    c.ExpectEq(placed.sd_path, std::string(row.path), what + ": " + placed.reason);
    c.ExpectEq(placed.emulator, std::string("tico-duckstation"),
               what + ": the placement names who reads it");
    std::string said;
    c.Expect(ReadsBack(card, config, ra_defaults, index, placed.sd_path, row.read_back_as,
                       row.slot, &said),
             what + ": read back as the same card: " + said);
  }

  // A RetroArch core's save is not a DuckStation card, whatever slot it is in.
  c.Expect(!placer.Save(Download(1, card1, "mednafen_psx_hw", "Card.srm")).ok(),
           "a Beetle save is not placed in DuckStation's folder");
}

/// #239: a state is placed only where its emulator loads it, and under the name
/// that frontend gives the slot -- RetroArch's mapped onto Tico's when asked.
/// Every refusal for "not this console's to load" is marked `foreign`, which
/// `sync::SyncStates` passes over silently rather than failing every tick.
void StateSlots(checks::Checks& c) {
  const roms::RomIndex index = Library();
  const retroarch::Settings ra_defaults;
  config::Config mapped = config::Defaults();
  mapped.sync.states = true;
  mapped.sync.map_state_slots = true;
  config::Config unmapped = mapped;
  unmapped.sync.map_state_slots = false;
  const std::string snes = "/tico/states/users/" + kAccount + "/snes/";
  const std::string smw = "Super Mario World (USA)";

  {
    Card card("state-slots-tico");
    card.MakeDirs("/tico/states/users/" + kAccount);
    place::Placer placer(mapped, ra_defaults, index, card.files());
    struct Row {
      const char* server;
      const char* tico;
    };
    for (const Row row : {Row{".state.auto", ".state5"}, Row{".state1", ".state0"},
                          Row{".state5", ".state4"}}) {
      const place::Placement placed = placer.State(1, smw + row.server, "snes9x");
      c.ExpectEq(placed.sd_path, snes + smw + row.tico,
                 std::string("RetroArch's ") + row.server + " lands as Tico's " + row.tico + ": " +
                     placed.reason);
      std::string said;
      c.Expect(StateReadsBack(card, mapped, ra_defaults, index, placed.sd_path, 1, &said,
                              smw + row.server),
               std::string("and is read back under RetroArch's name again: ") + said);
    }
    for (const char* none : {".state", ".state6", ".state0"}) {
      const place::Placement placed = placer.State(1, smw + none, "snes9x");
      c.Expect(!placed.ok() && placed.foreign,
               std::string(none) + " is a slot Tico has none of, passed over: " + placed.reason);
    }
    const place::Placement other = placer.State(1, smw + ".state.auto", "bsnes");
    c.Expect(!other.ok() && other.foreign, "another core's state is not this console's to load");
    const place::Placement unnamed = placer.State(1, smw + ".state.auto", "");
    c.Expect(!unnamed.ok() && unnamed.foreign,
             "nor is one that does not say which core wrote it: a state is a core's memory");
  }

  {
    // Off, each keeps its own name, and Tico takes only its own.
    Card card("state-slots-unmapped");
    card.MakeDirs("/tico/states/users/" + kAccount);
    place::Placer placer(unmapped, ra_defaults, index, card.files());
    const place::Placement own = placer.State(1, smw + ".state5", "snes9x");
    c.ExpectEq(own.sd_path, snes + smw + ".state5", "Tico's own Auto, as it is: " + own.reason);
    std::string said;
    c.Expect(StateReadsBack(card, unmapped, ra_defaults, index, own.sd_path, 1, &said),
             "read back under the same name: " + said);
    const place::Placement auto_state = placer.State(1, smw + ".state.auto", "snes9x");
    c.Expect(!auto_state.ok() && auto_state.foreign,
             "RetroArch's Auto is not one Tico loads without the mapping");
  }

  {
    // The acceptance: DuckStation's and a RetroArch PSX core's never meet.
    Card card("state-slots-psx");
    card.MakeDirs("/tico/states/users/" + kAccount);
    place::Placer tico(mapped, ra_defaults, index, card.files());
    const place::Placement own = tico.State(3, "Psx Game.state5", "tico-duckstation");
    c.ExpectEq(own.sd_path, "/tico/states/users/" + kAccount + "/psx/Psx Game.state5",
               "DuckStation's state, never mapped, in DuckStation's folder: " + own.reason);
    std::string said;
    c.Expect(StateReadsBack(card, mapped, ra_defaults, index, own.sd_path, 3, &said),
             "read back under its own name: " + said);
    for (const char* core : {"pcsx_rearmed", "swanstation", "mednafen_psx_hw"}) {
      const place::Placement placed = tico.State(3, "Psx Game.state5", core);
      c.Expect(!placed.ok() && placed.foreign,
               std::string(core) + "'s state is never placed for DuckStation");
    }

    config::LoadResult retro = config::ParseConfig("[platform.psx]\nplay_with = retroarch\n");
    retro.value.sync.states = true;
    retro.value.sync.map_state_slots = true;
    place::Placer retroarch_psx(retro.value, ra_defaults, index, card.files());
    const place::Placement isolated =
        retroarch_psx.State(3, "Psx Game.state5", "tico-duckstation");
    c.Expect(!isolated.ok() && isolated.foreign,
             "and DuckStation's is never placed for a RetroArch PSX core");
    const place::Placement core = retroarch_psx.State(3, "Psx Game.state.auto", "pcsx_rearmed");
    c.ExpectEq(core.sd_path,
               std::string("/retroarch/cores/savestates/PCSX-ReARMed/Psx Game.state.auto"),
               "a RetroArch core's Auto goes to that core's folder: " + core.reason);
    c.Expect(StateReadsBack(card, retro.value, ra_defaults, index, core.sd_path, 3, &said),
             "and RetroArch's Auto is read back as its rom's: " + said);
    const place::Placement zero = retroarch_psx.State(3, "Psx Game.state0", "pcsx_rearmed");
    c.Expect(!zero.ok() && zero.foreign, "RetroArch loads no .state0");
  }
}

/// #239: the two names one state goes by, Tico's and RetroArch's -- the table
/// from the issue, row by row, both ways, and the names neither side has.
void StateTable(checks::Checks& c) {
  struct Row {
    const char* slot;
    const char* tico;       ///< "" -- Tico has no such slot
    const char* retroarch;  ///< "" -- RetroArch has no such slot
  };
  const Row kTable[] = {
      {"menu slot 1", "Game (USA).state0", "Game (USA).state1"},
      {"menu slot 2", "Game (USA).state1", "Game (USA).state2"},
      {"menu slot 3", "Game (USA).state2", "Game (USA).state3"},
      {"menu slot 4", "Game (USA).state3", "Game (USA).state4"},
      {"menu slot 5", "Game (USA).state4", "Game (USA).state5"},
      {"Auto", "Game (USA).state5", "Game (USA).state.auto"},
      {"RetroArch slot 0", "", "Game (USA).state"},
      {"RetroArch slot 6", "", "Game (USA).state6"},
  };
  for (const Row& row : kTable) {
    const std::string slot = row.slot;
    if (*row.tico != '\0') {
      c.ExpectEq(scan::RetroArchStateName(row.tico).value_or("(none)"),
                 std::string(row.retroarch), slot + ": Tico's file under RetroArch's name");
      c.Expect(scan::IsTicoStateName(row.tico), slot + ": a name Tico loads");
    }
    const std::optional<std::string> tico = scan::TicoStateName(row.retroarch);
    c.ExpectEq(tico.value_or(""), std::string(row.tico),
               slot + ": RetroArch's file under Tico's name, or none");
    c.Expect(scan::IsRetroArchStateName(row.retroarch), slot + ": a name RetroArch loads");
  }

  // The names that are neither side's slot map to nothing, rather than to a
  // guess: RetroArch never writes `.state0`, Tico never `.state6` or `.state`.
  for (const char* name : {"Game.state0", "Game.srm", "Game", ".state", "Game.state01",
                           "Game.state-1", "Game.auto"}) {
    c.Expect(!scan::TicoStateName(name).has_value(),
             std::string(name) + " is not a RetroArch slot Tico has");
  }
  for (const char* name : {"Game.state", "Game.state6", "Game.state.auto", "Game.srm", ".state0",
                           "Game.state00"}) {
    c.Expect(!scan::RetroArchStateName(name).has_value(),
             std::string(name) + " is not a Tico slot RetroArch has");
  }
  c.Expect(!scan::IsRetroArchStateName("Game.state0"), "RetroArch's slot 0 is .state, not .state0");
  c.Expect(!scan::IsTicoStateName("Game.state.auto") && !scan::IsTicoStateName("Game.state"),
           "Tico writes only .state0 to .state5");

  // FAT32 does not keep case apart, and neither does the table; the stem is
  // kept as it was.
  c.ExpectEq(scan::RetroArchStateName("Game.STATE5").value_or(""), std::string("Game.state.auto"),
             "an upper-case extension maps like a lower-case one");
  c.ExpectEq(scan::TicoStateName("Game.State.Auto").value_or(""), std::string("Game.state5"),
             "both ways");

  // What a state's name is matched to a rom by: `.state.auto` is one extension.
  c.ExpectEq(scan::StateMatchName("Game (USA).state.auto"), std::string("Game (USA).state"),
             "RetroArch's Auto state is matched by its rom's name");
  c.ExpectEq(scan::StateMatchName("Game (USA).state3"), std::string("Game (USA).state3"),
             "and every other name as it is");

  // Mapped only when asked, and only for a libretro core.
  config::Config on = config::Defaults();
  on.sync.map_state_slots = true;
  c.Expect(!config::Defaults().sync.map_state_slots, "the mapping is off by default (#226 D4)");
  c.Expect(scan::MapsStateSlots(on, "snes9x"), "on, a libretro module's slots are mapped");
  c.Expect(!scan::MapsStateSlots(on, "tico-duckstation"),
           "an isolated module's are not: no RetroArch core loads them");
  c.Expect(!scan::MapsStateSlots(on, ""), "nor an unnamed emulator's");
  c.Expect(!scan::MapsStateSlots(config::Defaults(), "snes9x"), "and off, nobody's");
  const config::LoadResult parsed = config::ParseConfig("[sync]\nmap_state_slots = true\n");
  bool said_of_it = false;
  for (const config::Diagnostic& note : parsed.diagnostics) {
    said_of_it = said_of_it || note.key == "map_state_slots";
  }
  c.Expect(parsed.value.sync.map_state_slots && !said_of_it,
           "[sync] map_state_slots = true turns it on: " + parsed.DescribeDiagnostics());
}

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "config";
  checks::Checks c;
  if (scenario == "config") {
    Config(c);
  } else if (scenario == "names") {
    Names(c);
  } else if (scenario == "saves") {
    Saves(c);
  } else if (scenario == "states") {
    States(c);
  } else if (scenario == "cards") {
    Cards(c);
  } else if (scenario == "state_table") {
    StateTable(c);
  } else if (scenario == "state_slots") {
    StateSlots(c);
  } else if (scenario == "rom_folder") {
    RomFolder(c);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }
  if (c.failures() == 0) {
    std::cout << "place." << scenario << " ok\n";
  }
  return c.failures() == 0 ? 0 : 1;
}

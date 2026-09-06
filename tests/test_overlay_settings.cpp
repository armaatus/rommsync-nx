// The overlay's settings screen: the effective configuration, every complaint
// the parser had about it, and the one action the screen owns.
//
// M4-4 (#26). The screen itself cannot be checked before the M8-1 gate --
// nothing in this repo draws a frame -- so everything about it that is a
// *decision* lives in `rommsync::overlay::SettingsView`, and this is what holds
// it: which sentence a console with no `server.url` gets, what `interval_min =
// 0` reads as, which `roms` folder is marked as the one that gets written to,
// and what a folder map too large to send says instead of nothing.
//
// The configurations here come from `config::ParseConfig` and go through
// `ipc::EncodeConfigView`/`DecodeConfigView` rather than being hand-filled
// structs, for the reason `overlay.library` round-trips its pages: the screen
// renders what the *wire* carried, and a suite that skipped the codec would
// agree with itself while a real console drew empty rows.
//
// The one criterion that is about the code rather than about its behaviour is
// the leak: a `Diagnostic` about `server.url` never quotes the URL
// (`config.hpp`), and this screen has the URL in hand. It is asserted rather
// than reviewed, because it is the only leak this screen can cause.
//
// No server, no console, no emulator -- pure decisions over pure data, so this
// never skips.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "rommsync/overlay_sync_actions.hpp"

namespace {

namespace config = rommsync::config;
namespace ipc = rommsync::ipc;
namespace overlay = rommsync::overlay;

using checks::Checks;

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// A `ConfigView` for `text`, as the overlay would receive it.
///
/// Through the real codec, and through `ipc::TrimDiagnostics` -- which is what
/// `GetConfig` applies -- so a case written here is a case a console can be in.
/// A payload this build cannot decode is `Link::kUnreadable` and is not this
/// function's to produce, so a failed decode is a failure of the suite.
ipc::ConfigView ViewFor(Checks& checks, const std::string& text) {
  const config::LoadResult parsed = config::ParseConfig(text);
  ipc::ConfigView sent;
  sent.config = parsed.value;
  sent.diagnostics = ipc::TrimDiagnostics(parsed.diagnostics);

  const ipc::Decoded<ipc::ConfigView> decoded = ipc::DecodeConfigView(ipc::EncodeConfigView(sent));
  checks.Expect(decoded.ok(), "the config view round-trips through the wire");
  return decoded.ok() ? decoded.value : sent;
}

/// The section titled `title`, or nullptr.
const overlay::SettingsSection* SectionNamed(const overlay::SettingsView& view,
                                             const std::string& title) {
  for (const overlay::SettingsSection& section : view.sections) {
    if (section.title == title) {
      return &section;
    }
  }
  return nullptr;
}

/// The row labelled `label` in the section titled `title`, or nullptr.
const overlay::SettingsRow* RowNamed(const overlay::SettingsView& view, const std::string& title,
                                     const std::string& label) {
  const overlay::SettingsSection* section = SectionNamed(view, title);
  if (section == nullptr) {
    return nullptr;
  }
  for (const overlay::SettingsRow& row : section->rows) {
    if (row.label == label) {
      return &row;
    }
  }
  return nullptr;
}

/// A `config.ini` with a usable server and nothing else wrong with it.
const char* kMinimal = "[server]\nurl = https://romm.example.com\n";

// --- the file, as the screen reads it ----------------------------------------

/// A console with no `config.ini` at all: defaults, one notice, and no server.
///
/// The first-boot screen, and the one that has to be right -- a user who has
/// just installed this sees it before they see anything else.
void CheckNoConfigFileAtAll(Checks& checks) {
  const std::filesystem::path missing =
      std::filesystem::temp_directory_path() / "rommsync-settings-no-such-dir" / "config.ini";
  std::error_code ignored;
  std::filesystem::remove_all(missing.parent_path(), ignored);

  const config::LoadResult loaded = config::LoadConfig(missing.string());
  ipc::ConfigView sent;
  sent.config = loaded.value;
  sent.diagnostics = ipc::TrimDiagnostics(loaded.diagnostics);

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  checks.Expect(view.link == overlay::Link::kOk, "a missing file is a configuration, not a link");
  checks.ExpectEq(view.headline, std::string("No server set"),
                  "a console with no config.ini leads with the server");
  checks.Expect(view.tone == overlay::Tone::kBad, "no server reads as bad");
  checks.Expect(!view.hint.empty(), "the no-server headline names the line to add");
  checks.Expect(!view.complaints.empty(), "the missing file is said out loud");

  // The defaults are the *effective* configuration and are drawn as such: the
  // screen shows what is in force, not what the file says (#26).
  const overlay::SettingsRow* interval = RowNamed(view, "[sync]", "interval_min");
  checks.Expect(interval != nullptr, "the default sync section is drawn");
  if (interval != nullptr) {
    checks.ExpectEq(interval->value, std::string("30 minutes"), "the default interval is drawn");
  }
  const overlay::SettingsSection* snes = SectionNamed(view, "[platform.snes]");
  checks.Expect(snes != nullptr, "the built-in folder map is drawn, not just the file's");

  const overlay::SettingsRow* url = RowNamed(view, "[server]", "url");
  checks.Expect(url != nullptr && url->value == "(blank)",
                "the url row says the field is blank rather than claiming there is no line");
}

/// A file whose `url` the parser refused: a warning for the line and an error
/// for the file.
void CheckErrorConfig(Checks& checks) {
  const ipc::ConfigView sent =
      ViewFor(checks, "[server]\nurl = ftp://romm.example.com\n[sync]\nenabled = true\n");
  const overlay::SettingsView view = overlay::RenderSettings(sent);

  checks.ExpectEq(view.headline, std::string("No server set"),
                  "a rejected url leaves the console with no server, which is the headline");
  bool has_error = false;
  for (const overlay::Line& line : view.complaints) {
    has_error = has_error || line.label == std::string("error");
  }
  checks.Expect(has_error, "the error is drawn");
  checks.Expect(view.complaints.front().label == std::string("error"),
                "kError sorts first");
}

/// An error over a console that still has a server.
///
/// `ParseConfig` cannot produce one today -- every `kError` it raises is about
/// the file or about the `url`, and both leave `configured()` false -- but the
/// *wire* can: `GetConfig` adds one for a URL too long to send, and a sysmodule
/// from a later release may add others. The headline has an arm for it, so the
/// arm is held rather than left to a reader.
void CheckErrorOverAConfiguredConsole(Checks& checks) {
  ipc::ConfigView sent;
  sent.config = config::Defaults();
  sent.config.server.url = "https://romm.example.com";
  sent.diagnostics.push_back(config::Diagnostic{config::Severity::kError, 4, "downloads",
                                                "enabled", "something a later build knows about"});

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  checks.ExpectEq(view.headline, std::string("config.ini has errors"),
                  "an error over a configured console is counted in the headline");
  checks.Expect(view.tone == overlay::Tone::kBad, "an error reads as bad");
  checks.Expect(!view.hint.empty(), "the error headline says what the rows below are");
}

/// Warnings and notices, and the headline each one gets.
void CheckWarningsAndNotices(Checks& checks) {
  const ipc::ConfigView warned =
      ViewFor(checks, "[server]\nurl = https://romm.example.com\n[sync]\nstates = maybe\n");
  const overlay::SettingsView warning_view = overlay::RenderSettings(warned);
  checks.ExpectEq(warning_view.headline, std::string("config.ini has warnings"),
                  "a dropped line is counted in the headline");
  checks.Expect(warning_view.tone == overlay::Tone::kWarn, "a warning reads as a warning");
  // The value in force is the default, not the text that was refused.
  const overlay::SettingsRow* states = RowNamed(warning_view, "[sync]", "states");
  checks.Expect(states != nullptr && states->value == "Off",
                "the row shows the value in force, not the line that was dropped");

  // A slug this build ships no default for is a notice and nothing more.
  const ipc::ConfigView noticed = ViewFor(
      checks, "[server]\nurl = https://romm.example.com\n[platform.vectrex]\nroms = /roms/vec\n");
  const overlay::SettingsView notice_view = overlay::RenderSettings(noticed);
  checks.ExpectEq(notice_view.headline, std::string("Settings in force"),
                  "a notice is not a complaint about the settings being wrong");
  checks.Expect(notice_view.tone == overlay::Tone::kGood, "a file with no warnings reads as good");
  checks.Expect(!notice_view.complaints.empty(), "the notice is still drawn");
  checks.Expect(SectionNamed(notice_view, "[platform.vectrex]") != nullptr,
                "a platform this build has no default for is still mapped and still drawn");
}

/// An override that empties a platform: the documented way to switch one off.
///
/// **`ParseConfig` drops the platform rather than keeping an empty mapping for
/// it**, and says so in a `kNotice` -- which is not what #26 assumed when it
/// asked for "an empty section marked *skipped*". The screen keeps the promise
/// through the complaint instead, and that is the render this holds: the
/// platform is gone from the effective map, and the sentence saying so is on
/// screen. #26 is edited to say it.
void CheckEmptiedPlatform(Checks& checks) {
  const ipc::ConfigView sent = ViewFor(
      checks, "[server]\nurl = https://romm.example.com\n[platform.psp]\nroms =\nsaves =\nstates =\n");
  const overlay::SettingsView view = overlay::RenderSettings(sent);

  checks.Expect(SectionNamed(view, "[platform.psp]") == nullptr,
                "a platform mapped nowhere is not in the effective configuration");
  bool said_so = false;
  for (const overlay::Line& complaint : view.complaints) {
    said_so = said_so || (Contains(complaint.value, "psp") && Contains(complaint.value, "skipped"));
  }
  checks.Expect(said_so,
                "and the screen says it is skipped rather than leaving the user to notice");

  // ...and the platforms it did not mention are untouched, which is what "a
  // section replaces that platform's defaults" means.
  const overlay::SettingsSection* snes = SectionNamed(view, "[platform.snes]");
  checks.Expect(snes != nullptr && !snes->rows.empty(),
                "emptying one platform leaves the rest of the built-in map alone");
}

/// A mapping that *is* in the map and maps nothing.
///
/// `ParseConfig` cannot produce one (see above) and the wire carries one
/// perfectly well, so the arm that draws it is reachable from a sysmodule whose
/// `Config` did not come from the parser -- and it is one line of code away from
/// being drawn as a platform with three blank rows.
void CheckPlatformMappedNowhere(Checks& checks) {
  ipc::ConfigView built;
  built.config = config::Defaults();
  built.config.server.url = "https://romm.example.com";
  built.config.platforms["psp"] = config::PlatformFolders{};
  const ipc::Decoded<ipc::ConfigView> decoded =
      ipc::DecodeConfigView(ipc::EncodeConfigView(built));
  checks.Expect(decoded.ok(), "an empty mapping survives the wire");

  const overlay::SettingsView view = overlay::RenderSettings(decoded.value);
  const overlay::SettingsSection* psp = SectionNamed(view, "[platform.psp]");
  checks.Expect(psp != nullptr, "a platform in the map is drawn");
  if (psp == nullptr) {
    return;
  }
  checks.Expect(psp->rows.empty(), "one that maps nothing draws no folder rows");
  checks.Expect(Contains(psp->note, "skipped"), "and is marked skipped");
}

/// A platform with roms and no saves: downloads work, saves are skipped.
void CheckHalfMappedPlatform(Checks& checks) {
  const ipc::ConfigView sent = ViewFor(
      checks,
      "[server]\nurl = https://romm.example.com\n[platform.snes]\nroms = /tico/roms/snes, /old/snes\n");
  const overlay::SettingsView view = overlay::RenderSettings(sent);

  const overlay::SettingsSection* snes = SectionNamed(view, "[platform.snes]");
  checks.Expect(snes != nullptr, "the overridden platform is drawn");
  if (snes == nullptr) {
    return;
  }
  checks.ExpectEq(snes->rows.size(), static_cast<std::size_t>(4),
                  "two roms folders, and one row each for the two keys that map nowhere");

  checks.ExpectEq(snes->rows[0].value, std::string("/tico/roms/snes"), "the first roms folder");
  checks.Expect(Contains(snes->rows[0].note, "write target"),
                "the first roms entry is marked as the one that gets written to");
  checks.ExpectEq(snes->rows[1].value, std::string("/old/snes"), "the second roms folder");
  checks.Expect(!Contains(snes->rows[1].note, "write target"),
                "a later roms entry is not a write target -- it is only checked");

  checks.ExpectEq(snes->rows[2].label, std::string("saves"), "the saves key is drawn");
  checks.ExpectEq(snes->rows[2].value, std::string("nowhere"),
                  "a key that maps nowhere says so rather than drawing a blank");
  checks.Expect(Contains(snes->rows[2].note, "skipped"), "and says what that costs");
  checks.ExpectEq(snes->rows[3].label, std::string("states"), "the states key is drawn");
}

/// A folder map too large to have been sent.
void CheckTruncatedFolderMap(Checks& checks) {
  ipc::ConfigView sent;
  sent.config = config::Defaults();
  sent.config.server.url = "https://romm.example.com";
  // What `ServiceCore::GetConfig` does: the map goes whole, and the flag and a
  // notice go in its place.
  sent.config.platforms.clear();
  sent.platforms_truncated = true;
  sent.diagnostics.push_back(config::Diagnostic{
      config::Severity::kNotice, 0, "", "",
      "the folder map is too large to send over IPC; read config.ini for it"});

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  const overlay::SettingsSection* platforms = SectionNamed(view, "[platform.*]");
  checks.Expect(platforms != nullptr, "the folder map's absence is drawn");
  if (platforms == nullptr) {
    return;
  }
  checks.Expect(Contains(platforms->note, "too large"),
                "a map that did not fit says so rather than reading as an empty one");
  checks.Expect(Contains(platforms->note, "config.ini"), "and says where to read it");
  checks.Expect(!Contains(platforms->note, "nothing is mapped"),
                "a truncated map is never drawn as a console with no folders");
}

/// A map that really is empty is a different sentence from one that did not
/// fit.
void CheckEmptyFolderMap(Checks& checks) {
  ipc::ConfigView sent;
  sent.config = config::Defaults();
  sent.config.server.url = "https://romm.example.com";
  sent.config.platforms.clear();

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  const overlay::SettingsSection* platforms = SectionNamed(view, "[platform.*]");
  checks.Expect(platforms != nullptr && Contains(platforms->note, "nothing is mapped"),
                "an empty map says every platform is skipped");
  checks.Expect(platforms != nullptr && !Contains(platforms->note, "too large"),
                "and is not confused with one that did not fit");
}

// --- the complaints -----------------------------------------------------------

/// Every complaint is exactly `Describe()`, and the list is bounded.
void CheckDiagnosticsAreDescribedExactly(Checks& checks) {
  ipc::ConfigView sent;
  sent.config = config::Defaults();
  sent.config.server.url = "https://romm.example.com";
  // Past the bound on purpose. `GetConfig` trims to eight (#29), so this is a
  // shape only a host caller reaches -- and it is the bound this view owns.
  const std::size_t total = config::kMaxDiagnostics + 7;
  for (std::size_t index = 0; index < total; ++index) {
    sent.diagnostics.push_back(config::Diagnostic{config::Severity::kWarning,
                                                  static_cast<int>(index) + 1, "sync", "states",
                                                  "expected true or false"});
  }

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  checks.ExpectEq(view.complaints.size(), config::kMaxDiagnostics + 1,
                  "the list is capped, plus the line saying so");
  for (std::size_t index = 0; index < config::kMaxDiagnostics; ++index) {
    checks.ExpectEq(view.complaints[index].value, sent.diagnostics[index].Describe(),
                    "the complaint is exactly what Describe() renders");
    checks.ExpectEq(view.complaints[index].label, std::string("warning"),
                    "the severity is drawn beside it");
  }
  const overlay::Line& summary = view.complaints.back();
  checks.Expect(Contains(summary.value, "7 more"), "the summary counts what did not fit");
  checks.Expect(Contains(summary.value, "config.ini"), "and says where the rest are");
}

/// Errors first, file order within a severity.
void CheckErrorsSortFirstAndKeepFileOrder(Checks& checks) {
  ipc::ConfigView sent;
  sent.config = config::Defaults();
  sent.config.server.url = "https://romm.example.com";
  sent.diagnostics.push_back(
      config::Diagnostic{config::Severity::kNotice, 1, "sync", "", "first notice"});
  sent.diagnostics.push_back(
      config::Diagnostic{config::Severity::kWarning, 2, "sync", "", "first warning"});
  sent.diagnostics.push_back(
      config::Diagnostic{config::Severity::kError, 3, "sync", "", "the error"});
  sent.diagnostics.push_back(
      config::Diagnostic{config::Severity::kNotice, 4, "sync", "", "second notice"});

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  checks.ExpectEq(view.complaints.size(), static_cast<std::size_t>(4), "all four are drawn");
  checks.Expect(Contains(view.complaints[0].value, "the error"), "the error is first");
  checks.Expect(Contains(view.complaints[1].value, "first warning"), "then the warning");
  checks.Expect(Contains(view.complaints[2].value, "first notice"),
                "then the notices, in the order they arrived");
  checks.Expect(Contains(view.complaints[3].value, "second notice"),
                "file order survives the sort");
  checks.Expect(view.complaints[0].tone == overlay::Tone::kBad, "an error is drawn as one");
  checks.Expect(view.complaints[1].tone == overlay::Tone::kWarn, "a warning is drawn as one");
  checks.Expect(view.complaints[2].tone == overlay::Tone::kNeutral, "a notice is quiet");
}

/// The one leak this screen can cause.
///
/// A `Diagnostic` about `server.url` never quotes the URL (`config.hpp`,
/// docs/SECURITY.md), because a URL is the one configured field that can carry
/// a credential. This screen has the URL in hand and draws it in its own row --
/// what it may not do is put it next to a complaint that deliberately left it
/// out.
void CheckTheUrlIsNeverBesideAComplaint(Checks& checks) {
  // A URL with a needle in it, and a file that complains about the `[server]`
  // section for reasons that have nothing to do with the value.
  const std::string needle = "hunter2-needle.example.com";
  ipc::ConfigView sent;
  sent.config = config::Defaults();
  sent.config.server.url = "https://" + needle;
  sent.diagnostics.push_back(config::Diagnostic{
      config::Severity::kError, 2, "server", "url",
      "the configured server URL is too long to send over IPC; edit config.ini directly"});
  sent.diagnostics.push_back(config::Diagnostic{config::Severity::kWarning, 2, "server", "url",
                                                "plain http sends the token in the clear"});

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  for (const overlay::Line& line : view.complaints) {
    checks.Expect(!Contains(line.value, needle),
                  "no complaint carries the server URL: " + line.value);
    checks.Expect(!Contains(line.label, needle), "no severity label carries it either");
  }
  for (const overlay::SettingsSection& section : view.sections) {
    checks.Expect(!Contains(section.note, needle), "no section note carries the server URL");
    for (const overlay::SettingsRow& row : section.rows) {
      checks.Expect(!Contains(row.note, needle),
                    "no row's note carries the server URL: " + row.label);
    }
  }
  checks.Expect(!Contains(view.headline, needle), "and neither does the headline");
  checks.Expect(!Contains(view.hint, needle), "nor the hint");
  checks.Expect(!Contains(view.notice, needle), "nor the notice");
  checks.Expect(!Contains(view.repair.refusal, needle), "nor the button's refusal");

  // The row itself is the exception, and it is the point of the screen: this is
  // the field the user came to read.
  const overlay::SettingsRow* url = RowNamed(view, "[server]", "url");
  checks.Expect(url != nullptr && Contains(url->value, needle),
                "the url row draws the url -- it is the field this screen shows");

  // And the parser really does keep its side of it, which is what makes the
  // rule above worth asserting rather than a rule about nothing.
  const config::LoadResult parsed =
      config::ParseConfig("[server]\nurl = https://me:hunter2@" + needle + "\n");
  for (const config::Diagnostic& diagnostic : parsed.diagnostics) {
    checks.Expect(!Contains(diagnostic.Describe(), needle),
                  "the parser does not quote a rejected url either");
    checks.Expect(!Contains(diagnostic.Describe(), "hunter2"),
                  "and it certainly does not quote the credential in one");
  }
}

/// A `server.url` too long to send, which `GetConfig` withholds.
///
/// The path #26 named and no case above reached: over `ipc::kMaxServerUrlBytes`
/// the sysmodule clears the field and appends a `kError` rather than making the
/// payload unsendable (`ServiceCore::GetConfig`). Built the way that command
/// builds it, because a `ConfigView` is all the screen ever sees.
///
/// **What this pins is a known limitation, not the render #26 would have
/// chosen.** `ipc::ConfigView` carries no flag telling a withheld URL from a
/// console that never had one, so `Config::configured()` is false either way and
/// the headline is the no-server one on a console that syncs fine. What has to
/// hold regardless is that the complaint naming the real cause is on screen and
/// first, and that no line carries the URL. The fix is a flag beside
/// `platforms_truncated`, which is an `ipc::kVersion` bump; #26 records it.
void CheckAWithheldUrlSaysWhy(Checks& checks) {
  // The URL the console has and the payload does not. Over
  // `ipc::kMaxServerUrlBytes` by construction, so this is the case that reaches
  // the withholding arm rather than one that merely looks like it.
  const std::string url = "https://romm.example.com/" + std::string(ipc::kMaxServerUrlBytes, 'a');
  checks.Expect(url.size() > ipc::kMaxServerUrlBytes, "the URL really is one GetConfig withholds");

  ipc::ConfigView sent;
  sent.config = config::Defaults();
  // Exactly what `ServiceCore::GetConfig` does with one this long: the value is
  // cleared and named, never sent.
  sent.config.server.url.clear();
  std::vector<config::Diagnostic> diagnostics;
  diagnostics.push_back(config::Diagnostic{
      config::Severity::kError, 0, "server", "url",
      "the configured server URL is too long to send over IPC; edit config.ini directly"});
  sent.diagnostics = ipc::TrimDiagnostics(diagnostics);
  checks.Expect(sent.config.server.url.empty(), "the payload carries no URL at all");

  const ipc::Decoded<ipc::ConfigView> decoded = ipc::DecodeConfigView(ipc::EncodeConfigView(sent));
  checks.Expect(decoded.ok(), "a withheld URL round-trips through the wire");
  const overlay::SettingsView view = overlay::RenderSettings(decoded.value);

  const overlay::SettingsRow* row = RowNamed(view, "[server]", "url");
  checks.Expect(row != nullptr && !row->value.empty(),
                "the url row still carries text rather than a blank the eye skips");
  checks.Expect(row != nullptr && !Contains(row->value, "aaaa"),
                "and it is not the URL, which the payload never carried");

  checks.Expect(!view.complaints.empty(), "the complaint is drawn");
  if (!view.complaints.empty()) {
    checks.Expect(Contains(view.complaints.front().value, "too long"),
                  "and it is first, because it is the one that names the cause");
    checks.Expect(Contains(view.complaints.front().value, "config.ini"),
                  "and says where to fix it");
  }
  for (const overlay::Line& line : view.complaints) {
    checks.Expect(!Contains(line.value, url), "no complaint quotes the URL back");
    checks.Expect(!Contains(line.value, "aaaa"), "nor any part of it");
  }
}

// --- the menu and the button ---------------------------------------------------

/// The root menu this overlay did not have.
///
/// `SyncScreen` (#24), `LibraryScreen` (#25) and `PairingScreen` (#27) all
/// compiled and nothing pushed any of them; #26 is where they become reachable.
/// So the menu is a criterion rather than a detail.
void CheckMenuReachesEveryScreen(Checks& checks) {
  const overlay::SettingsView view = overlay::RenderSettings(ViewFor(checks, kMinimal));
  checks.Expect(!view.sections.empty(), "there are sections");
  if (view.sections.empty()) {
    return;
  }
  const overlay::SettingsSection& menu = view.sections.front();
  checks.ExpectEq(menu.title, std::string("Screens"),
                  "the menu is first: the folder map below it is one section per platform");

  bool sync = false;
  bool library = false;
  bool pairing = false;
  for (const overlay::SettingsRow& row : menu.rows) {
    const std::string what = overlay::ToString(row.destination);
    checks.Expect(row.selectable, "every menu row can be pressed: " + what);
    checks.Expect(row.kind == overlay::SettingsRowKind::kNavigate, "and is a menu row: " + what);
    checks.Expect(!row.editable, "a menu row is not a setting: " + what);
    checks.Expect(row.destination != overlay::Destination::kNone,
                  "and opens something: " + row.label);
    sync = sync || row.destination == overlay::Destination::kSync;
    library = library || row.destination == overlay::Destination::kLibrary;
    pairing = pairing || row.destination == overlay::Destination::kPairing;
  }
  checks.Expect(sync, "the sync screen is reachable");
  checks.Expect(library, "the library browser is reachable");
  checks.Expect(pairing, "the pairing screen is reachable without discarding anything");

  // Nothing outside the menu is selectable: this screen is read-only, and a row
  // that highlights is a row a user presses.
  for (std::size_t index = 1; index < view.sections.size(); ++index) {
    for (const overlay::SettingsRow& row : view.sections[index].rows) {
      checks.Expect(!row.selectable,
                    "no configuration row is selectable in this build: " + row.label);
    }
  }
}

/// Every scalar the write path can edit is marked, and nothing else is.
void CheckEditableRowsAreMarked(Checks& checks) {
  const overlay::SettingsView view = overlay::RenderSettings(ViewFor(checks, kMinimal));
  for (const char* key : {"enabled", "interval_min", "on_boot", "saves", "states",
                          "conflict_show"}) {
    const overlay::SettingsRow* row = RowNamed(view, "[sync]", key);
    checks.Expect(row != nullptr && row->editable,
                  std::string("[sync] ") + key + " is marked as a value M5-3 can edit");
  }
  const overlay::SettingsRow* url = RowNamed(view, "[server]", "url");
  checks.Expect(url != nullptr && url->editable, "so is the url");
}

/// "Re-pair" asks twice, and asks the sysmodule before it discards anything.
void CheckRepairConfirmsBeforeItAsks(Checks& checks) {
  const ipc::ConfigView sent = ViewFor(checks, kMinimal);

  const overlay::SettingsView idle = overlay::RenderSettings(sent);
  checks.Expect(idle.repair.state == overlay::ControlState::kLive,
                "a paired, configured console offers Re-pair");
  checks.Expect(!idle.repair.label.empty(), "the button carries a label");
  checks.Expect(idle.repair.refusal.empty(), "a live button has no refusal");
  checks.Expect(idle.notice.empty(), "and nothing has happened yet");

  overlay::RepairState confirming;
  confirming.confirming = true;
  const overlay::SettingsView asked = overlay::RenderSettings(sent, confirming);
  checks.Expect(asked.repair.state == overlay::ControlState::kLive, "the second press is live");
  checks.Expect(asked.repair.label != idle.repair.label,
                "the button says the next press is the one that does it");
  checks.Expect(Contains(asked.notice, "discards"),
                "a destructive action on a console with no dialog says so first");
  checks.Expect(asked.notice_tone == overlay::Tone::kWarn, "and says it as a warning");
}

/// What each answer to a press says.
///
/// The three refusals all have to say the console was left alone. That is the
/// whole reason the button sends `StartPair` first: `SdEngine::StartPairing` is
/// `kUnavailable` today, and a button that discarded the token before finding
/// that out would leave a console that cannot pair again from the overlay.
void CheckEveryRepairOutcomeIsDrawn(Checks& checks) {
  const ipc::ConfigView sent = ViewFor(checks, kMinimal);

  for (const overlay::RepairOutcome outcome :
       {overlay::RepairOutcome::kNotConfigured, overlay::RepairOutcome::kUnavailable,
        overlay::RepairOutcome::kRefused, overlay::RepairOutcome::kUnpairFailed}) {
    overlay::RepairState state;
    state.outcome = outcome;
    const overlay::SettingsView view = overlay::RenderSettings(sent, state);
    checks.Expect(!view.notice.empty(), std::string("the answer is drawn: ") +
                                            std::to_string(static_cast<int>(outcome)));
    checks.ExpectEq(view.notice, overlay::RepairOutcomeText(outcome),
                    "and it is the published sentence for it");
  }

  checks.Expect(Contains(overlay::RepairOutcomeText(overlay::RepairOutcome::kUnavailable),
                         "still paired"),
                "a sysmodule that cannot pair leaves the pairing alone, and says so");
  checks.Expect(
      Contains(overlay::RepairOutcomeText(overlay::RepairOutcome::kRefused), "still paired"),
      "so does any other refusal");
  checks.Expect(Contains(overlay::RepairOutcomeText(overlay::RepairOutcome::kNotConfigured),
                         "nothing was changed"),
                "and so does a console with no server");
  checks.Expect(
      overlay::RepairOutcomeTone(overlay::RepairOutcome::kUnpairFailed) == overlay::Tone::kBad,
      "a pairing that restarted over a token that would not go is the one bad outcome");
  checks.Expect(overlay::RepairOutcomeText(overlay::RepairOutcome::kNone).empty(),
                "nothing pressed says nothing");
}

/// Every refusal the sysmodule can name is read the same way on and off console.
///
/// `overlay/AGENTS.md`: what a screen *says* is a decision and lives in `core/`,
/// where a host test can reach it. This mapping was written inside
/// `settings_screen.cpp` first, which put the difference between "this console
/// is still paired" and a sentence that does not say so beyond the reach of
/// every test in this file.
void CheckEveryRefusalIsRead(Checks& checks) {
  checks.Expect(overlay::RepairOutcomeFor(ipc::Error::kNotConfigured) ==
                    overlay::RepairOutcome::kNotConfigured,
                "a console with no server is told which of the two it is");
  checks.Expect(
      overlay::RepairOutcomeFor(ipc::Error::kUnavailable) == overlay::RepairOutcome::kUnavailable,
      "and so is one whose sysmodule cannot start a pairing -- the outcome this button is gated "
      "on");

  // Everything else, including `kOk`: this is only asked about a `StartPair`
  // that failed, so there is no arm that reads as a success -- and every arm
  // has to leave the console described as still paired, because the token is
  // not discarded until `StartPair` has answered.
  for (const ipc::Error error : ipc::kAllErrors) {
    const overlay::RepairOutcome outcome = overlay::RepairOutcomeFor(error);
    if (error == ipc::Error::kNotConfigured || error == ipc::Error::kUnavailable) {
      continue;
    }
    checks.Expect(outcome == overlay::RepairOutcome::kRefused,
                  std::string("a refusal this screen has no separate sentence for is one "
                              "sentence: ") +
                      ipc::ToString(error));
  }
  for (const ipc::Error error : ipc::kAllErrors) {
    const overlay::RepairOutcome outcome = overlay::RepairOutcomeFor(error);
    checks.Expect(outcome != overlay::RepairOutcome::kNone,
                  std::string("a refusal is never read as 'nothing was pressed': ") +
                      ipc::ToString(error));
    checks.Expect(outcome != overlay::RepairOutcome::kUnpairFailed,
                  std::string("nor as a token that was discarded -- a refused StartPair never "
                              "reached Unpair: ") +
                      ipc::ToString(error));
  }
}

/// The headline never states a count the payload cannot support.
///
/// `GetConfig` sends at most `ipc::kMaxDiagnosticsInPayload` complaints and a
/// `kNotice` saying how many did not fit, so a headline counting the list it was
/// handed reads "config.ini has 7 warnings" over a file with twenty-three. This
/// is the one screen whose job is telling a user what the parser found, and it
/// is also the one place that number can be checked against nothing.
void CheckTheHeadlineDoesNotCountATrimmedList(Checks& checks) {
  std::string text = "[server]\nurl = https://romm.example.com\n[sync]\n";
  for (int line = 0; line < 12; ++line) {
    text += "states = maybe\n";
  }
  const config::LoadResult parsed = config::ParseConfig(text);
  checks.Expect(parsed.diagnostics.size() > ipc::kMaxDiagnosticsInPayload,
                "the file really does produce more complaints than one payload carries");

  const ipc::ConfigView sent = ViewFor(checks, text);
  checks.Expect(sent.diagnostics.size() <= ipc::kMaxDiagnosticsInPayload,
                "and the wire really did trim them");

  const overlay::SettingsView view = overlay::RenderSettings(sent);
  // Whatever the wording, it may not be a number: every number this screen could
  // produce here is the trimmed one, and the untrimmed count is not in the
  // payload. `Status::config_error_count` is computed from the *untrimmed* list
  // (`ipc_service.cpp`), so a count here would also disagree with the status
  // screen about the same file.
  for (char digit = '0'; digit <= '9'; ++digit) {
    checks.Expect(view.headline.find(digit) == std::string::npos,
                  std::string("the headline states no count over a trimmed list: ") +
                      view.headline);
  }
  checks.Expect(Contains(view.headline, "config.ini"), "it still names the file");
  checks.Expect(view.tone == overlay::Tone::kWarn, "and still reads as a warning");
  checks.Expect(!view.complaints.empty(), "and the complaints are still under it");
}

/// A half-pressed "Re-pair" does not survive the button going blocked.
///
/// The state with no exit: `configured()` going false between the two presses
/// left the confirmation warning drawn under a button that refuses every press,
/// and nothing on the screen cleared it.
void CheckAConfirmationNeverSitsUnderABlockedButton(Checks& checks) {
  overlay::RepairState confirming;
  confirming.confirming = true;

  const overlay::SettingsView blocked =
      overlay::RenderSettings(ViewFor(checks, "[sync]\nenabled = true\n"), confirming);
  checks.Expect(blocked.repair.state == overlay::ControlState::kBlocked,
                "a console with no server blocks the button");
  checks.Expect(!Contains(blocked.notice, "discards"),
                "and draws no warning about a discard no press can reach");
  checks.Expect(!blocked.repair.refusal.empty(), "the refusal under it is what says why");

  // ...and the live console is unchanged: this is a guard on one arm, not a
  // confirmation that stopped confirming.
  const overlay::SettingsView live = overlay::RenderSettings(ViewFor(checks, kMinimal), confirming);
  checks.Expect(Contains(live.notice, "discards"), "a live button still asks first");
}

/// The no-server headline comes first, and Re-pair is still there under it.
void CheckNoServerLeadsAndStillOffersRepair(Checks& checks) {
  // A file with a warning *and* no usable server: the warning is real and is
  // not the first thing to fix.
  const ipc::ConfigView sent =
      ViewFor(checks, "[sync]\nstates = maybe\ninterval_min = -5\n");
  const overlay::SettingsView view = overlay::RenderSettings(sent);

  checks.ExpectEq(view.headline, std::string("No server set"),
                  "no server outranks every other complaint");
  checks.Expect(view.complaints.size() >= 3,
                "the warnings are still drawn under it, and so is the error");

  checks.Expect(view.repair.state == overlay::ControlState::kBlocked,
                "Re-pair is reachable on a console with no server");
  checks.Expect(!view.repair.label.empty(), "it carries a label");
  checks.Expect(!view.repair.refusal.empty(),
                "and pressing it says why it would not work, rather than doing nothing");
  checks.Expect(Contains(view.repair.refusal, "server"), "which is the server it has not got");
}

// --- the states that are not a configuration ----------------------------------

/// A sysmodule that could not be asked.
void CheckUnreachable(Checks& checks) {
  for (const overlay::Link link :
       {overlay::Link::kNotRunning, overlay::Link::kUnreadable, overlay::Link::kIncompatible}) {
    const overlay::SettingsView view = overlay::RenderSettingsUnreachable(link, 3);
    const overlay::StatusView diagnosis = overlay::RenderUnreachable(link, 3);
    checks.Expect(view.link == link, "the link is reported");
    checks.ExpectEq(view.headline, diagnosis.headline,
                    "the wording is the status screen's -- it is the same diagnosis");
    checks.ExpectEq(view.hint, diagnosis.hint, "and so is the hint");
    checks.Expect(view.tone == diagnosis.tone, "and the tone");
    checks.Expect(view.sections.empty(),
                  "no rows: every value would be one this overlay does not have");
    checks.Expect(view.complaints.empty(), "and no complaints, which would be the same");
    checks.Expect(view.repair.state == overlay::ControlState::kInert,
                  "there is nothing to press against");
    checks.Expect(!view.repair.label.empty(), "the button still carries a label");
    checks.Expect(view.repair.refusal.empty(),
                  "and no second sentence: the headline is already saying why");
  }
}

/// `interval_min = 0` is a setting, not a missing one.
void CheckIntervalWording(Checks& checks) {
  checks.ExpectEq(overlay::FormatInterval(0), std::string("only on boot and on demand"),
                  "0 is the documented 'boot and manual', never 'every 0 minutes'");
  checks.ExpectEq(overlay::FormatInterval(1), std::string("1 minute"), "one is singular");
  checks.ExpectEq(overlay::FormatInterval(30), std::string("30 minutes"), "and the rest are not");
  checks.ExpectEq(overlay::FormatInterval(config::kMaxIntervalMinutes),
                  std::string("10080 minutes"), "the clamped ceiling is drawn as it stands");

  const ipc::ConfigView sent =
      ViewFor(checks, "[server]\nurl = https://romm.example.com\n[sync]\ninterval_min = 0\n");
  const overlay::SettingsView view = overlay::RenderSettings(sent);
  const overlay::SettingsRow* row = RowNamed(view, "[sync]", "interval_min");
  checks.Expect(row != nullptr && row->value == "only on boot and on demand",
                "and the row reads that way too");
}

/// Every row a user can see carries text.
///
/// The rule the whole overlay keeps (overlay/AGENTS.md): a value drawn as an
/// empty string is indistinguishable from a screen that failed to read
/// something.
void CheckEveryRowCarriesText(Checks& checks) {
  const std::string cases[] = {
      std::string(),
      kMinimal,
      "[server]\nurl = https://romm.example.com\n[sync]\nstates = maybe\ninterval_min = 0\n",
      "[server]\nurl = ftp://nope\n",
      "[server]\nurl = https://romm.example.com\n[platform.psp]\nroms =\nsaves =\nstates =\n",
  };
  for (const std::string& text : cases) {
    const overlay::SettingsView view = overlay::RenderSettings(ViewFor(checks, text));
    checks.Expect(!view.headline.empty(), "the headline is never empty");
    checks.Expect(!view.repair.label.empty(), "the button always says what it is");
    for (const overlay::Line& line : view.complaints) {
      checks.Expect(!line.label.empty(), "a complaint names its severity");
      checks.Expect(!line.value.empty(), "and says something");
    }
    for (const overlay::SettingsSection& section : view.sections) {
      checks.Expect(!section.title.empty(), "a section is titled");
      checks.Expect(!section.rows.empty() || !section.note.empty(),
                    "a section with no rows says why: " + section.title);
      for (const overlay::SettingsRow& row : section.rows) {
        checks.Expect(!row.label.empty(), "a row is labelled");
        checks.Expect(!row.value.empty(), "and carries a value: " + row.label);
      }
    }
  }
}

// --- the code rather than the behaviour ---------------------------------------

/// `needle` in `haystack`, but not as the tail of a longer identifier.
///
/// `overlay.library`'s, for its reason: `sync::` is in `rommsync::overlay`, and
/// a grep that did not know that would fail every file in this directory on its
/// own namespace.
bool NamesToken(const std::string& haystack, const std::string& needle) {
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + 1)) {
    if (at == 0) {
      return true;
    }
    const char before = haystack[at - 1];
    const bool identifier = (before >= 'a' && before <= 'z') ||
                            (before >= 'A' && before <= 'Z') ||
                            (before >= '0' && before <= '9') || before == '_' || before == ':';
    if (!identifier) {
      return true;
    }
  }
  return false;
}

template <std::size_t N>
void ScanForbidden(Checks& checks, const std::filesystem::path& path,
                   const char* const (&forbidden)[N], const std::string& why) {
  std::ifstream file(path);
  checks.Expect(file.good(), "the source is readable: " + path.string());
  std::string line;
  int number = 0;
  while (std::getline(file, line)) {
    ++number;
    const std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
      continue;
    }
    for (const char* token : forbidden) {
      checks.Expect(!NamesToken(line, token), path.filename().string() + ":" +
                                                  std::to_string(number) + " names " + token +
                                                  "; " + why);
    }
  }
}

/// Nothing under `overlay/` writes `config.ini`.
///
/// The same grep `overlay.sync_actions` and `overlay.library` run, repeated
/// here for the reason #25 repeated it: it scans a *directory*, so a rule about
/// that directory is only as good as the suites that scan it -- and this is the
/// issue that adds a screen whose whole subject is the file the sysmodule owns.
/// Reading `config.ini` is allowed (docs/ARCHITECTURE.md); writing it is not.
void CheckSettingsScreenWritesNothing(Checks& checks) {
  static constexpr const char* kForbidden[] = {
      "ofstream",  "fopen", "fwrite", "WriteAtomically",     "atomic_file",
      "ApplyEdit", "boot2", "flags/", "atmosphere/contents", "SetSyncEnabled",
  };

  bool found_the_screen = false;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(ROMMSYNC_OVERLAY_SOURCE_DIR)) {
    const std::filesystem::path path = entry.path();
    const std::string extension = path.extension().string();
    if (extension != ".cpp" && extension != ".hpp") {
      continue;
    }
    if (path.filename().string() == "settings_screen.cpp") {
      found_the_screen = true;
    }
    ScanForbidden(checks, path, kForbidden, "the sysmodule owns every write");
  }
  checks.Expect(found_the_screen, "the settings screen is in overlay/source/ and was scanned");
}

/// The settings screen calls no engine.
///
/// It is the screen most likely to reach for one: it renders `config::` types,
/// and `config::LoadConfig` is one include away from a file this build already
/// compiles into the overlay.
void CheckSettingsScreenCallsNoEngine(Checks& checks) {
  static constexpr const char* kForbidden[] = {
      "LoadConfig", "ParseConfig", "download::", "http::", "sync::", "auth::Gate", "state_db",
  };
  ScanForbidden(checks, std::filesystem::path(ROMMSYNC_OVERLAY_SOURCE_DIR) / "settings_screen.cpp",
                kForbidden,
                "the overlay renders what the sysmodule reports and calls no engine");
}

/// The view model names no libnx and no libultrahand type (hard rule 4).
void CheckViewStaysPortable(Checks& checks) {
  static constexpr const char* kForbidden[] = {
      "switch.h", "tesla.hpp", "Result", "tsl::", "libnx", "MAKERESULT",
  };
  for (const char* path : {ROMMSYNC_SETTINGS_VIEW_HDR, ROMMSYNC_SETTINGS_VIEW_SRC}) {
    ScanForbidden(checks, path, kForbidden,
                  "core/ names no host-only or libnx type (hard rule 4)");
  }
}

}  // namespace

int main() {
  Checks checks;
  CheckNoConfigFileAtAll(checks);
  CheckErrorConfig(checks);
  CheckErrorOverAConfiguredConsole(checks);
  CheckWarningsAndNotices(checks);
  CheckEmptiedPlatform(checks);
  CheckPlatformMappedNowhere(checks);
  CheckHalfMappedPlatform(checks);
  CheckTruncatedFolderMap(checks);
  CheckEmptyFolderMap(checks);
  CheckDiagnosticsAreDescribedExactly(checks);
  CheckErrorsSortFirstAndKeepFileOrder(checks);
  CheckTheUrlIsNeverBesideAComplaint(checks);
  CheckMenuReachesEveryScreen(checks);
  CheckEditableRowsAreMarked(checks);
  CheckRepairConfirmsBeforeItAsks(checks);
  CheckEveryRepairOutcomeIsDrawn(checks);
  CheckTheHeadlineDoesNotCountATrimmedList(checks);
  CheckAConfirmationNeverSitsUnderABlockedButton(checks);
  CheckAWithheldUrlSaysWhy(checks);
  CheckEveryRefusalIsRead(checks);
  CheckNoServerLeadsAndStillOffersRepair(checks);
  CheckUnreachable(checks);
  CheckIntervalWording(checks);
  CheckEveryRowCarriesText(checks);
  CheckSettingsScreenWritesNothing(checks);
  CheckSettingsScreenCallsNoEngine(checks);
  CheckViewStaysPortable(checks);

  if (checks.failures() > 0) {
    std::cerr << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "ok: the settings screen shows what is in force, and what the parser said\n";
  return 0;
}

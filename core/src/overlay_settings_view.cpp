#include "rommsync/overlay_settings_view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "rommsync/overlay_sync_actions.hpp"

namespace rommsync::overlay {
namespace {

/// The two spellings of a boolean this screen uses.
///
/// "On"/"Off" rather than `config.ini`'s `true`/`false`: the row is read in a
/// list beside "30 minutes" and "the write target", and the file's own spelling
/// is a detail of a file the user is not looking at. The moment a row becomes
/// editable (M5-3, #30) the *edit* still speaks `config.ini`'s grammar, which
/// is `config::ParseBool`'s and not this function's.
const char* BoolText(bool value) { return value ? "On" : "Off"; }

SettingsRow Value(std::string label, std::string value) {
  SettingsRow row;
  row.kind = SettingsRowKind::kValue;
  row.label = std::move(label);
  row.value = std::move(value);
  // Every scalar in `[server]`, `[sync]` and `[downloads]` is a key
  // `config::ApplyEdit` accepts, so every one of them is marked. Nothing here
  // offers the edit -- see the header note.
  row.editable = true;
  return row;
}

SettingsRow Folder(std::string label, std::string value, std::string note) {
  SettingsRow row;
  row.kind = SettingsRowKind::kFolder;
  row.label = std::move(label);
  row.value = std::move(value);
  row.note = std::move(note);
  row.editable = true;
  return row;
}

SettingsRow Navigate(std::string label, std::string value, Destination destination) {
  SettingsRow row;
  row.kind = SettingsRowKind::kNavigate;
  row.label = std::move(label);
  row.value = std::move(value);
  row.selectable = true;
  row.destination = destination;
  return row;
}

/// The menu the overlay did not have until this screen.
///
/// It is first rather than last, and that is deliberate: the folder map is one
/// section per platform and the built-in map alone is eleven of them, so a menu
/// under it is a menu nobody scrolls to.
SettingsSection Menu() {
  SettingsSection section;
  section.title = "Screens";
  section.rows.push_back(Navigate("Sync", "the switch and Sync now", Destination::kSync));
  section.rows.push_back(
      Navigate("Library", "browse roms and the queue", Destination::kLibrary));
  // Reaching the pairing screen without discarding anything. "Re-pair" is the
  // destructive way in and is a button rather than a row; this is how a user
  // reads a code that is already on its way (#27).
  section.rows.push_back(Navigate("Pairing", "the pairing code", Destination::kPairing));
  return section;
}

SettingsSection ServerSection(const config::Config& config) {
  SettingsSection section;
  section.title = "[server]";
  if (config.server.url.empty()) {
    // "(blank)" rather than "not set": the field is also blank when the URL was
    // withheld for being longer than `ipc::kMaxServerUrlBytes`, which is a URL
    // that *is* set, and the complaint above already says which of the two this
    // console is. Claiming the file has no `url` would send a user looking for
    // a line that is there.
    SettingsRow row = Value("url", "(blank)");
    row.note = "no server: this console does not sync";
    row.tone = Tone::kBad;
    section.rows.push_back(std::move(row));
    return section;
  }
  section.rows.push_back(Value("url", config.server.url));
  return section;
}

SettingsSection SyncSection(const config::SyncConfig& sync) {
  SettingsSection section;
  section.title = "[sync]";
  section.rows.push_back(Value("enabled", BoolText(sync.enabled)));
  section.rows.push_back(Value("interval_min", FormatInterval(sync.interval_min)));
  section.rows.push_back(Value("on_boot", BoolText(sync.on_boot)));
  section.rows.push_back(Value("saves", BoolText(sync.saves)));
  section.rows.push_back(Value("states", BoolText(sync.states)));
  section.rows.push_back(Value("conflict_show", BoolText(sync.conflict_show)));
  return section;
}

SettingsSection DownloadsSection(const config::DownloadsConfig& downloads) {
  SettingsSection section;
  section.title = "[downloads]";
  section.rows.push_back(Value("enabled", BoolText(downloads.enabled)));
  section.rows.push_back(Value("verify_hash", BoolText(downloads.verify_hash)));
  section.rows.push_back(Value("resume", BoolText(downloads.resume)));
  return section;
}

/// One `roms`/`saves`/`states` key of one platform.
///
/// A key with no paths is a row rather than nothing at all. An absent line and
/// a line the parser dropped look identical on a screen that omits both, and
/// "downloads work, saves are skipped for it" is exactly the state docs/CONFIG.md
/// promises to say out loud.
void AddFolderRows(SettingsSection* section, const char* key,
                   const std::vector<std::string>& paths, const char* skipped) {
  if (paths.empty()) {
    SettingsRow row = Folder(key, "nowhere", skipped);
    section->rows.push_back(std::move(row));
    return;
  }
  const bool roms = std::string_view(key) == "roms";
  for (std::size_t index = 0; index < paths.size(); ++index) {
    // Only the first `roms` entry is ever written to; the rest are where the
    // client looks to see whether a rom is already on the card
    // (docs/CONFIG.md). A screen that drew them identically would have a user
    // reorder the list to move their downloads and watch nothing move.
    const char* note = "";
    if (roms) {
      note = index == 0 ? "the write target" : "checked for roms already here";
    }
    section->rows.push_back(Folder(key, paths[index], note));
  }
}

void AddPlatformSections(SettingsView* view, const ipc::ConfigView& config) {
  if (config.platforms_truncated) {
    // Never an empty map: that would tell the user they had configured no
    // folders, which is the one thing this flag exists to prevent
    // (`ipc::ConfigView`).
    SettingsSection section;
    section.title = "[platform.*]";
    section.note = "too large to show here -- read config.ini for the folder map";
    section.note_tone = Tone::kWarn;
    view->sections.push_back(std::move(section));
    return;
  }
  if (config.config.platforms.empty()) {
    // Not the same thing as the flag above, and it says so: the map really is
    // empty, which means every platform is skipped. It takes a `config.ini`
    // that emptied the built-in map on purpose to get here.
    SettingsSection section;
    section.title = "[platform.*]";
    section.note = "nothing is mapped: every platform is skipped";
    section.note_tone = Tone::kWarn;
    view->sections.push_back(std::move(section));
    return;
  }

  for (const auto& [slug, folders] : config.config.platforms) {
    SettingsSection section;
    section.title = "[platform." + slug + "]";
    if (folders.empty()) {
      // A section emptied on purpose is how a platform is switched off
      // (docs/CONFIG.md). Drawn as one sentence rather than as three "nowhere"
      // rows saying the same thing three times.
      section.note = "skipped: no folders mapped";
      view->sections.push_back(std::move(section));
      continue;
    }
    AddFolderRows(&section, "roms", folders.roms, "downloads are skipped for this platform");
    AddFolderRows(&section, "saves", folders.saves, "saves are skipped for this platform");
    AddFolderRows(&section, "states", folders.states, "states are skipped for this platform");
    view->sections.push_back(std::move(section));
  }
}

/// How the complaints sort: `kError` first, and file order within a severity.
///
/// A stable sort rather than a comparison that also orders by line, because
/// "in file order" is the order `GetConfig` sent them in -- it has already
/// dropped some, and re-deriving an order from line numbers would put a
/// whole-file complaint (`line == 0`) above the line that caused it.
int SeverityRank(config::Severity severity) {
  switch (severity) {
    case config::Severity::kError:
      return 0;
    case config::Severity::kWarning:
      return 1;
    case config::Severity::kNotice:
      break;
  }
  return 2;
}

Tone ToneFor(config::Severity severity) {
  switch (severity) {
    case config::Severity::kError:
      return Tone::kBad;
    case config::Severity::kWarning:
      return Tone::kWarn;
    case config::Severity::kNotice:
      break;
  }
  return Tone::kNeutral;
}

void AddComplaints(SettingsView* view, const std::vector<config::Diagnostic>& diagnostics) {
  std::vector<config::Diagnostic> ordered = diagnostics;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const config::Diagnostic& left, const config::Diagnostic& right) {
                     return SeverityRank(left.severity) < SeverityRank(right.severity);
                   });

  const std::size_t shown = std::min<std::size_t>(ordered.size(), config::kMaxDiagnostics);
  for (std::size_t index = 0; index < shown; ++index) {
    const config::Diagnostic& diagnostic = ordered[index];
    // `Describe()` exactly. The line number and the section are the difference
    // between a fix and an evening, and a screen that reworded them would be a
    // second grammar to keep in step with the parser's -- and the one place a
    // `server.url` could get pasted back beside a message that deliberately
    // omitted it.
    view->complaints.push_back(
        Line{config::ToString(diagnostic.severity), diagnostic.Describe(),
             ToneFor(diagnostic.severity)});
  }
  if (ordered.size() > shown) {
    view->complaints.push_back(Line{"notice",
                                    std::to_string(ordered.size() - shown) +
                                        " more are not shown here; read config.ini for them",
                                    Tone::kNeutral});
  }
}

std::size_t CountOf(const std::vector<config::Diagnostic>& diagnostics,
                    config::Severity severity) {
  std::size_t count = 0;
  for (const config::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == severity) {
      ++count;
    }
  }
  return count;
}

std::string Plural(std::size_t count, const char* singular, const char* plural) {
  return std::to_string(count) + " " + (count == 1 ? singular : plural);
}

/// The headline, in remedy order.
///
/// `configured() == false` comes first whatever else is wrong, because a
/// console with no server does nothing at all -- a warning about a folder path
/// is not the first thing to fix on one, and putting it on top would send a
/// user to the wrong line (#26).
void SetHeadline(SettingsView* view, const ipc::ConfigView& config) {
  if (!config.config.configured()) {
    view->headline = "No server set";
    // Not "set it here": this screen is read-only in this build, and a hint
    // that offers what the screen cannot do is worse than none.
    view->hint = "Set url under [server] in config.ini";
    view->tone = Tone::kBad;
    return;
  }
  const std::size_t errors = CountOf(config.diagnostics, config::Severity::kError);
  if (errors > 0) {
    view->headline = "config.ini has " + Plural(errors, "error", "errors");
    view->hint = "The values below are the ones in force";
    view->tone = Tone::kBad;
    return;
  }
  const std::size_t warnings = CountOf(config.diagnostics, config::Severity::kWarning);
  if (warnings > 0) {
    view->headline = "config.ini has " + Plural(warnings, "warning", "warnings");
    view->hint = "Those lines did not take effect as written";
    view->tone = Tone::kWarn;
    return;
  }
  view->headline = "Settings in force";
  view->tone = Tone::kGood;
}

/// The button, and what the next press does.
///
/// Blocked rather than absent on a console with no server: `StartPair` answers
/// `kNotConfigured` for exactly that console (`ipc::ServiceCore::StartPair`),
/// so the refusal is predicted here for the reason the sync screen predicts its
/// own -- a screen that sends a command it knows the answer to makes the
/// sentence arrive a frame later, and a control that vanishes is one a user
/// cannot find to learn why.
void SetRepair(SettingsView* view, const ipc::ConfigView& config, const RepairState& repair) {
  if (!config.config.configured()) {
    view->repair =
        Button{ControlState::kBlocked, "Re-pair",
               "there is no server to pair with: set url under [server] first", Tone::kWarn};
  } else if (repair.confirming) {
    view->repair = Button{ControlState::kLive, "Re-pair -- press again to confirm",
                          std::string(), Tone::kNeutral};
  } else {
    view->repair = Button{ControlState::kLive, "Re-pair", std::string(), Tone::kNeutral};
  }

  if (repair.confirming) {
    // The sentence a destructive action owes a console with no dialog. It is
    // the notice rather than the button's `refusal`, which is what a press that
    // will not go through says.
    //
    // It names the pairing *first* because that is the order the press runs in
    // and the order that decides what a refusal costs: nothing is discarded
    // until a new pairing is genuinely starting, so a press that gets no
    // further leaves the console exactly as it was. A sentence promising the
    // discard outright would be a promise this button deliberately does not
    // make (`overlay_settings_view.hpp`).
    view->notice = "This starts a new pairing, then discards the one on this console";
    view->notice_tone = Tone::kWarn;
    return;
  }
  if (repair.outcome != RepairOutcome::kNone) {
    view->notice = RepairOutcomeText(repair.outcome);
    view->notice_tone = RepairOutcomeTone(repair.outcome);
  }
}

}  // namespace

const char* ToString(Destination destination) {
  switch (destination) {
    case Destination::kNone:
      return "none";
    case Destination::kSync:
      return "sync";
    case Destination::kLibrary:
      return "library";
    case Destination::kPairing:
      return "pairing";
  }
  return "none";
}

RepairOutcome RepairOutcomeFor(ipc::Error error) {
  switch (error) {
    case ipc::Error::kNotConfigured:
      return RepairOutcome::kNotConfigured;
    case ipc::Error::kUnavailable:
      return RepairOutcome::kUnavailable;
    default:
      break;
  }
  // Including `kOk`. This is only asked about a `StartPair` that failed, and a
  // refusal that named no error is a refusal -- reading it as anything else
  // would draw a pairing that is not happening.
  return RepairOutcome::kRefused;
}

std::string RepairOutcomeText(RepairOutcome outcome) {
  switch (outcome) {
    case RepairOutcome::kNone:
      return std::string();
    case RepairOutcome::kNotConfigured:
      return "There is no server to pair with; nothing was changed";
    case RepairOutcome::kUnavailable:
      // The asymmetry this button is gated on, in a sentence: the console is
      // exactly as it was, and saying so is the difference between a user
      // waiting for a code and a user thinking they have just lost their
      // pairing.
      return "This sysmodule cannot start a pairing yet; this console is still paired";
    case RepairOutcome::kRefused:
      return "sys-rommsync refused to start a pairing; this console is still paired";
    case RepairOutcome::kUnpairFailed:
      break;
  }
  return "Pairing restarted, and the old token could not be discarded; pair again to replace it";
}

Tone RepairOutcomeTone(RepairOutcome outcome) {
  switch (outcome) {
    case RepairOutcome::kNone:
      return Tone::kNeutral;
    case RepairOutcome::kNotConfigured:
    case RepairOutcome::kUnavailable:
    case RepairOutcome::kRefused:
      return Tone::kWarn;
    case RepairOutcome::kUnpairFailed:
      break;
  }
  return Tone::kBad;
}

std::string FormatInterval(int interval_min) {
  if (interval_min == 0) {
    // The documented setting, not the absence of one (docs/CONFIG.md). "every 0
    // minutes" is the reading this exists to prevent.
    return "only on boot and on demand";
  }
  return std::to_string(interval_min) + (interval_min == 1 ? " minute" : " minutes");
}

SettingsView RenderSettings(const ipc::ConfigView& config, const RepairState& repair) {
  SettingsView view;
  view.link = Link::kOk;
  SetHeadline(&view, config);
  AddComplaints(&view, config.diagnostics);

  view.sections.push_back(Menu());
  view.sections.push_back(ServerSection(config.config));
  view.sections.push_back(SyncSection(config.config.sync));
  view.sections.push_back(DownloadsSection(config.config.downloads));
  AddPlatformSections(&view, config);

  SetRepair(&view, config, repair);
  return view;
}

SettingsView RenderSettingsUnreachable(Link link, std::uint32_t sysmodule_interface) {
  // The headline, hint and tone are the status screen's, because it is the same
  // diagnosis and a console that says two different things about one missing
  // sysmodule reads as two problems.
  const StatusView diagnosis = RenderUnreachable(link, sysmodule_interface);

  SettingsView view;
  view.link = diagnosis.link;
  view.headline = diagnosis.headline;
  view.hint = diagnosis.hint;
  view.tone = diagnosis.tone;

  // No sections, and no menu: every value would be one this overlay does not
  // have, and every screen a menu row reached would draw this same sentence
  // from this same session.
  view.repair = Button{ControlState::kInert, "Re-pair", std::string(), Tone::kNeutral};
  return view;
}

}  // namespace rommsync::overlay

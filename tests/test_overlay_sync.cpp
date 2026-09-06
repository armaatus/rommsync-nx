// The overlay's sync screen: which control may be pressed, and what a press
// that may not says.
//
// M4-2 (#24). The screen itself cannot be checked before the M8-1 gate --
// nothing in this repo draws a frame -- so everything about it that is a
// *decision* lives in `rommsync::overlay::SyncActionsView`, and this is what
// holds it.
//
// The centrepiece is `CheckPredictionMatchesTheSysmodule`: the screen greys
// "Sync now" by predicting what `SyncNow` would answer, and a prediction that
// drifts from `ipc::ServiceCore::SyncNow` is either a button that lies or a
// spinner that never moves. So the two are held against each other over the
// whole cross-product of the four inputs `ServiceCore` decides on, through a
// real `ServiceCore` over a fake `Engine` -- not against a second copy of the
// table written out here, which would agree with itself forever.
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
#include "rommsync/overlay_status_view.hpp"
#include "rommsync/overlay_sync_actions.hpp"

namespace {

namespace config = rommsync::config;
namespace ipc = rommsync::ipc;
namespace overlay = rommsync::overlay;

using checks::Checks;

/// An `ipc::Engine` that is knobs and nothing else, so what is under test is
/// `ServiceCore`'s decisions rather than an engine's opinions. The same shape
/// `test_ipc.cpp`'s `FakeEngine` has, cut to the four things this suite turns.
class KnobEngine : public ipc::Engine {
 public:
  config::Config settings = config::Defaults();
  ipc::EngineSnapshot snapshot;
  int sync_requests = 0;

  const config::Config& config() const override { return settings; }
  const std::vector<config::Diagnostic>& config_diagnostics() const override { return notes_; }
  ipc::EngineSnapshot Snapshot() const override { return snapshot; }
  rommsync::auth::PairingStatus pairing_status() const override { return {}; }

  ipc::Error SetSyncEnabled(bool enabled) override {
    settings.sync.enabled = enabled;
    return ipc::Error::kOk;
  }
  ipc::Error ApplyConfigEdit(const ipc::ConfigEdit&, std::vector<config::Diagnostic>*) override {
    return ipc::Error::kOk;
  }
  /// True unless a tick is already running, which is the one thing
  /// `ServiceCore::SyncNow` reads it for -- and the same fact `Status` carries
  /// as `sync_in_progress`, so the two cannot disagree here either.
  bool RequestSync() override {
    ++sync_requests;
    return !snapshot.sync_in_progress;
  }
  ipc::Error StartPairing() override { return ipc::Error::kOk; }
  ipc::Error Unpair() override { return ipc::Error::kOk; }
  ipc::Error Enqueue(std::int64_t, std::int32_t*) override { return ipc::Error::kOk; }
  ipc::Error Dequeue(std::int64_t) override { return ipc::Error::kOk; }
  ipc::Error ListBegin(const ipc::ListRequest&, ipc::Cursor*) override {
    return ipc::Error::kOk;
  }
  ipc::Error ListNext(ipc::Cursor, ipc::ListPage*) override { return ipc::Error::kOk; }
  ipc::Error ListEnd(ipc::Cursor) override { return ipc::Error::kOk; }

 private:
  std::vector<config::Diagnostic> notes_;
};

/// The four things this console can differ in, as far as `SyncNow` cares.
struct Console {
  bool configured = true;
  ipc::AuthState auth = ipc::AuthState::kPaired;
  bool enabled = true;
  bool sync_in_progress = false;
};

/// A working console. Every scenario below takes this and changes one thing, so
/// what a scenario is about is the field it touches.
Console Working() { return Console{}; }

void Load(KnobEngine* engine, const Console& console) {
  engine->settings = config::Defaults();
  engine->settings.server.url = console.configured ? "http://romm.lan:8080" : "";
  engine->settings.sync.enabled = console.enabled;
  engine->snapshot = ipc::EngineSnapshot{};
  engine->snapshot.auth = console.auth;
  engine->snapshot.online = true;
  engine->snapshot.sync_in_progress = console.sync_in_progress;
}

/// The `Status` the overlay would have received, produced by the sysmodule's
/// own `GetStatus` rather than hand-built: a screen never sees a status that
/// did not come off that command, and a field this suite set by hand could be
/// one `ServiceCore` never sets.
ipc::Status StatusOf(const Console& console) {
  KnobEngine engine;
  Load(&engine, console);
  ipc::ServiceCore core(engine);
  return core.GetStatus();
}

overlay::SyncActionsView ViewOf(const Console& console, const overlay::LastCommand& last = {}) {
  return overlay::RenderSyncActions(StatusOf(console), last);
}

std::string Describe(const Console& console) {
  return std::string(console.configured ? "configured" : "unconfigured") + "/" +
         ipc::ToString(console.auth) + "/" + (console.enabled ? "on" : "off") + "/" +
         (console.sync_in_progress ? "syncing" : "idle");
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// Everything a view would draw, joined. What a "the screen never shows X"
/// assertion is made against, and what two views are compared as.
std::string AllText(const overlay::SyncActionsView& view) {
  std::string text = view.headline + "\n" + view.hint + "\n" + view.notice + "\n" +
                     view.toggle.label + "\n" + view.toggle.refusal + "\n" +
                     view.sync_now.label + "\n" + view.sync_now.refusal + "\n" +
                     overlay::ToString(view.toggle.state) + "\n" +
                     overlay::ToString(view.sync_now.state) + "\n" +
                     (view.enabled ? "on" : "off");
  for (const overlay::Line& line : view.lines) {
    text += "\n" + line.label + "=" + line.value;
  }
  return text;
}

/// The invariants every view holds, in every state. A control with no label, or
/// a refusal with no sentence, is the failure `ControlState` exists to prevent:
/// a button that visibly does nothing and says nothing about why.
void ExpectWellFormed(Checks& checks, const overlay::SyncActionsView& view,
                      const std::string& what) {
  checks.Expect(!view.headline.empty(), what + ": the headline is never empty");
  const overlay::Button* buttons[] = {&view.toggle, &view.sync_now};
  for (const overlay::Button* button : buttons) {
    checks.Expect(!button->label.empty(), what + ": every control carries a label");
    checks.ExpectEq(button->refusal.empty(), button->state != overlay::ControlState::kBlocked,
                    what + ": a blocked control carries its sentence and nothing else does");
  }
  for (const overlay::Line& line : view.lines) {
    checks.Expect(!line.value.empty(), what + ": the row " + line.label + " carries a value");
    checks.Expect(!line.label.empty(), what + ": every row carries a label");
  }
  checks.Expect(!view.notice.empty() || view.notice_tone == overlay::Tone::kNeutral,
                what + ": a tone without a sentence is a colour nothing is drawn in");
}

// --- the prediction, against the sysmodule that answers it -------------------

/// `PredictSyncNow` and `ServiceCore::SyncNow` agree on every console, and
/// "Sync now" is live exactly when the command would be accepted.
///
/// This is the acceptance criterion with teeth. The screen greys the button by
/// predicting the answer, so a prediction one branch out of step with the
/// sysmodule is either a control that refuses a sync the console would have
/// run, or one that sends a command whose refusal the screen has no line for.
void CheckPredictionMatchesTheSysmodule(Checks& checks) {
  const ipc::AuthState kAuth[] = {ipc::AuthState::kNeverPaired,
                                  ipc::AuthState::kUnauthenticated, ipc::AuthState::kPaired};
  for (bool configured : {false, true}) {
    for (ipc::AuthState auth : kAuth) {
      for (bool enabled : {false, true}) {
        for (bool in_progress : {false, true}) {
          const Console console{configured, auth, enabled, in_progress};
          const std::string what = Describe(console);

          KnobEngine engine;
          Load(&engine, console);
          ipc::ServiceCore core(engine);
          const ipc::Status status = core.GetStatus();
          const ipc::SyncOutcome actual = core.SyncNow();
          const ipc::SyncOutcome predicted = overlay::PredictSyncNow(status);
          checks.ExpectEq(std::string(ipc::ToString(predicted)), ipc::ToString(actual),
                          what + ": the screen predicts what the sysmodule answers");

          const overlay::SyncActionsView view = overlay::RenderSyncActions(status);
          ExpectWellFormed(checks, view, what);
          checks.ExpectEq(view.sync_now.state == overlay::ControlState::kLive,
                          actual == ipc::SyncOutcome::kAccepted,
                          what + ": Sync now is live exactly when the command is accepted");
          if (view.sync_now.state == overlay::ControlState::kBlocked) {
            checks.ExpectEq(view.sync_now.refusal, overlay::SyncOutcomeText(actual),
                            what + ": a refused press says what the sysmodule would have");
          }
        }
      }
    }
  }
}

/// The screen refuses a press it knows the answer to instead of sending it.
///
/// `sync_requests` counts what reached the engine. A console mid-tick draws
/// `already_running` off `Status` alone, and the only way that can be true
/// while the count is still zero is that nothing was sent.
void CheckMidSyncSendsNothing(Checks& checks) {
  Console console = Working();
  console.sync_in_progress = true;

  KnobEngine engine;
  Load(&engine, console);
  ipc::ServiceCore core(engine);
  const overlay::SyncActionsView view = overlay::RenderSyncActions(core.GetStatus());

  checks.ExpectEq(std::string(overlay::ToString(view.sync_now.state)),
                  overlay::ToString(overlay::ControlState::kBlocked),
                  "a sync already running blocks the button");
  checks.ExpectEq(view.sync_now.refusal,
                  overlay::SyncOutcomeText(ipc::SyncOutcome::kAlreadyRunning),
                  "...with the already_running sentence");
  checks.ExpectEq(engine.sync_requests, 0,
                  "...and deciding that took no SyncNow at all");
  checks.Expect(Contains(AllText(view), "Syncing"), "and the screen says a sync is running");
}

// --- the states the issue names ----------------------------------------------

/// Enabled, disabled, unconfigured, never-paired, expired and mid-sync each
/// produce their own control state and their own sentence. No two of them read
/// the same: a console that is paused and one that has never paired are two
/// different things to fix, and collapsing them sends the user to the wrong
/// screen.
void CheckEveryState(Checks& checks) {
  Console unconfigured = Working();
  unconfigured.configured = false;
  Console never_paired = Working();
  never_paired.auth = ipc::AuthState::kNeverPaired;
  Console expired = Working();
  expired.auth = ipc::AuthState::kUnauthenticated;
  Console off = Working();
  off.enabled = false;
  Console syncing = Working();
  syncing.sync_in_progress = true;

  const Console kConsoles[] = {Working(), off, unconfigured, never_paired, expired, syncing};
  std::vector<std::string> headlines;
  for (const Console& console : kConsoles) {
    const overlay::SyncActionsView view = ViewOf(console);
    ExpectWellFormed(checks, view, Describe(console));
    // The switch is live on every console the sysmodule answers on: a boolean
    // is always a legal `[sync] enabled`, so greying it would withhold a write
    // that takes -- including on an unconfigured console, where turning
    // auto-sync on ahead of writing a `server.url` is a thing a user may do.
    checks.ExpectEq(std::string(overlay::ToString(view.toggle.state)),
                    overlay::ToString(overlay::ControlState::kLive),
                    Describe(console) + ": the switch is live");
    checks.ExpectEq(view.enabled, console.enabled,
                    Describe(console) + ": the switch is drawn from the reported state");
    headlines.push_back(view.headline);
  }

  for (std::size_t i = 0; i < headlines.size(); ++i) {
    for (std::size_t j = i + 1; j < headlines.size(); ++j) {
      checks.Expect(headlines[i] != headlines[j],
                    "no two consoles read the same: \"" + headlines[i] + "\"");
    }
  }

  // The one distinction #24 is written around: a paused sysmodule and a
  // sysmodule that is not running are never the same sentence.
  const overlay::SyncActionsView paused = ViewOf(off);
  const overlay::SyncActionsView gone =
      overlay::RenderSyncActionsUnreachable(overlay::Link::kNotRunning);
  checks.Expect(paused.headline != gone.headline,
                "a paused sysmodule does not read as one that is not running");
  checks.Expect(Contains(AllText(gone), "not running"),
                "and the missing one says so in as many words");

  // Unconfigured names the setting, rather than the switch the user has already
  // set: each answer is the *first* thing standing between this console and a
  // sync.
  checks.Expect(Contains(AllText(ViewOf(unconfigured)), "server.url"),
                "an unconfigured console is told which setting is missing");
  // Both send the user to the pairing flow, and they are still two different
  // sentences: a token the server stopped accepting is not a console that has
  // never been paired (`ipc::AuthState`).
  checks.Expect(Contains(AllText(ViewOf(never_paired)), "air"),
                "a never-paired console is sent to the pairing flow");
  checks.Expect(Contains(AllText(ViewOf(expired)), "air"),
                "...and so is one whose sign-in expired");
}

/// Toggling twice is idempotent: the same reported state renders the same
/// screen, whatever it took to get back there. A view that latched anything of
/// its own would drift on the way back.
void CheckToggleIsIdempotent(Checks& checks) {
  Console on = Working();
  Console off = Working();
  off.enabled = false;

  const overlay::LastCommand applied{overlay::LastCommand::Kind::kSetEnabled,
                                     ipc::WriteOutcome::kApplied, ipc::SyncOutcome::kAccepted};
  const overlay::SyncActionsView first = ViewOf(on, applied);
  const overlay::SyncActionsView middle = ViewOf(off, applied);
  const overlay::SyncActionsView again = ViewOf(on, applied);

  checks.ExpectEq(AllText(first), AllText(again), "on -> off -> on lands on the same screen");
  checks.Expect(AllText(first) != AllText(middle), "and the trip through off was visible");
}

/// The switch is drawn from what took, and a write that did not take says so.
///
/// `kInvalid` is drawn too, and is not treated as unreachable: a boolean is
/// always a legal `[sync] enabled`, but a `config.ini` the sysmodule cannot
/// read answers `write_failed` with a diagnostic and both leave the switch
/// where it was (M5-3, #30).
void CheckWriteThatDidNotTake(Checks& checks) {
  Console off = Working();
  off.enabled = false;

  for (ipc::WriteOutcome outcome : {ipc::WriteOutcome::kInvalid, ipc::WriteOutcome::kWriteFailed}) {
    const overlay::LastCommand last{overlay::LastCommand::Kind::kSetEnabled, outcome,
                                    ipc::SyncOutcome::kAccepted};
    // The user asked for "on" and the sysmodule still reports "off".
    const overlay::SyncActionsView view = ViewOf(off, last);
    ExpectWellFormed(checks, view, std::string("set_enabled ") + ipc::ToString(outcome));
    checks.Expect(!view.notice.empty(),
                  std::string("a ") + ipc::ToString(outcome) + " write is a sentence on screen");
    checks.Expect(!view.enabled, "...and the switch stays where the sysmodule says it is");
  }

  const overlay::LastCommand applied{overlay::LastCommand::Kind::kSetEnabled,
                                     ipc::WriteOutcome::kApplied, ipc::SyncOutcome::kAccepted};
  checks.Expect(ViewOf(off, applied).notice.empty(),
                "a write that took needs no notice: the switch having moved is the answer");
}

/// Every `SyncNow` outcome is a sentence, and no two of them are the same one.
/// `kAccepted` gets one too -- the command returns before `Status` reports the
/// tick, so a press with nothing on screen for two frames is a press a user
/// repeats.
void CheckEveryOutcomeIsDrawn(Checks& checks) {
  const ipc::SyncOutcome kOutcomes[] = {
      ipc::SyncOutcome::kAccepted,      ipc::SyncOutcome::kAlreadyRunning,
      ipc::SyncOutcome::kNotConfigured, ipc::SyncOutcome::kUnauthenticated,
      ipc::SyncOutcome::kDisabled,
  };
  std::vector<std::string> sentences;
  for (ipc::SyncOutcome outcome : kOutcomes) {
    const std::string text = overlay::SyncOutcomeText(outcome);
    checks.Expect(!text.empty(), std::string("outcome ") + ipc::ToString(outcome) + " has a sentence");
    const overlay::LastCommand last{overlay::LastCommand::Kind::kSyncNow,
                                    ipc::WriteOutcome::kApplied, outcome};
    const overlay::SyncActionsView view = ViewOf(Working(), last);
    ExpectWellFormed(checks, view, std::string("sync_now ") + ipc::ToString(outcome));
    checks.ExpectEq(view.notice, text, std::string("...and pressing it draws that sentence"));
    sentences.push_back(text);
  }
  for (std::size_t i = 0; i < sentences.size(); ++i) {
    for (std::size_t j = i + 1; j < sentences.size(); ++j) {
      checks.Expect(sentences[i] != sentences[j],
                    "no two refusals read the same: \"" + sentences[i] + "\"");
    }
  }
}

/// A sysmodule that could not be reached is a screen of its own: both controls
/// inert, no rows, and the status screen's wording, because it is the same
/// diagnosis.
void CheckUnreachable(Checks& checks) {
  const overlay::Link kLinks[] = {overlay::Link::kNotRunning, overlay::Link::kUnreadable,
                                  overlay::Link::kIncompatible};
  for (overlay::Link link : kLinks) {
    const overlay::SyncActionsView view = overlay::RenderSyncActionsUnreachable(link, 3);
    const std::string what = std::string("link ") + overlay::ToString(link);
    ExpectWellFormed(checks, view, what);
    checks.ExpectEq(std::string(overlay::ToString(view.link)), overlay::ToString(link),
                    what + ": the link");
    checks.ExpectEq(std::string(overlay::ToString(view.toggle.state)),
                    overlay::ToString(overlay::ControlState::kInert),
                    what + ": the switch is inert");
    checks.ExpectEq(std::string(overlay::ToString(view.sync_now.state)),
                    overlay::ToString(overlay::ControlState::kInert),
                    what + ": Sync now is inert");
    checks.Expect(view.lines.empty(), what + ": and no row is a number this overlay does not have");
    checks.ExpectEq(view.headline, overlay::RenderUnreachable(link, 3).headline,
                    what + ": the same diagnosis reads the same as on the status screen");
  }
  checks.Expect(Contains(overlay::RenderSyncActionsUnreachable(overlay::Link::kIncompatible, 3).hint,
                         "3"),
                "a version mismatch names the two numbers");
}

// --- what the code may not do ------------------------------------------------

/// Nothing under `overlay/` writes `config.ini`, and nothing there names a boot
/// flag.
///
/// The sysmodule owns every write (docs/ARCHITECTURE.md); the overlay asks.
/// And the boot flag under `/atmosphere/contents/<TID>/flags/` is a *different*
/// off from this switch -- it is ovl-sysmodules' and M6-2's (#33), and an
/// overlay that touched it here would be turning a runtime pause into a
/// sysmodule that is not running.
///
/// A grep rather than a review, because the failure is one line added to one
/// screen in a year's time. Comment lines are skipped: this file's own
/// neighbours explain the rule in prose, and explaining it is not breaking it.
void CheckOverlayWritesNothing(Checks& checks) {
  // The write path, and the flag. `SetConfig` and `SetEnabled` are absent on
  // purpose -- those are the *commands* by which the overlay asks the sysmodule
  // to write, which is exactly the arrangement this test defends.
  static constexpr const char* kForbidden[] = {
      "config.ini",   "ofstream",   "fopen",  "fwrite",   "AtomicWrite",
      "atomic_file",  "ApplyEdit",  "boot2",  "flags/",   "atmosphere/contents",
      "SetSyncEnabled",
  };

  int scanned = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(ROMMSYNC_OVERLAY_SOURCE_DIR)) {
    const std::filesystem::path path = entry.path();
    const std::string extension = path.extension().string();
    if (extension != ".cpp" && extension != ".hpp") {
      continue;
    }
    ++scanned;
    std::ifstream file(path);
    checks.Expect(file.good(), "the overlay source is readable: " + path.string());
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
      ++number;
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
        continue;
      }
      for (const char* token : kForbidden) {
        checks.Expect(!Contains(line, token),
                      path.filename().string() + ":" + std::to_string(number) + " names " +
                          token + "; the sysmodule owns every write and the boot flag is #33's");
      }
    }
  }
  checks.Expect(scanned >= 6, "the scan found the overlay's sources");
}

}  // namespace

int main() {
  Checks checks;
  CheckPredictionMatchesTheSysmodule(checks);
  CheckMidSyncSendsNothing(checks);
  CheckEveryState(checks);
  CheckToggleIsIdempotent(checks);
  CheckWriteThatDidNotTake(checks);
  CheckEveryOutcomeIsDrawn(checks);
  CheckUnreachable(checks);
  CheckOverlayWritesNothing(checks);

  if (checks.failures() > 0) {
    std::cerr << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "ok: the sync screen greys what the sysmodule would refuse, and says why\n";
  return 0;
}

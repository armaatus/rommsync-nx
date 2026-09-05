// The overlay's status screen, decided over a decoded `ipc::Status`.
//
// M4-1. The screen itself cannot be checked before the M8-1 gate -- nothing in
// this repo draws a frame -- so everything about it that is a *decision* was
// moved off the framebuffer into `rommsync::overlay::StatusView`, and this is
// what holds it. What is under test is the sentence a never-paired console
// gets, what a download with no declared length shows instead of a percentage,
// and what goes where a timestamp would go on a console that has never synced.
//
// Every case here goes through `EncodeStatus` and `DecodeStatus` rather than
// rendering a hand-built struct: the overlay never sees a `Status` that did not
// come off the wire, and a field that stopped surviving the codec would
// otherwise render fine here and wrongly on a console.
//
// No server, no console, no emulator -- pure decisions over pure data, so this
// never skips.
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace {

namespace ipc = rommsync::ipc;
namespace overlay = rommsync::overlay;

using checks::Checks;

/// Some fixed instant, so a relative time is a fact rather than a race with the
/// clock. 2025-09-05T00:00:00Z.
constexpr std::int64_t kNow = 1757030400;

/// A console with nothing wrong with it: configured, paired, switched on,
/// online, one sync behind it. Every scenario below takes this and breaks one
/// thing, so what a scenario is about is the line it changes.
ipc::Status Healthy() {
  ipc::Status status;
  status.interface = ipc::kVersion;
  status.build = "0.4.2";
  status.enabled = true;
  status.auth = ipc::AuthState::kPaired;
  status.configured = true;
  status.online = true;
  status.last_sync_at = kNow - 600;
  status.last_sync_result = ipc::SyncResult::kOk;
  status.uploaded = 2;
  status.downloaded = 3;
  return status;
}

/// Render `status` the way the overlay does: encode it, decode it, draw that.
///
/// The decode is asserted rather than assumed -- a `Status` this suite could
/// not put on the wire is a bug in the payload, not in the screen, and it must
/// not be reported as a rendering failure.
overlay::StatusView RenderOverWire(Checks& checks, const ipc::Status& status,
                                   std::int64_t now = kNow) {
  const ipc::Decoded<ipc::Status> decoded = ipc::DecodeStatus(ipc::EncodeStatus(status));
  checks.Expect(decoded.ok(), "the status survives the wire: " + decoded.error.Describe());
  if (!decoded.ok()) {
    return overlay::RenderUnreachable(overlay::Link::kUnreadable);
  }
  return overlay::Render(decoded.value, now);
}

/// The value beside `label`, or "" when the screen has no such row.
std::string ValueOf(const overlay::StatusView& view, const std::string& label) {
  for (const overlay::Line& line : view.lines) {
    if (line.label == label) {
      return line.value;
    }
  }
  return "";
}

bool HasLabel(const overlay::StatusView& view, const std::string& label) {
  for (const overlay::Line& line : view.lines) {
    if (line.label == label) {
      return true;
    }
  }
  return false;
}

/// The guarantee the whole view model exists for, asserted on every screen this
/// file renders rather than on one of them: an empty string where a value goes
/// is indistinguishable from an overlay that failed to read something.
void ExpectNothingBlank(Checks& checks, const overlay::StatusView& view,
                        const std::string& what) {
  checks.Expect(!view.headline.empty(), what + ": the headline is never empty");
  for (const overlay::Line& line : view.lines) {
    checks.Expect(!line.label.empty(), what + ": every line has a label");
    checks.Expect(!line.value.empty(),
                  what + ": '" + line.label + "' has a value, not an empty string");
  }
  if (view.progress.kind != overlay::Progress::Kind::kNone) {
    checks.Expect(!view.progress.label.empty(), what + ": a bar names its file");
    checks.Expect(!view.progress.caption.empty(), what + ": a bar carries a caption");
  }
}

// --- the states a console is actually in --------------------------------------

void CheckHealthy(Checks& checks) {
  const overlay::StatusView view = RenderOverWire(checks, Healthy());
  ExpectNothingBlank(checks, view, "healthy");
  checks.ExpectEq(std::string(overlay::ToString(view.link)), std::string("ok"),
                  "a status that decoded is a linked screen");
  checks.ExpectEq(view.headline, std::string("Up to date"), "a healthy console says so");
  checks.ExpectEq(std::string(overlay::ToString(view.tone)), std::string("good"),
                  "and says it in a good tone");
  checks.ExpectEq(ValueOf(view, "Server"), std::string("Reachable"), "the server line");
  checks.ExpectEq(ValueOf(view, "Pairing"), std::string("Paired"), "the pairing line");
  checks.ExpectEq(ValueOf(view, "Auto-sync"), std::string("On"), "the switch line");
  checks.ExpectEq(ValueOf(view, "Last sync"), std::string("10 minutes ago"),
                  "the last sync is relative");
  checks.ExpectEq(ValueOf(view, "Result"), std::string("Finished"), "how it went");
  checks.ExpectEq(ValueOf(view, "Uploaded"), std::string("2"), "uploaded");
  checks.ExpectEq(ValueOf(view, "Downloaded"), std::string("3"), "downloaded");
  checks.ExpectEq(ValueOf(view, "Build"), std::string("0.4.2"),
                  "the build a support thread starts with");
  checks.Expect(view.progress.kind == overlay::Progress::Kind::kNone,
                "an idle console draws no bar");
}

void CheckNever(Checks& checks) {
  // The screen a console draws on the first frame after its first boot:
  // nothing configured, nothing paired, nothing synced, every count zero.
  const overlay::StatusView view = RenderOverWire(checks, ipc::Status{});
  ExpectNothingBlank(checks, view, "fresh console");
  checks.ExpectEq(view.headline, std::string("No server set"),
                  "the first thing in the way is the one that is named");
  checks.Expect(!view.hint.empty(), "and it says what to do about it");
  checks.ExpectEq(ValueOf(view, "Server"), std::string("Not set"), "no server");
  checks.ExpectEq(ValueOf(view, "Pairing"), std::string("Not paired"), "never paired");
  checks.ExpectEq(ValueOf(view, "Auto-sync"), std::string("Off"), "the switch is off");
  checks.ExpectEq(ValueOf(view, "Last sync"), std::string("Never"),
                  "'Never', not an empty timestamp -- the reason this file exists");
  checks.ExpectEq(ValueOf(view, "Result"), std::string("Not yet"), "nothing has run");
  checks.ExpectEq(ValueOf(view, "Uploaded"), std::string("0"), "a zero is drawn as a zero");
  checks.ExpectEq(ValueOf(view, "Conflicts"), std::string("0"), "so is this one");
  checks.ExpectEq(ValueOf(view, "Queue"), std::string("Empty"), "an empty queue says so");
  checks.ExpectEq(ValueOf(view, "Download"), std::string("None"),
                  "and an idle worker gets a row rather than a missing one");
  checks.ExpectEq(ValueOf(view, "Build"), std::string("Unknown"),
                  "even a build the sysmodule did not report is a value");
}

void CheckAuthStates(Checks& checks) {
  ipc::Status never = Healthy();
  never.auth = ipc::AuthState::kNeverPaired;
  const overlay::StatusView first = RenderOverWire(checks, never);
  ExpectNothingBlank(checks, first, "never paired");
  checks.ExpectEq(first.headline, std::string("Not paired"), "a console that never paired");
  checks.ExpectEq(ValueOf(first, "Pairing"), std::string("Not paired"), "and its row");

  ipc::Status expired = Healthy();
  expired.auth = ipc::AuthState::kUnauthenticated;
  const overlay::StatusView second = RenderOverWire(checks, expired);
  ExpectNothingBlank(checks, second, "expired");
  // Distinct from "Not paired" on purpose: one of these consoles needs the
  // pairing flow and the other needs to be told its token stopped working
  // (`ipc::AuthState`).
  checks.ExpectEq(second.headline, std::string("Sign-in expired"),
                  "a revoked token is not an unpaired console");
  checks.ExpectEq(std::string(overlay::ToString(second.tone)), std::string("bad"),
                  "and it is worse than a warning");
}

void CheckOfflineAndOff(Checks& checks) {
  ipc::Status offline = Healthy();
  offline.online = false;
  const overlay::StatusView first = RenderOverWire(checks, offline);
  ExpectNothingBlank(checks, first, "offline");
  checks.ExpectEq(first.headline, std::string("Offline"), "an offline console says so");
  checks.ExpectEq(ValueOf(first, "Server"), std::string("Not reached"), "and its server row");

  ipc::Status off = Healthy();
  off.enabled = false;
  const overlay::StatusView second = RenderOverWire(checks, off);
  ExpectNothingBlank(checks, second, "sync off");
  checks.ExpectEq(second.headline, std::string("Sync is off"), "the switch outranks being online");
  checks.ExpectEq(ValueOf(second, "Auto-sync"), std::string("Off"), "and its row");

  // Off *and* offline: the switch is what the user can do something about, so
  // it is the one named.
  ipc::Status both = Healthy();
  both.enabled = false;
  both.online = false;
  checks.ExpectEq(RenderOverWire(checks, both).headline, std::string("Sync is off"),
                  "the actionable state wins");
}

void CheckResults(Checks& checks) {
  ipc::Status failed = Healthy();
  failed.last_sync_result = ipc::SyncResult::kFailed;
  const overlay::StatusView first = RenderOverWire(checks, failed);
  ExpectNothingBlank(checks, first, "failed");
  checks.ExpectEq(first.headline, std::string("Last sync failed"), "a failed sync");
  checks.ExpectEq(std::string(overlay::ToString(first.tone)), std::string("bad"), "reads as bad");

  // M2-7's distinction, carried to the screen: four saves uploaded and one that
  // did not is not a failed sync, and calling it one sends the user looking for
  // a problem with the four that worked (`ipc::SyncResult`).
  ipc::Status partial = Healthy();
  partial.last_sync_result = ipc::SyncResult::kPartial;
  partial.conflicts = 1;
  partial.failed = 2;
  const overlay::StatusView second = RenderOverWire(checks, partial);
  ExpectNothingBlank(checks, second, "partial");
  checks.ExpectEq(second.headline, std::string("Last sync partly finished"),
                  "a partial sync is not a failed one");
  checks.ExpectEq(ValueOf(second, "Conflicts"), std::string("1"), "conflicts are shown");
  checks.ExpectEq(ValueOf(second, "Failed"), std::string("2"), "and so are failures");

  ipc::Status never_run = Healthy();
  never_run.last_sync_at = 0;
  never_run.last_sync_result = ipc::SyncResult::kNever;
  const overlay::StatusView third = RenderOverWire(checks, never_run);
  ExpectNothingBlank(checks, third, "ready");
  checks.ExpectEq(third.headline, std::string("Ready"),
                  "a console with nothing wrong and nothing done yet is not 'up to date'");
  checks.ExpectEq(ValueOf(third, "Last sync"), std::string("Never"), "and has never synced");
}

void CheckSyncInProgress(Checks& checks) {
  ipc::Status running = Healthy();
  running.sync_in_progress = true;
  const overlay::StatusView view = RenderOverWire(checks, running);
  ExpectNothingBlank(checks, view, "syncing");
  checks.ExpectEq(view.headline, std::string("Syncing"), "a tick in flight is drawn");
  checks.ExpectEq(ValueOf(view, "Result"), std::string("Running now"),
                  "and the last run's result does not stand in for it");

  // A tick that is uploading saves has no download at all, which is why this is
  // its own field rather than something read off `DownloadSnapshot` (#23).
  checks.Expect(view.progress.kind == overlay::Progress::Kind::kNone,
                "a sync with no download in it draws no bar");

  // The switch can be turned off while a tick is already running.
  ipc::Status running_off = running;
  running_off.enabled = false;
  checks.ExpectEq(RenderOverWire(checks, running_off).headline, std::string("Syncing"),
                  "a running tick outranks a switch that was just turned off");
}

void CheckDownloads(Checks& checks) {
  ipc::Status downloading = Healthy();
  downloading.download.state = ipc::DownloadState::kDownloading;
  downloading.download.rom_id = 91;
  downloading.download.fs_name = "Some Game (USA).gba";
  downloading.download.bytes_done = 12 * 1024 * 1024;
  downloading.download.bytes_total = 48 * 1024 * 1024;
  downloading.queue_depth = 3;
  const overlay::StatusView view = RenderOverWire(checks, downloading);
  ExpectNothingBlank(checks, view, "downloading");
  checks.ExpectEq(view.headline, std::string("Downloading"), "the headline");
  checks.ExpectEq(ValueOf(view, "File"), std::string("Some Game (USA).gba"), "the file row");
  checks.ExpectEq(ValueOf(view, "Queue"), std::string("3 waiting"), "the queue depth");
  checks.Expect(view.progress.kind == overlay::Progress::Kind::kFraction,
                "a declared length gets a real bar");
  checks.ExpectEq(view.progress.permille, 250, "a quarter of the way through");
  checks.ExpectEq(view.progress.caption, std::string("12.0 MiB of 48.0 MiB"), "the caption");

  // A server that declared no length is a real answer (#22). Anything that
  // synthesised a percentage from `bytes_done` alone would be inventing one.
  ipc::Status unknown_total = downloading;
  unknown_total.download.bytes_total = 0;
  const overlay::StatusView second = RenderOverWire(checks, unknown_total);
  ExpectNothingBlank(checks, second, "no declared length");
  checks.Expect(second.progress.kind == overlay::Progress::Kind::kIndeterminate,
                "no total means an indeterminate bar");
  checks.ExpectEq(second.progress.permille, 0, "and no percentage is invented");
  checks.ExpectEq(second.progress.caption, std::string("12.0 MiB"), "only what is known");

  // Verifying is its own row rather than a bar sitting at 100%: a checksum over
  // a large rom is seconds with no bytes moving (`ipc::DownloadState`).
  ipc::Status verifying = downloading;
  verifying.download.state = ipc::DownloadState::kVerifying;
  verifying.download.bytes_done = verifying.download.bytes_total;
  const overlay::StatusView third = RenderOverWire(checks, verifying);
  ExpectNothingBlank(checks, third, "verifying");
  checks.ExpectEq(third.headline, std::string("Downloading"), "still the download screen");
  checks.ExpectEq(ValueOf(third, "Download"), std::string("Checking"),
                  "and a full bar is labelled rather than left to read as a hang");

  ipc::Status failed = downloading;
  failed.download.state = ipc::DownloadState::kFailed;
  const overlay::StatusView fourth = RenderOverWire(checks, failed);
  ExpectNothingBlank(checks, fourth, "failed download");
  checks.ExpectEq(ValueOf(fourth, "Download"), std::string("Failed"), "a failed download");

  // A rom the sysmodule had no name for still gets a row with something in it.
  ipc::Status unnamed = downloading;
  unnamed.download.fs_name.clear();
  const overlay::StatusView fifth = RenderOverWire(checks, unnamed);
  ExpectNothingBlank(checks, fifth, "unnamed download");

  // ...and a server that under-declared its length cannot drive the bar past
  // its own end.
  ipc::Status overrun = downloading;
  overrun.download.bytes_done = overrun.download.bytes_total * 2;
  const overlay::StatusView sixth = RenderOverWire(checks, overrun);
  checks.ExpectEq(sixth.progress.permille, 1000, "the bar stops at full");
}

// --- the sysmodule that is not there ------------------------------------------

void CheckUnreachable(Checks& checks) {
  const overlay::StatusView missing = overlay::RenderUnreachable(overlay::Link::kNotRunning);
  ExpectNothingBlank(checks, missing, "not running");
  checks.ExpectEq(std::string(overlay::ToString(missing.link)), std::string("not_running"),
                  "a missing service is its own state");
  checks.ExpectEq(missing.headline, std::string("sys-rommsync is not running"),
                  "and it is what a user who forgot to enable it sees");
  checks.Expect(!missing.hint.empty(), "with something to do about it");
  checks.Expect(missing.lines.empty(),
                "no rows: every one would be a number this overlay does not have");
  checks.Expect(missing.progress.kind == overlay::Progress::Kind::kNone, "and no bar");

  // The acceptance criterion this file was written for: a payload that did not
  // decode renders as "sysmodule unreachable", never as a half-parsed struct
  // whose defaulted fields read as a working console.
  const ipc::Decoded<ipc::Status> truncated =
      ipc::DecodeStatus(std::string_view(R"({"interface":1,"build":"0.4.2","enab)"));
  checks.Expect(!truncated.ok(), "a truncated status does not decode");
  const overlay::StatusView unreadable = overlay::RenderUnreachable(overlay::Link::kUnreadable);
  ExpectNothingBlank(checks, unreadable, "unreadable");
  checks.ExpectEq(unreadable.headline, std::string("sysmodule unreachable"),
                  "a decode failure is a link state, not a status");
  checks.Expect(unreadable.lines.empty(), "and it draws no half-parsed rows");
  checks.Expect(!HasLabel(unreadable, "Last sync"),
                "in particular, no timestamp row with nothing behind it");

  // A field of the wrong type is the same answer -- the decoder refuses rather
  // than defaulting (`ipc.hpp`), and the screen must not soften that.
  const ipc::Decoded<ipc::Status> wrong_type = ipc::DecodeStatus(
      std::string_view(R"({"interface":1,"build":"0.4.2","enabled":"yes"})"));
  checks.Expect(!wrong_type.ok(), "a wrong-typed field does not decode either");

  const overlay::StatusView old = overlay::RenderUnreachable(overlay::Link::kIncompatible, 99);
  ExpectNothingBlank(checks, old, "incompatible");
  checks.ExpectEq(old.headline, std::string("sysmodule unreachable"), "the same headline");
  // ...but a different hint, because there is something to do about this one and
  // command 0's frozen encoding is what makes it knowable (`ipc::Command`).
  checks.Expect(old.hint.find("Update the sysmodule") != std::string::npos,
                "and 'update the sysmodule' rather than a decode failure");
  checks.Expect(old.hint.find("99") != std::string::npos, "naming what it answered");
  checks.Expect(old.hint.find(std::to_string(ipc::kVersion)) != std::string::npos,
                "and what this overlay speaks");
}

// --- the pieces the screen is made of -----------------------------------------

void CheckFormatting(Checks& checks) {
  checks.ExpectEq(overlay::FormatRelativeTime(0, kNow), std::string("Never"),
                  "zero is never, not 1970");
  checks.ExpectEq(overlay::FormatRelativeTime(kNow - 5, kNow), std::string("Just now"),
                  "under a minute");
  checks.ExpectEq(overlay::FormatRelativeTime(kNow - 60, kNow), std::string("1 minute ago"),
                  "singular");
  checks.ExpectEq(overlay::FormatRelativeTime(kNow - 3599, kNow), std::string("59 minutes ago"),
                  "plural, and it does not round up into an hour");
  checks.ExpectEq(overlay::FormatRelativeTime(kNow - 7200, kNow), std::string("2 hours ago"),
                  "hours");
  checks.ExpectEq(overlay::FormatRelativeTime(kNow - 86400 * 3, kNow), std::string("3 days ago"),
                  "days");
  // A console that has been off the network comes back with a clock that moved.
  // "in 3 days" is not a thing a status screen may say.
  checks.ExpectEq(overlay::FormatRelativeTime(kNow + 86400, kNow), std::string("Just now"),
                  "a clock that moved backwards does not render a future");

  checks.ExpectEq(overlay::FormatBytes(0), std::string("0 B"), "zero");
  checks.ExpectEq(overlay::FormatBytes(512), std::string("512 B"), "whole bytes get no decimal");
  checks.ExpectEq(overlay::FormatBytes(1023), std::string("1023 B"), "and stay bytes to the end");
  checks.ExpectEq(overlay::FormatBytes(1024), std::string("1.0 KiB"), "the step");
  checks.ExpectEq(overlay::FormatBytes(1536), std::string("1.5 KiB"), "one decimal");
  checks.ExpectEq(overlay::FormatBytes(48LL * 1024 * 1024), std::string("48.0 MiB"), "megabytes");
  checks.ExpectEq(overlay::FormatBytes(3LL * 1024 * 1024 * 1024), std::string("3.0 GiB"),
                  "and gigabytes -- a rom is not always small");
}

}  // namespace

int main() {
  Checks checks;
  CheckHealthy(checks);
  CheckNever(checks);
  CheckAuthStates(checks);
  CheckOfflineAndOff(checks);
  CheckResults(checks);
  CheckSyncInProgress(checks);
  CheckDownloads(checks);
  CheckUnreachable(checks);
  CheckFormatting(checks);

  if (checks.failures() > 0) {
    std::cerr << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "ok: the status screen renders every state, and never a blank value\n";
  return 0;
}

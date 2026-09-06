#include "rommsync/overlay_status_view.hpp"

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "rommsync/ipc.hpp"

namespace rommsync::overlay {
namespace {

/// The units `FormatBytes` steps through. Binary, because a rom's size is what
/// the card reports and every other tool a user has open reports the same one.
constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
constexpr std::int64_t kUnitStep = 1024;

/// Rounded to one decimal, without floating point: the value in tenths of a
/// unit, then split. A `double` would render 1023.95 MiB as "1024.0 MiB", which
/// is the one number a size is not allowed to be.
std::string Tenths(std::int64_t tenths, const char* unit) {
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " " +
         std::string(unit);
}

void Add(std::vector<Line>* lines, std::string label, std::string value,
         Tone tone = Tone::kNeutral) {
  lines->push_back(Line{std::move(label), std::move(value), tone});
}

/// The count lines, which are the last sync's rather than a running total
/// (`ipc::Status`). Conflicts and failures are toned even at zero-is-fine so a
/// screen that is quiet looks quiet.
void AddCounts(std::vector<Line>* lines, const ipc::Status& status) {
  Add(lines, "Uploaded", std::to_string(status.uploaded));
  Add(lines, "Downloaded", std::to_string(status.downloaded));
  Add(lines, "Conflicts", std::to_string(status.conflicts),
      status.conflicts > 0 ? Tone::kWarn : Tone::kNeutral);
  Add(lines, "Failed", std::to_string(status.failed),
      status.failed > 0 ? Tone::kBad : Tone::kNeutral);
}

/// The download rows and the bar. Split out because `kIdle` is the common case
/// and it is the one that must add a line rather than nothing -- a screen that
/// simply omits "Download" when nothing is downloading looks like a screen that
/// failed to read it.
void AddDownload(StatusView* view, const ipc::DownloadSnapshot& download) {
  switch (download.state) {
    case ipc::DownloadState::kIdle:
      Add(&view->lines, "Download", "None");
      return;
    case ipc::DownloadState::kQueued:
      Add(&view->lines, "Download", "Waiting to start");
      break;
    case ipc::DownloadState::kDownloading:
      Add(&view->lines, "Download", "Downloading");
      break;
    case ipc::DownloadState::kVerifying:
      // Its own row rather than a bar at 100%: a checksum over a large rom is
      // seconds with no bytes moving, and an unlabelled full bar reads as a
      // hang (`ipc::DownloadState`).
      Add(&view->lines, "Download", "Checking");
      break;
    case ipc::DownloadState::kFailed:
      // The file is still named -- a user needs to know which one -- but there
      // is no bar: a track frozen wherever the transfer stopped reads as a
      // download that is still going.
      Add(&view->lines, "Download", "Failed", Tone::kBad);
      Add(&view->lines, "File",
          download.fs_name.empty() ? std::string("Unnamed file") : download.fs_name,
          Tone::kBad);
      return;
  }

  // A name the sysmodule did not have is still a row with something in it.
  const std::string name = download.fs_name.empty() ? "Unnamed file" : download.fs_name;
  Add(&view->lines, "File", name);

  view->progress.label = name;
  if (download.bytes_total > 0) {
    view->progress.kind = Progress::Kind::kFraction;
    view->progress.caption =
        FormatBytes(download.bytes_done) + " of " + FormatBytes(download.bytes_total);
    // Clamped rather than trusted: `bytes_done` is a resumed download's total
    // on the card and a server that under-declared its length would otherwise
    // drive a bar past its own end.
    const std::int64_t done =
        download.bytes_done < download.bytes_total ? download.bytes_done : download.bytes_total;
    view->progress.permille = static_cast<int>(done * 1000 / download.bytes_total);
  } else {
    view->progress.kind = Progress::Kind::kIndeterminate;
    view->progress.caption = FormatBytes(download.bytes_done);
    view->progress.permille = 0;
  }
}

/// The headline, in precedence order.
///
/// The order is the point: a console that is not configured is also not paired
/// and also offline, and telling it the third of those sends the user to the
/// wrong screen. Each branch names the *first* thing standing between this
/// console and a sync.
void SetHeadline(StatusView* view, const ipc::Status& status) {
  if (status.config_error_count > 0) {
    // Ahead of "No server set", because a `server.url` that would not parse is
    // *why* there is no server, and sending that user to write one they have
    // already written is the wrong instruction.
    view->headline = "config.ini has errors";
    view->hint = "Open Settings to see what the sysmodule could not read";
    view->tone = Tone::kBad;
    return;
  }
  if (!status.configured) {
    view->headline = "No server set";
    view->hint = "Set server.url in config.ini";
    view->tone = Tone::kWarn;
    return;
  }
  if (status.auth == ipc::AuthState::kNeverPaired) {
    view->headline = "Not paired";
    view->hint = "Pair this console from Settings";
    view->tone = Tone::kWarn;
    return;
  }
  if (status.auth == ipc::AuthState::kUnauthenticated) {
    view->headline = "Sign-in expired";
    view->hint = "Pair this console again";
    view->tone = Tone::kBad;
    return;
  }
  // A running tick outranks the switch: `SetEnabled` turns auto-sync off
  // without stopping the tick already in flight, and a screen that showed "Sync
  // is off" over a sync that is moving files is worse than one that is a few
  // seconds out of date.
  if (status.sync_in_progress) {
    view->headline = "Syncing";
    view->tone = Tone::kNeutral;
    return;
  }
  // Above the switch, for the same reason `sync_in_progress` is: turning
  // auto-sync off does not stop a transfer already in flight, and "Sync is off"
  // over a bar that is visibly moving is a screen contradicting itself.
  if (status.download.state == ipc::DownloadState::kDownloading ||
      status.download.state == ipc::DownloadState::kVerifying) {
    view->headline = "Downloading";
    view->tone = Tone::kNeutral;
    return;
  }
  if (!status.enabled) {
    view->headline = "Sync is off";
    view->hint = "Turn sync on to start";
    view->tone = Tone::kWarn;
    return;
  }
  if (!status.online) {
    view->headline = "Offline";
    view->hint = "The last attempt did not reach the server";
    view->tone = Tone::kWarn;
    return;
  }
  if (status.last_sync_result == ipc::SyncResult::kFailed) {
    view->headline = "Last sync failed";
    view->tone = Tone::kBad;
    return;
  }
  if (status.last_sync_result == ipc::SyncResult::kPartial) {
    view->headline = "Last sync partly finished";
    view->tone = Tone::kWarn;
    return;
  }
  if (status.last_sync_result == ipc::SyncResult::kNever) {
    // Reachable, paired and switched on, and nothing has run yet. Not an error
    // and not "up to date" either -- the scheduler has simply not ticked.
    view->headline = "Ready";
    view->hint = "No sync has run yet";
    view->tone = Tone::kNeutral;
    return;
  }
  view->headline = "Up to date";
  view->tone = Tone::kGood;
}

}  // namespace

const char* ToString(Link link) {
  switch (link) {
    case Link::kOk:
      return "ok";
    case Link::kNotRunning:
      return "not_running";
    case Link::kUnreadable:
      return "unreadable";
    case Link::kIncompatible:
      return "incompatible";
  }
  return "unknown";
}

const char* ToString(Tone tone) {
  switch (tone) {
    case Tone::kNeutral:
      return "neutral";
    case Tone::kGood:
      return "good";
    case Tone::kWarn:
      return "warn";
    case Tone::kBad:
      return "bad";
  }
  return "unknown";
}

std::string SyncResultText(ipc::SyncResult result) {
  switch (result) {
    case ipc::SyncResult::kNever:
      return "Not yet";
    case ipc::SyncResult::kOk:
      return "Finished";
    case ipc::SyncResult::kPartial:
      return "Partly finished";
    case ipc::SyncResult::kFailed:
      return "Failed";
  }
  return "Unknown";
}

Tone SyncResultTone(ipc::SyncResult result) {
  switch (result) {
    case ipc::SyncResult::kNever:
      return Tone::kNeutral;
    case ipc::SyncResult::kOk:
      return Tone::kGood;
    case ipc::SyncResult::kPartial:
      return Tone::kWarn;
    case ipc::SyncResult::kFailed:
      return Tone::kBad;
  }
  return Tone::kNeutral;
}

std::string FormatBytes(std::int64_t bytes) {
  if (bytes < 0) {
    // Not a size any caller should produce, and not worth a crash on a console
    // with no debugger: it renders as nothing rather than as a negative.
    return "0 B";
  }
  std::size_t unit = 0;
  std::int64_t value = bytes;
  while (value >= kUnitStep * kUnitStep && unit + 2 < std::size(kUnits)) {
    value /= kUnitStep;
    ++unit;
  }
  if (value < kUnitStep) {
    // Whole bytes get no decimal: "512 B", not "512.0 B".
    return std::to_string(value) + " " + kUnits[unit];
  }
  // Tenths of the next unit up, rounded half-up, in integer arithmetic.
  std::int64_t tenths = (value * 10 + kUnitStep / 2) / kUnitStep;
  // ...and the unit is re-checked *after* rounding, not before. 1048525 bytes
  // rounds to 1024.0 of the unit below, which is the one number a size is not
  // allowed to be -- and it is reachable by any value in the top ~0.05% of a
  // unit, so a rom of 1 GiB minus a byte would otherwise caption as
  // "1024.0 MiB of 1.0 GiB".
  std::size_t scaled = unit + 1;
  if (tenths >= kUnitStep * 10 && scaled + 1 < std::size(kUnits)) {
    tenths /= kUnitStep;
    ++scaled;
  }
  return Tenths(tenths, kUnits[scaled]);
}

std::string FormatRelativeTime(std::int64_t then_unix, std::int64_t now_unix) {
  if (then_unix <= 0) {
    return "Never";
  }
  // A clock that moved backwards is ordinary on a console that has been off the
  // network, and "in 3 days" is not a thing a status screen may say.
  const std::int64_t seconds = now_unix > then_unix ? now_unix - then_unix : 0;
  if (seconds < 60) {
    return "Just now";
  }
  struct Unit {
    std::int64_t seconds;
    const char* singular;
    const char* plural;
  };
  static constexpr Unit kSpans[] = {
      {60, "minute", "minutes"},
      {3600, "hour", "hours"},
      {86400, "day", "days"},
  };
  // Largest unit that still gives a whole number of at least one.
  const Unit* chosen = &kSpans[0];
  for (const Unit& unit : kSpans) {
    if (seconds >= unit.seconds) {
      chosen = &unit;
    }
  }
  const std::int64_t count = seconds / chosen->seconds;
  return std::to_string(count) + " " + (count == 1 ? chosen->singular : chosen->plural) + " ago";
}

StatusView Render(const ipc::Status& status, std::int64_t now_unix) {
  StatusView view;
  view.link = Link::kOk;
  SetHeadline(&view, status);

  Add(&view.lines, "Server", status.configured ? (status.online ? "Reachable" : "Not reached")
                                               : "Not set",
      status.configured ? (status.online ? Tone::kGood : Tone::kWarn) : Tone::kWarn);

  switch (status.auth) {
    case ipc::AuthState::kNeverPaired:
      Add(&view.lines, "Pairing", "Not paired", Tone::kWarn);
      break;
    case ipc::AuthState::kUnauthenticated:
      Add(&view.lines, "Pairing", "Expired", Tone::kBad);
      break;
    case ipc::AuthState::kPaired:
      Add(&view.lines, "Pairing", "Paired", Tone::kGood);
      break;
  }

  Add(&view.lines, "Auto-sync", status.enabled ? "On" : "Off",
      status.enabled ? Tone::kGood : Tone::kNeutral);
  Add(&view.lines, "Last sync", FormatRelativeTime(status.last_sync_at, now_unix));
  Add(&view.lines, "Result",
      status.sync_in_progress ? std::string("Running now")
                              : SyncResultText(status.last_sync_result),
      status.sync_in_progress ? Tone::kNeutral : SyncResultTone(status.last_sync_result));
  AddCounts(&view.lines, status);
  Add(&view.lines, "Queue",
      status.queue_depth == 0 ? std::string("Empty")
                              : std::to_string(status.queue_depth) + " waiting");
  // Only when there is something wrong. A "Config: OK" row on every screen is a
  // row a user learns to stop reading, which is the opposite of what the count
  // is for -- and the whole list is `GetConfig`'s (`ipc::Status`).
  if (status.config_error_count > 0) {
    Add(&view.lines, "Config",
        std::to_string(status.config_error_count) +
            (status.config_error_count == 1 ? " problem" : " problems"),
        Tone::kBad);
  }
  AddDownload(&view, status.download);
  // Last, because it is the line a user reads once and a support thread reads
  // first (`ipc::Status::build`).
  Add(&view.lines, "Build",
      status.build.empty() ? std::string("Unknown") : status.build);
  return view;
}

StatusView RenderUnreachable(Link link, std::uint32_t sysmodule_interface) {
  StatusView view;
  view.link = link;
  view.tone = Tone::kBad;
  switch (link) {
    case Link::kOk:
      // A caller that got here has a `Status` and should have rendered it. Not
      // an assert: a screen that draws the wrong sentence is recoverable and a
      // sysmodule-side abort taking the overlay with it is not.
      view.link = Link::kUnreadable;
      view.headline = "sysmodule unreachable";
      view.hint = "The overlay asked for a status it did not get";
      break;
    case Link::kNotRunning:
      view.headline = "sys-rommsync is not running";
      // No "and reboot": `toolbox.json` declares `requires_reboot: false`, so
      // that overlay's own button starts the process where it stands (M6-2,
      // #33, docs/INSTALL.md step 2). This is the answer with no card behind it
      // -- the overload below says which of the four states it is -- and it is
      // also `StatusScreen`'s initial `view_`, so it is what the first frame
      // draws before the first poll returns.
      view.hint = "Turn it on in ovl-sysmodules";
      break;
    case Link::kUnreadable:
      view.headline = "sysmodule unreachable";
      view.hint = "It answered something this overlay cannot read";
      break;
    case Link::kIncompatible:
      view.headline = "sysmodule unreachable";
      view.hint = "Update the sysmodule: it speaks version " +
                  std::to_string(sysmodule_interface) + ", this overlay speaks " +
                  std::to_string(ipc::kVersion);
      break;
  }
  // No rows at all. Every one of them would be a number this overlay does not
  // have, and a zero the user cannot tell from a real one is the failure the
  // whole `Link` enum exists to avoid.
  return view;
}

StatusView RenderUnreachable(Link link, const CardState& card,
                             std::uint32_t sysmodule_interface) {
  StatusView view = RenderUnreachable(link, sysmodule_interface);
  if (view.link != Link::kNotRunning) {
    // The card has nothing to add. A sysmodule that answered at all is
    // installed and running, so `exefs.nsp` and `boot2.flag` would only repeat
    // what the session already proved.
    return view;
  }

  if (!card.installed) {
    // The state the old sentence could not say. "Enable it in the sysmodule
    // list" sends a user to a list `sys-rommsync` is not in, and ovl-sysmodules
    // lists what has a `toolbox.json` beside its `exefs.nsp` -- so an install
    // that half landed looks exactly like a toggle that will not stay on.
    view.headline = "sys-rommsync is not installed";
    view.hint = "Unpack the release zip onto the root of the SD card";
    Add(&view.lines, "Installed", "No", Tone::kBad);
    return view;
  }

  Add(&view.lines, "Installed", "Yes", Tone::kGood);
  if (!card.listable) {
    // `exefs.nsp` is there and `toolbox.json` is not, which is what an upgrade
    // from a release before that file shipped leaves, and what a half-landed
    // unzip leaves. Atmosphère would load this sysmodule; ovl-sysmodules will
    // not list it, and says nothing about why. "Turn it on in ovl-sysmodules"
    // would send the user to a screen it is missing from -- the same
    // misdirection as telling a user with nothing installed to use the
    // sysmodule list.
    Add(&view.lines, "Listed by ovl-sysmodules", "No", Tone::kBad);
    view.hint = "Unpack the release zip again: toolbox.json is missing beside exefs.nsp";
    return view;
  }
  Add(&view.lines, "Start at boot", card.set_to_boot ? "On" : "Off",
      card.set_to_boot ? Tone::kGood : Tone::kWarn);
  if (card.set_to_boot) {
    // Installed, flagged, and still silent. Not a state a working console
    // reaches: either this boot predates the flag, or the process aborted at
    // start. Both are answered by starting it, and ovl-sysmodules is where
    // that button is.
    view.hint = "It is set to start at boot but is not answering -- start it in ovl-sysmodules";
  } else {
    view.hint = "Turn sys-rommsync on in ovl-sysmodules";
  }

  if (card.config_read) {
    // Deliberately labelled as the file rather than as the state: this console
    // is not syncing whatever the line says, and a bare "Sync: On" over a
    // process that does not exist is the mislabelling the four states exist to
    // prevent (#33). It is worth drawing anyway -- a user who has already
    // turned this switch off needs to know they will have to turn it back on.
    Add(&view.lines, "Sync switch in config.ini", card.sync_enabled ? "On" : "Off",
        card.sync_enabled ? Tone::kNeutral : Tone::kWarn);
  }
  return view;
}

}  // namespace rommsync::overlay

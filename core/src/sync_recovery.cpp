// The sweep a tick runs before it does anything else: what an interrupted one
// left beside a save, and the one correct answer for each.
//
// Read `sync_tick.hpp` first -- the three rules and the reasoning behind them
// are there. What is only here is the discovery: the leftovers are found by
// *listing* the directories rather than by walking the saves the scan found,
// because the case that matters most is the one where the save is missing. A
// `<save>.old` with no `<save>` beside it is the save itself, parked by an
// interrupted commit, and a sweep driven by the scan would never look at a name
// the scan could not see.
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/atomic_file.hpp"
#include "rommsync/file_system.hpp"
#include "rommsync/http.hpp"
#include "rommsync/sync_tick.hpp"

namespace rommsync::sync {
namespace {

/// How many lines one sweep may hand up.
///
/// The same bound `state::kMaxDiagnostics` draws and for the same reason: a card
/// that will not delete anything must cost a bounded amount of memory on a
/// 512 KiB heap, not one string per file (core/AGENTS.md). The counts in
/// `RecoveryReport` stay honest past it.
constexpr std::size_t kMaxWarnings = 16;

/// `.tmp`, `.old` and `.part`, taken from the functions that write them.
///
/// `TempPathFor("")` is `".tmp"`. Deriving them is not cleverness for its own
/// sake: a literal here is a second spelling inside `core/` of something the
/// writers own, and the day one of them changes, this sweep would quietly stop
/// finding what it is for.
const std::string& TempSuffix() {
  static const std::string suffix = io::TempPathFor("");
  return suffix;
}

const std::string& PreviousSuffix() {
  static const std::string suffix = io::PreviousPathFor("");
  return suffix;
}

const std::string& PartialSuffix() {
  static const std::string suffix = http::PartialPathFor("");
  return suffix;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() > suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void Warn(RecoveryReport* report, std::string line) {
  if (report->warnings.size() < kMaxWarnings) {
    report->warnings.push_back(std::move(line));
  }
}

/// Remove `path`, counting it or naming why not.
void Discard(const std::string& path, const std::string& sd_path, std::size_t* counter,
             RecoveryReport* report) {
  if (std::remove(path.c_str()) == 0 || !io::Exists(path)) {
    ++*counter;
    return;
  }
  Warn(report, "the leftover " + sd_path + " could not be removed");
}

}  // namespace

RecoveryReport RecoverStaging(fs::FileSystem& files,
                              const std::vector<std::string>& directories) {
  RecoveryReport report;
  for (const std::string& directory : directories) {
    const fs::Listing listing = files.List(directory);
    if (!listing.ok() && listing.error != fs::ListError::kMissing) {
      // A missing directory is not worth a line: a save folder a user mapped and
      // never created is a normal card (file_system.hpp), and there is by
      // definition nothing in it to recover. Anything else is, and the entries a
      // partial listing *did* return are still swept -- `kTooManyEntries` hands
      // back the first `kMaxDirectoryEntries` by name, and a leftover among them
      // is one fewer next time.
      Warn(&report, "the directory " + directory + " could not be swept whole: " +
                        listing.message);
    }
    for (const fs::Entry& entry : listing.entries) {
      if (entry.is_directory) {
        continue;
      }
      const std::string sd_path = directory + "/" + entry.name;
      const std::string path = files.Resolve(sd_path);
      if (path.empty()) {
        Warn(&report, "the leftover " + sd_path + " is not a path on this card");
        continue;
      }

      // `.tmp.part` before `.part`, and `.part` never on its own: the partial of
      // a *rom* download is a M3-3 range-resume in progress, and this sweep does
      // not get to throw away a gigabyte of one (`sync_tick.hpp`).
      if (EndsWith(entry.name, TempSuffix() + PartialSuffix())) {
        Discard(path, sd_path, &report.partials_removed, &report);
        continue;
      }
      if (EndsWith(entry.name, TempSuffix())) {
        // Complete bytes, and unverified ones. See `sync_tick.hpp`: committing
        // this would overwrite a save with a body nothing checked and no backup
        // beside it, and the digest that would settle it died with the plan.
        Discard(path, sd_path, &report.staged_removed, &report);
        continue;
      }
      if (!EndsWith(entry.name, PreviousSuffix())) {
        continue;
      }

      // The save itself, moved aside by `io::CommitStaged`. Whether it is the
      // only copy is exactly the question of whether the base name is there.
      const std::string base_sd_path =
          sd_path.substr(0, sd_path.size() - PreviousSuffix().size());
      const std::string base = files.Resolve(base_sd_path);
      if (base.empty()) {
        Warn(&report, "the leftover " + sd_path + " names no path on this card");
        continue;
      }
      if (io::Exists(base)) {
        // The commit finished and only the tidy-up did not, so these are the
        // previous bytes -- which for a save are already under `.backup/`.
        Discard(path, sd_path, &report.previous_removed, &report);
        continue;
      }
      // The destination does not exist, which is the one thing Horizon's rename
      // needs (atomic_file.hpp), so this is the same two-rename commit finished
      // in the other direction.
      if (std::rename(path.c_str(), base.c_str()) != 0) {
        Warn(&report, "the interrupted commit at " + sd_path + " could not be undone; " +
                          base_sd_path + " is still missing");
        continue;
      }
      ++report.saves_restored;
    }
  }
  return report;
}

}  // namespace rommsync::sync

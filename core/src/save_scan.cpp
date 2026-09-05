#include "rommsync/save_scan.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rommsync::scan {
namespace {

/// ASCII-only lowering, deliberately: a platform slug and a save extension are
/// ASCII, and a locale-aware `tolower` on a UTF-8 byte is how a Turkish locale
/// turns `I` into something that matches nothing.
char LowerAscii(char character) {
  return (character >= 'A' && character <= 'Z')
             ? static_cast<char>(character - 'A' + 'a')
             : character;
}

std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    out.push_back(LowerAscii(character));
  }
  return out;
}

/// The `/`-separated segments of an SD path, in order, with empties dropped.
std::vector<std::string_view> Segments(std::string_view sd_path) {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= sd_path.size()) {
    const std::size_t slash = sd_path.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? sd_path.size() : slash;
    if (end > start) {
      segments.push_back(sd_path.substr(start, end - start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  return segments;
}

/// The largest whole second a `sync::Timestamp` can actually hold.
///
/// `sync.hpp` warns that `system_clock::time_point` cannot be relied on to hold
/// `kMaxTimestampSeconds`, because its tick is implementation-defined and a
/// nanosecond one runs out in 2262 -- which is libstdc++, so it is the Switch
/// toolchain and CI, not a hypothetical. Multiplying a larger mtime into that
/// representation is signed overflow, and the values it wraps to are the
/// dangerous kind: a year-2600 mtime wraps to a *plausible recent* instant that
/// `sync::Validate` accepts and the server then arbitrates as newer than its own
/// copy. So the bound is computed from the clock rather than assumed.
constexpr std::int64_t MaxRepresentableSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(sync::Timestamp::duration::max())
      .count();
}

/// The window an mtime has to be in to mean anything: past the epoch, and
/// spellable both by `FormatTimestamp` and by the clock underneath it.
constexpr std::int64_t kLatestUsableMtime =
    sync::kMaxTimestampSeconds < MaxRepresentableSeconds() ? sync::kMaxTimestampSeconds
                                                           : MaxRepresentableSeconds();

Skip MakeSkip(SkipReason reason, std::string sd_path, std::string message) {
  Skip skip;
  skip.reason = reason;
  skip.sd_path = std::move(sd_path);
  skip.message = std::move(message);
  return skip;
}

std::string Join(std::string_view directory, std::string_view name) {
  std::string path(directory);
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(name);
  return path;
}

}  // namespace

const char* ToString(SkipReason reason) {
  switch (reason) {
    case SkipReason::kUnmatched:
      return "unmatched";
    case SkipReason::kAmbiguous:
      return "ambiguous";
    case SkipReason::kDuplicateSlot:
      return "duplicate slot";
    case SkipReason::kUnusable:
      return "unusable";
    case SkipReason::kDirectoryFailed:
      return "directory failed";
    case SkipReason::kTooManySaves:
      return "too many saves";
  }
  return "unknown";
}

std::string Skip::Describe() const {
  std::string out = ToString(reason);
  if (!sd_path.empty()) {
    out += " " + sd_path;
  }
  if (!message.empty()) {
    out += ": " + message;
  }
  return out;
}

std::string ScanResult::DescribeSkipped() const {
  if (skipped_total == 0) {
    return {};
  }
  std::string out;
  for (const Skip& skip : skipped) {
    out += skip.Describe();
    out.push_back('\n');
  }
  if (skipped_total > skipped.size()) {
    out += "and " + std::to_string(skipped_total - skipped.size()) + " more skipped\n";
  }
  return out;
}

std::string BaseName(std::string_view file_name) {
  const std::size_t dot = file_name.rfind('.');
  // `npos` is "no extension"; `0` is a dotfile, whose whole name is its name --
  // stripping there turns `.DS_Store` into nothing, which matches every rom
  // with an empty name and none with a real one.
  if (dot == std::string_view::npos || dot == 0) {
    return std::string(file_name);
  }
  return std::string(file_name.substr(0, dot));
}

std::string EmulatorFor(std::string_view sd_dir) {
  for (const std::string_view segment : Segments(sd_dir)) {
    const std::string lowered = LowerAscii(segment);
    if (lowered == "retroarch" || lowered == "tico") {
      return lowered;
    }
  }
  return {};
}

std::string SlotFor(std::string_view emulator, std::string_view file_name) {
  // The extension, lowercased, with everything that is not a plain identifier
  // character dropped. FAT32 permits a good deal in a name, and the slot is a
  // key the server stores and this client has to reproduce byte for byte on
  // every later tick -- so it is built from a narrow alphabet rather than from
  // whatever the card happened to hold.
  const std::size_t dot = file_name.rfind('.');
  std::string extension;
  if (dot != std::string_view::npos && dot > 0) {
    for (const char character : file_name.substr(dot + 1)) {
      const char lowered = LowerAscii(character);
      const bool keep = (lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9') ||
                        lowered == '-' || lowered == '_';
      if (keep) {
        extension.push_back(lowered);
      }
    }
  }
  if (extension.empty()) {
    // A save with no usable extension still needs a stable slot, and "save" is
    // stable. It cannot collide with an extension-derived one for the same
    // emulator unless the file really is called `x.save`, which pairs the two
    // as the same slot -- the duplicate is then reported rather than silently
    // overwriting through the server.
    extension = "save";
  }
  if (emulator.empty()) {
    return extension;
  }
  return LowerAscii(emulator) + "-" + extension;
}

std::map<std::string, std::string, std::less<>> PlatformHints(const config::Config& config) {
  std::map<std::string, std::string, std::less<>> hints;
  for (const auto& [slug, folders] : config.platforms) {
    for (const std::string& directory : folders.saves) {
      const auto found = hints.find(directory);
      if (found == hints.end()) {
        hints.emplace(directory, slug);
        continue;
      }
      if (found->second != slug) {
        // Two platforms, one folder: it implies neither. Emptied rather than
        // left at the first slug, because "first platform in the map wins" is
        // exactly the guess this module exists not to make.
        found->second.clear();
      }
    }
  }
  return hints;
}

sync::ClientSaveState SaveFile::ToClientSaveState(std::optional<std::string> content_hash) const {
  sync::ClientSaveState state;
  state.rom_id = rom_id;
  state.file_name = file_name;
  if (!slot.empty()) {
    state.slot = slot;
  }
  if (!emulator.empty()) {
    state.emulator = emulator;
  }
  // Never an empty string: "" and `null` are different values to the server and
  // only one of them is a value, which is why `sync::Validate` refuses the
  // former outright.
  if (content_hash.has_value() && !content_hash->empty()) {
    state.content_hash = std::move(content_hash);
  } else if (!this->content_hash.empty()) {
    state.content_hash = this->content_hash;
  }
  // Fails closed: an mtime outside the window above becomes the epoch, which
  // `sync::Validate` refuses as "an unset clock rather than an mtime". The
  // scan never reaches here with one -- it skips them by name -- but this is
  // the conversion, so the guard against overflowing the clock belongs here
  // rather than in the one caller that happens to check first.
  state.updated_at = sync::Timestamp{};
  if (modified_unix >= sync::kMinTimestampSeconds && modified_unix <= kLatestUsableMtime) {
    state.updated_at += std::chrono::seconds(modified_unix);
  }
  state.file_size_bytes = size_bytes;
  return state;
}

ScanResult ScanSaves(const config::Config& config, const roms::RomIndex& index,
                     fs::FileSystem& files, const state::Baseline& baseline) {
  ScanResult result;
  const std::map<std::string, std::string, std::less<>> hints = PlatformHints(config);

  // `(rom_id, slot)` is what the server pairs on, so two local files landing on
  // the same pair would overwrite each other *through RomM*, on alternating
  // ticks, forever. The first one in scan order keeps it and the second is
  // reported -- which is only deterministic because the directory order is the
  // config's and each directory's entries are sorted below.
  std::vector<std::pair<std::int64_t, std::string>> claimed;

  const auto record_skip = [&result](SkipReason reason, std::string sd_path, std::string message) {
    ++result.skipped_total;
    if (result.skipped.size() < kMaxSkipsReported) {
      result.skipped.push_back(MakeSkip(reason, std::move(sd_path), std::move(message)));
    }
  };

  for (const std::string& directory : config.SaveScanDirs()) {
    fs::Listing listing = files.List(directory);
    if (!listing.ok() && listing.error != fs::ListError::kMissing) {
      // A *missing* folder is a normal card -- a platform mapped to a Tico
      // directory the user never created -- so it is silent. Anything else is
      // a folder that is there and whose saves are therefore absent from this
      // tick without anyone having decided that, which has to be said out loud.
      record_skip(SkipReason::kDirectoryFailed, directory,
                  listing.message.empty() ? fs::ToString(listing.error) : listing.message);
      if (listing.entries.empty()) {
        continue;
      }
      // `kTooManyEntries` reports what it did read. Scanning it is better than
      // dropping it: the files that were read are still saves.
    }

    // `readdir` promises no order, and a scan whose duplicate-slot resolution
    // depends on the card's directory layout resolves it differently after an
    // unrelated file is deleted.
    std::sort(listing.entries.begin(), listing.entries.end(),
              [](const fs::Entry& left, const fs::Entry& right) { return left.name < right.name; });

    const std::string emulator = EmulatorFor(directory);
    const auto hint = hints.find(directory);
    const std::string_view platform_hint =
        hint == hints.end() ? std::string_view() : std::string_view(hint->second);

    for (const fs::Entry& entry : listing.entries) {
      if (entry.is_directory || entry.name.empty()) {
        continue;  // the walk is not recursive; see file_system.hpp
      }
      ++result.files_seen;
      const std::string sd_path = Join(directory, entry.name);

      if (result.saves.size() >= kMaxSaves) {
        record_skip(SkipReason::kTooManySaves, sd_path,
                    "the scan stopped at " + std::to_string(kMaxSaves) +
                        " saves; the rest of this card was not read");
        return result;
      }

      const roms::Match match = index.Find(BaseName(entry.name), platform_hint);
      if (match.outcome == roms::MatchOutcome::kAmbiguous) {
        record_skip(SkipReason::kAmbiguous, sd_path, match.reason);
        continue;
      }
      if (!match.matched()) {
        record_skip(SkipReason::kUnmatched, sd_path, match.reason);
        continue;
      }

      // Range-checked before the record is built rather than left to
      // `sync::Validate`, because the conversion into a `sync::Timestamp` is
      // what overflows and it happens first. A card that reports 2600 is a
      // clock nobody set or a directory entry nobody wrote; either way it is
      // one file's problem, said by name.
      if (entry.modified_unix < sync::kMinTimestampSeconds ||
          entry.modified_unix > kLatestUsableMtime) {
        record_skip(SkipReason::kUnusable, sd_path,
                    "its mtime of " + std::to_string(entry.modified_unix) +
                        " is not an instant this client can report; the card's clock is unset "
                        "or the directory entry is wrong");
        continue;
      }

      SaveFile save;
      save.rom_id = match.rom->id;
      save.sd_path = sd_path;
      save.file_name = entry.name;
      save.slot = SlotFor(emulator, entry.name);
      save.emulator = emulator;
      save.platform_fs_slug = match.rom->platform_fs_slug;
      save.size_bytes = entry.size_bytes;
      save.modified_unix = entry.modified_unix;

      // Checked here rather than left to the encoder: a save that cannot be
      // expressed faithfully must cost one file, not the whole request body --
      // `EncodeNegotiateRequest` stops at the first bad entry, so one unreadable
      // mtime would otherwise take the tick's every other save with it.
      if (const json::Error error = sync::Validate(save.ToClientSaveState()); !error.ok()) {
        record_skip(SkipReason::kUnusable, sd_path, error.Describe());
        continue;
      }

      const auto pair = std::make_pair(save.rom_id, save.slot);
      if (std::find(claimed.begin(), claimed.end(), pair) != claimed.end()) {
        record_skip(SkipReason::kDuplicateSlot, sd_path,
                    "rom " + std::to_string(save.rom_id) + " already has a save in slot \"" +
                        save.slot + "\" this tick; only the first is synced");
        continue;
      }
      claimed.push_back(pair);

      // Hashed last, after the record is known to be one that will be reported:
      // `ContentHashFor` reads the file when the baseline cannot answer, and
      // reading a save that is about to be skipped is the one cost of this walk
      // that is measured in card I/O rather than in string compares.
      //
      // The path is resolved by the backend, not built here. `sd_path` is
      // SD-root absolute and means nothing to `fopen` on either platform.
      const std::string real_path = files.Resolve(save.sd_path);
      const state::HashOutcome hashed =
          real_path.empty()
              ? state::HashOutcome{{}, state::HashError::kUnreadable, false,
                                   save.sd_path + ": not a path on this card"}
              : state::ContentHashFor(baseline, save.rom_id, save.ToClientSaveState().slot,
                                      real_path, save.ToClientSaveState().updated_at,
                                      save.size_bytes);
      if (hashed.ok()) {
        save.content_hash = hashed.content_hash;
      } else {
        // Reported without a digest rather than dropped. A save the card would
        // not open this once is still the user's save, and the server compares
        // it on timestamps -- less precisely, and it will plan an upload for
        // bytes it may already have, which is why this is counted.
        ++result.unhashed_total;
        if (result.unhashed.size() < kMaxSkipsReported) {
          result.unhashed.push_back(MakeSkip(SkipReason::kUnusable, save.sd_path,
                                             hashed.message.empty()
                                                 ? std::string(state::ToString(hashed.error))
                                                 : hashed.message));
        }
      }

      result.saves.push_back(std::move(save));
    }
  }
  return result;
}

}  // namespace rommsync::scan

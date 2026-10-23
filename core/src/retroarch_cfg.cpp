#include "rommsync/retroarch_cfg.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

#include "rommsync/config_text.hpp"

namespace rommsync::retroarch {
namespace {

using config::Diagnostic;
using config::Severity;
using config::text::LowerAscii;
using config::text::Redact;
using config::text::Trim;

/// A complaint about the file, naming it: these sit beside `config.ini`'s own
/// on the settings screen, and "line 12" alone would send a user to the wrong
/// file.
Diagnostic Note(Severity severity, std::string message) {
  Diagnostic note;
  note.severity = severity;
  note.message = std::move(message);
  return note;
}

/// The keys read, and what each one sets.
enum class Key {
  kNone,
  kSavefileDirectory,
  kSavestateDirectory,
  kSortSavefiles,
  kSortSavestates,
  kSortSavefilesByContent,
  kSortSavestatesByContent,
  kSavefilesInContentDir,
  kSavestatesInContentDir,
  kCloudSync,
  kCloudSyncSaves,
};

Key KeyOf(std::string_view name) {
  struct Entry {
    const char* name;
    Key key;
  };
  static constexpr Entry kKeys[] = {
      {"savefile_directory", Key::kSavefileDirectory},
      {"savestate_directory", Key::kSavestateDirectory},
      {"sort_savefiles_enable", Key::kSortSavefiles},
      {"sort_savestates_enable", Key::kSortSavestates},
      {"sort_savefiles_by_content_enable", Key::kSortSavefilesByContent},
      {"sort_savestates_by_content_enable", Key::kSortSavestatesByContent},
      {"savefiles_in_content_dir", Key::kSavefilesInContentDir},
      {"savestates_in_content_dir", Key::kSavestatesInContentDir},
      {"cloud_sync_enable", Key::kCloudSync},
      {"cloud_sync_sync_saves", Key::kCloudSyncSaves},
  };
  for (const Entry& entry : kKeys) {
    if (name == entry.name) {
      return entry.key;
    }
  }
  return Key::kNone;
}

bool* FlagFor(Settings* settings, Key key) {
  switch (key) {
    case Key::kSortSavefiles:
      return &settings->sort_savefiles;
    case Key::kSortSavestates:
      return &settings->sort_savestates;
    case Key::kSortSavefilesByContent:
      return &settings->sort_savefiles_by_content;
    case Key::kSortSavestatesByContent:
      return &settings->sort_savestates_by_content;
    case Key::kSavefilesInContentDir:
      return &settings->savefiles_in_content_dir;
    case Key::kSavestatesInContentDir:
      return &settings->savestates_in_content_dir;
    case Key::kCloudSync:
      return &settings->cloud_sync;
    case Key::kCloudSyncSaves:
      return &settings->cloud_sync_saves;
    case Key::kNone:
    case Key::kSavefileDirectory:
    case Key::kSavestateDirectory:
      break;
  }
  return nullptr;
}

/// The line-at-a-time half of `Parse` and `Load`, so the two cannot read one
/// file two ways.
class Reader {
 public:
  /// One line, without its `\n`. `overlong` says it was cut at `kMaxLineBytes`.
  void Line(std::string_view raw, bool overlong) {
    ++line_number_;
    std::string_view line = raw;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const std::string_view content = Trim(line);
    if (content.empty() || content.front() == '#') {
      return;  // a comment, or `#include`, which nothing here follows
    }
    const std::size_t equals = content.find('=');
    if (equals == std::string_view::npos) {
      Complain("line " + std::to_string(line_number_) + " is not a 'key = value' line, ignored");
      return;
    }
    const std::string_view name = Trim(content.substr(0, equals));
    const Key key = KeyOf(name);
    if (key == Key::kNone) {
      return;  // the hundreds of settings that do not decide where a save is
    }
    if (overlong) {
      Complain(std::string(name) + " on line " + std::to_string(line_number_) +
               " is longer than any value RetroArch writes; RetroArch's default is assumed");
      return;
    }

    std::string_view value = Trim(content.substr(equals + 1));
    if (!value.empty() && value.front() == '"') {
      const std::size_t close = value.find('"', 1);
      if (close == std::string_view::npos) {
        Complain(std::string(name) + " on line " + std::to_string(line_number_) +
                 " has no closing quote; RetroArch's default is assumed");
        return;
      }
      value = value.substr(1, close - 1);
    } else {
      const std::size_t end = value.find_first_of(" \t#");
      value = value.substr(0, end);
    }
    Apply(key, name, value);
  }

  void TooLarge() { too_large_ = true; }
  void Unreadable() { unreadable_ = true; }

  LoadResult Finish(bool found) {
    LoadResult result;
    result.found = found;
    if (too_large_ || unreadable_) {
      // Nothing it said is believed: a file past the bound is a corrupt region,
      // and one that stopped reading part-way may have stopped before the line
      // that turned Cloud Sync on.
      result.diagnostics.push_back(
          Note(Severity::kWarning,
               too_large_ ? "retroarch.cfg is larger than the " +
                                std::to_string(kMaxConfigBytes) +
                                " bytes it can be; RetroArch's defaults are assumed"
                          : "retroarch.cfg exists and could not be read; RetroArch's defaults "
                            "are assumed"));
      return result;
    }
    result.value = std::move(settings_);
    result.diagnostics = std::move(diagnostics_);
    if (dropped_ > 0) {
      result.diagnostics.push_back(
          Note(Severity::kWarning, "retroarch.cfg: and " + std::to_string(dropped_) +
                                       " more problems, not listed"));
    }
    return result;
  }

 private:
  void Complain(std::string message) {
    if (diagnostics_.size() >= kMaxDiagnostics) {
      ++dropped_;
      return;
    }
    diagnostics_.push_back(Note(Severity::kWarning, "retroarch.cfg: " + std::move(message)));
  }

  void Apply(Key key, std::string_view name, std::string_view value) {
    const std::string where = std::string(name) + " on line " + std::to_string(line_number_);
    if (key == Key::kSavefileDirectory || key == Key::kSavestateDirectory) {
      std::string& target = key == Key::kSavefileDirectory ? settings_.savefile_directory
                                                           : settings_.savestate_directory;
      const char* fallback = key == Key::kSavefileDirectory ? kDefaultSavefileDirectory
                                                            : kDefaultSavestateDirectory;
      if (value.empty() || value == "default") {
        target = fallback;
        return;
      }
      // RetroArch on Horizon names the card's root `/`; a hand-edited file may
      // name its device too.
      if (LowerAscii(value.substr(0, 5)) == "sdmc:") {
        value.remove_prefix(5);
      }
      std::string normalised;
      std::string why;
      if (!config::NormalizeSdPath(value, &normalised, &why)) {
        // RetroArch ignores a directory it cannot use and keeps its default,
        // so that is what it is writing to.
        Complain(where + ": '" + Redact(value) + "' " + why + "; RetroArch's default " +
                 fallback + " is assumed");
        target = fallback;
        return;
      }
      target = std::move(normalised);
      return;
    }
    bool* flag = FlagFor(&settings_, key);
    if (flag == nullptr) {
      return;
    }
    // `config_get_bool`'s four spellings, and only those: anything else leaves
    // RetroArch at its default, so it leaves this client there too.
    if (value == "true" || value == "1") {
      *flag = true;
    } else if (value == "false" || value == "0") {
      *flag = false;
    } else {
      Complain(where + ": expected \"true\" or \"false\", got '" + Redact(value) +
               "'; RetroArch's default is assumed");
      *flag = *FlagFor(&defaults_, key);
    }
  }

  Settings settings_;
  Settings defaults_;
  std::vector<Diagnostic> diagnostics_;
  std::size_t dropped_ = 0;
  int line_number_ = 0;
  bool too_large_ = false;
  bool unreadable_ = false;
};

}  // namespace

LoadResult Parse(std::string_view text) {
  Reader reader;
  if (text.size() > kMaxConfigBytes) {
    reader.TooLarge();
    return reader.Finish(true);
  }
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t newline = text.find('\n', begin);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    const std::string_view line = text.substr(begin, end - begin);
    const bool overlong = line.size() > kMaxLineBytes;
    reader.Line(overlong ? line.substr(0, kMaxLineBytes) : line, overlong);
    begin = end + 1;
  }
  return reader.Finish(true);
}

LoadResult Load(const std::string& path) {
  LoadResult missing;
  missing.diagnostics.push_back(
      Note(Severity::kNotice, "retroarch.cfg was not found; RetroArch's defaults are assumed"));
  if (path.empty()) {
    return missing;
  }
  errno = 0;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // Only ENOENT and ENOTDIR mean nothing is there, as `io::ReadBounded` says.
    if (errno == ENOENT || errno == ENOTDIR) {
      return missing;
    }
    Reader reader;
    reader.Unreadable();
    return reader.Finish(true);
  }

  Reader reader;
  std::string line;
  bool overlong = false;
  std::size_t total = 0;
  char buffer[1024];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    total += got;
    if (total > kMaxConfigBytes) {
      reader.TooLarge();
      break;
    }
    for (std::size_t i = 0; i < got; ++i) {
      if (buffer[i] == '\n') {
        reader.Line(line, overlong);
        line.clear();
        overlong = false;
      } else if (line.size() < kMaxLineBytes) {
        line.push_back(buffer[i]);
      } else {
        overlong = true;
      }
    }
  }
  if (std::ferror(file) != 0) {
    reader.Unreadable();
  }
  std::fclose(file);
  if (!line.empty()) {
    reader.Line(line, overlong);
  }
  return reader.Finish(true);
}

Folder Settings::Saves() const {
  return {savefile_directory, kDefaultSavefileDirectory, sort_savefiles,
          sort_savefiles_by_content, savefiles_in_content_dir};
}

Folder Settings::States() const {
  return {savestate_directory, kDefaultSavestateDirectory, sort_savestates,
          sort_savestates_by_content, savestates_in_content_dir};
}

std::string EffectiveDirectory(const Folder& folder, fs::FileSystem& files) {
  if (LowerAscii(folder.directory) == LowerAscii(folder.default_directory)) {
    return folder.directory;
  }
  const fs::ListError error = files.List(folder.directory).error;
  return error == fs::ListError::kMissing || error == fs::ListError::kNotADirectory
             ? std::string(folder.default_directory)
             : folder.directory;
}

bool Enabled(const config::Config& config, const Settings& settings) {
  switch (config.sources.retroarch) {
    case config::SourceMode::kOn:
      return true;
    case config::SourceMode::kOff:
      return false;
    case config::SourceMode::kAuto:
      break;
  }
  return !settings.SyncsItself();
}

std::vector<config::Diagnostic> Notes(const config::Config& config, const Settings& settings) {
  std::vector<Diagnostic> notes;
  if (config.sources.retroarch == config::SourceMode::kOff) {
    return notes;  // the user said so; there is nothing about RetroArch to read
  }
  if (settings.SyncsItself()) {
    if (config.sources.retroarch == config::SourceMode::kAuto) {
      notes.push_back(Note(Severity::kNotice,
                           "RetroArch syncs itself: Cloud Sync is on in retroarch.cfg, so "
                           "RetroArch's saves and states are left to it. [sources] retroarch = on "
                           "syncs them here as well"));
      return notes;
    }
    notes.push_back(Note(Severity::kWarning,
                         "RetroArch's Cloud Sync and this client are both syncing RetroArch's "
                         "saves ([sources] retroarch = on): two writers to one save look like "
                         "another device's changes on every sync. Turn one of them off"));
  }
  if (settings.savefiles_in_content_dir && config.sync.saves &&
      !config.sources.retroarch_content_dir) {
    notes.push_back(Note(Severity::kWarning,
                         "RetroArch keeps saves beside the roms (savefiles_in_content_dir), "
                         "which are not read unless [sources] retroarch_content_dir = on, so "
                         "RetroArch's saves are not synced"));
  }
  if (settings.savestates_in_content_dir && config.sync.states &&
      !config.sources.retroarch_content_dir) {
    notes.push_back(Note(Severity::kWarning,
                         "RetroArch keeps states beside the roms (savestates_in_content_dir), "
                         "which are not read unless [sources] retroarch_content_dir = on, so "
                         "RetroArch's states are not synced"));
  }
  // Not read, and said with the remedy (#264): the player reads this on the
  // console, so it names RetroArch's own menu entry (`intl/msg_hash_us.h`,
  // v1.22.2) rather than a retroarch.cfg key. Both sentences stay under
  // `ipc::kMaxDiagnosticTextBytes`, which would otherwise cut the remedy off.
  if (settings.sort_savefiles_by_content && config.sync.saves) {
    notes.push_back(Note(Severity::kWarning,
                         "RetroArch sorts saves by content directory "
                         "(sort_savefiles_by_content_enable), which this client does not read, "
                         "so they are not synced: in RetroArch, turn off Settings > Saving > "
                         "Sort Saves into Folders by Content Directory"));
  }
  if (settings.sort_savestates_by_content && config.sync.states) {
    notes.push_back(Note(Severity::kWarning,
                         "RetroArch sorts states by content directory "
                         "(sort_savestates_by_content_enable), which this client does not read, "
                         "so they are not synced: in RetroArch, turn off Settings > Saving > "
                         "Sort Save States into Folders by Content Directory"));
  }
  return notes;
}

}  // namespace rommsync::retroarch

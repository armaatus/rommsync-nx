// `retroarch.cfg`, read for where RetroArch keeps saves and whether it syncs
// them itself (#230).
//
// Pure apart from the files `fallback` writes into scratch, so it never skips.
// The defaults asserted here are the Switch build's, as RetroArch v1.22.2's
// source states them (`platform_switch.c`, `config.def.h`) -- literals, not
// the constants the code uses, so a constant that drifts is a red run.
//
//   parse     -- every key read, quoted or not, with the device prefix, last wins
//   fallback  -- missing, malformed and oversized files fall back to the defaults,
//                each with a diagnostic
//   notes     -- [sources] against RetroArch's Cloud Sync, and what the user is told
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/config.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/retroarch_cfg.hpp"
#include "scratch.hpp"

namespace config = rommsync::config;
namespace ipc = rommsync::ipc;
namespace retroarch = rommsync::retroarch;

namespace {

bool Mentions(const std::vector<config::Diagnostic>& diagnostics, std::string_view text,
              config::Severity severity) {
  for (const config::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == severity && diagnostic.Describe().find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool AnyError(const std::vector<config::Diagnostic>& diagnostics) {
  for (const config::Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == config::Severity::kError) {
      return true;
    }
  }
  return false;
}

std::string Described(const std::vector<config::Diagnostic>& diagnostics) {
  std::string out;
  for (const config::Diagnostic& diagnostic : diagnostics) {
    out += std::string(config::ToString(diagnostic.severity)) + ": " + diagnostic.Describe() + "\n";
  }
  return out;
}

void ExpectDefaults(checks::Checks& c, const retroarch::Settings& s, const std::string& why) {
  c.ExpectEq(s.savefile_directory, std::string("/retroarch/cores/savefiles"),
             why + ": the Switch build's save folder");
  c.ExpectEq(s.savestate_directory, std::string("/retroarch/cores/savestates"),
             why + ": the Switch build's state folder");
  c.Expect(s.sort_savefiles && s.sort_savestates, why + ": sort-by-core is on by default");
  c.Expect(!s.sort_savefiles_by_content && !s.sort_savestates_by_content,
           why + ": sort-by-content is off by default");
  c.Expect(!s.savefiles_in_content_dir && !s.savestates_in_content_dir,
           why + ": saves are not kept beside the content by default");
  c.Expect(!s.cloud_sync && s.cloud_sync_saves && !s.SyncsItself(),
           why + ": Cloud Sync is off by default");
}

// --- parse ----------------------------------------------------------------------

void Parse(checks::Checks& c) {
  // What RetroArch writes, abridged: every value quoted, keys in no useful order,
  // hundreds of lines this client does not care about.
  const retroarch::LoadResult parsed = retroarch::Parse(
      "audio_driver = \"switch_thread\"\n"
      "savefile_directory = \"sdmc:/retroarch/saves\"\n"
      "# a comment, and a blank line\n"
      "\n"
      "savestate_directory = \"/Emulation/States\"\n"
      "sort_savefiles_enable = \"false\"\n"
      "sort_savestates_enable = \"true\"\n"
      "sort_savefiles_by_content_enable = \"true\"\n"
      "sort_savestates_by_content_enable = \"false\"\n"
      "savefiles_in_content_dir = \"true\"\n"
      "savestates_in_content_dir = 1\n"
      "cloud_sync_enable = \"true\"\n"
      "cloud_sync_sync_saves = \"false\"\n"
      "video_driver = \"gl\"\r\n");
  c.Expect(parsed.diagnostics.empty(), "a well-formed file says nothing: " +
                                           Described(parsed.diagnostics));
  c.Expect(parsed.found, "a parsed text counts as found");
  const retroarch::Settings& s = parsed.value;
  c.ExpectEq(s.savefile_directory, std::string("/retroarch/saves"),
             "the sdmc: device prefix is dropped: SD paths here start at the root");
  c.ExpectEq(s.savestate_directory, std::string("/Emulation/States"),
             "a directory keeps its case");
  c.Expect(!s.sort_savefiles && s.sort_savestates, "the two sort switches are separate");
  c.Expect(s.sort_savefiles_by_content && !s.sort_savestates_by_content,
           "and so are the two by-content ones");
  c.Expect(s.savefiles_in_content_dir && s.savestates_in_content_dir,
           "an unquoted 1 is true, as config_get_bool reads it");
  c.Expect(s.cloud_sync && !s.cloud_sync_saves, "Cloud Sync's two switches are read");
  c.Expect(!s.SyncsItself(), "Cloud Sync with saves left out does not sync saves");

  // RetroArch's own spelling of "the build's default", and the last line wins.
  const retroarch::LoadResult defaults = retroarch::Parse(
      "savefile_directory = \"/elsewhere\"\n"
      "savefile_directory = \"default\"\n"
      "savestate_directory = \"\"\n"
      "cloud_sync_enable = \"false\"\n"
      "cloud_sync_enable = \"true\"\n");
  c.Expect(defaults.diagnostics.empty(), "default and empty are values, not mistakes: " +
                                             Described(defaults.diagnostics));
  c.ExpectEq(defaults.value.savefile_directory, std::string("/retroarch/cores/savefiles"),
             "\"default\" is the build's folder, and the later line wins");
  c.ExpectEq(defaults.value.savestate_directory, std::string("/retroarch/cores/savestates"),
             "an empty directory is the build's folder");
  c.Expect(defaults.value.SyncsItself(), "the last cloud_sync_enable wins");

  // A path is normalised the way the folder map's are, so the two compare.
  const retroarch::LoadResult tidy =
      retroarch::Parse("savefile_directory = \"/retroarch//saves/\"\n");
  c.ExpectEq(tidy.value.savefile_directory, std::string("/retroarch/saves"),
             "a doubled or trailing slash is normalised away");

  ExpectDefaults(c, retroarch::Parse("").value, "an empty file");
}

// --- fallback -------------------------------------------------------------------

void Fallback(checks::Checks& c) {
  const std::filesystem::path dir = std::filesystem::path(scratch::Dir()) / "retroarch_cfg";
  std::error_code ignored;
  std::filesystem::remove_all(dir, ignored);
  std::filesystem::create_directories(dir);

  // Missing: RetroArch not installed, or never started. A notice, not a warning.
  const retroarch::LoadResult missing = retroarch::Load((dir / "retroarch.cfg").string());
  ExpectDefaults(c, missing.value, "a missing file");
  c.Expect(!missing.found, "a missing file is not found");
  c.Expect(Mentions(missing.diagnostics, "retroarch.cfg", config::Severity::kNotice),
           "a missing file is said, as a notice: " + Described(missing.diagnostics));
  const retroarch::LoadResult unresolved = retroarch::Load("");
  ExpectDefaults(c, unresolved.value, "a path the card could not resolve");
  c.Expect(!unresolved.diagnostics.empty(), "and that is said too");

  // Malformed: every bad line is one setting at its default and one sentence;
  // the good lines around it still count.
  const retroarch::LoadResult malformed = retroarch::Parse(
      "sort_savefiles_enable = \"maybe\"\n"
      "savefile_directory = \"saves\"\n"
      "savestate_directory = \"/retroarch/states\n"
      "cloud_sync_enable\n"
      "\x01\x02\x03 garbage from a torn write\n"
      "savestates_in_content_dir = \"true\"\n");
  ExpectDefaults(c, retroarch::Settings{}, "the struct's own defaults");
  c.Expect(malformed.value.sort_savefiles, "a boolean that is not one keeps the default");
  c.ExpectEq(malformed.value.savefile_directory, std::string("/retroarch/cores/savefiles"),
             "a relative directory keeps the default, as RetroArch ignores one it cannot use");
  c.ExpectEq(malformed.value.savestate_directory, std::string("/retroarch/cores/savestates"),
             "an unterminated quote keeps the default rather than reading half a path");
  c.Expect(malformed.value.savestates_in_content_dir, "a good line after bad ones still counts");
  c.Expect(Mentions(malformed.diagnostics, "sort_savefiles_enable", config::Severity::kWarning),
           "the bad boolean is named: " + Described(malformed.diagnostics));
  c.Expect(Mentions(malformed.diagnostics, "savefile_directory", config::Severity::kWarning),
           "the relative directory is named");
  c.Expect(Mentions(malformed.diagnostics, "savestate_directory", config::Severity::kWarning),
           "the unterminated quote is named");
  c.Expect(Mentions(malformed.diagnostics, "line 4", config::Severity::kWarning),
           "a line with no '=' is named by its number");
  c.Expect(!AnyError(malformed.diagnostics),
           "nothing about retroarch.cfg is an error: it is not this client's configuration");

  // A corrupt card region: thousands of bad lines cost a bounded list.
  std::string flood;
  for (int i = 0; i < 5000; ++i) {
    flood += "not a line\n";
  }
  const retroarch::LoadResult flooded = retroarch::Parse(flood);
  c.Expect(flooded.diagnostics.size() <= retroarch::kMaxDiagnostics + 1,
           "five thousand bad lines are a bounded list");
  c.Expect(Mentions(flooded.diagnostics, "more", config::Severity::kWarning),
           "and the rest are counted");

  // Read from disk, streamed: the same answers as Parse, CRLF and all.
  {
    std::ofstream out(dir / "retroarch.cfg", std::ios::binary);
    out << "savefile_directory = \"/retroarch/saves\"\r\nsort_savefiles_enable = \"false\"\r\n";
    // A line longer than any RetroArch writes, setting nothing read here.
    out << "video_shader = \"" << std::string(retroarch::kMaxLineBytes * 2, 'x') << "\"\r\n";
    out << "cloud_sync_enable = \"true\"";  // no newline at the end
  }
  const retroarch::LoadResult loaded = retroarch::Load((dir / "retroarch.cfg").string());
  c.Expect(loaded.found, "a file on disk is found");
  c.Expect(loaded.diagnostics.empty(),
           "an overlong line nobody reads is ignored quietly: " + Described(loaded.diagnostics));
  c.ExpectEq(loaded.value.savefile_directory, std::string("/retroarch/saves"),
             "a streamed file reads like a parsed one");
  c.Expect(!loaded.value.sort_savefiles, "CRLF line ends are line ends");
  c.Expect(loaded.value.SyncsItself(), "and the last line needs no newline");

  // An overlong line that *does* set a key read here is said, and ignored.
  {
    std::ofstream out(dir / "retroarch.cfg", std::ios::binary);
    out << "savefile_directory = \"/" << std::string(retroarch::kMaxLineBytes, 'y') << "\"\n";
  }
  const retroarch::LoadResult overlong = retroarch::Load((dir / "retroarch.cfg").string());
  c.ExpectEq(overlong.value.savefile_directory, std::string("/retroarch/cores/savefiles"),
             "an overlong directory keeps the default");
  c.Expect(Mentions(overlong.diagnostics, "savefile_directory", config::Severity::kWarning),
           "and is named: " + Described(overlong.diagnostics));

  // Oversized: none of it is believed.
  {
    std::ofstream out(dir / "retroarch.cfg", std::ios::binary);
    out << "cloud_sync_enable = \"true\"\n";
    const std::string filler(1024, '#');
    for (std::size_t written = 0; written <= retroarch::kMaxConfigBytes; written += 1025) {
      out << filler << "\n";
    }
  }
  const retroarch::LoadResult oversized = retroarch::Load((dir / "retroarch.cfg").string());
  ExpectDefaults(c, oversized.value, "an oversized file");
  c.Expect(Mentions(oversized.diagnostics, "retroarch.cfg", config::Severity::kWarning),
           "an oversized file is a warning: " + Described(oversized.diagnostics));

  std::filesystem::remove_all(dir, ignored);
}

// --- notes ----------------------------------------------------------------------

config::Config WithSources(const char* text) {
  const config::LoadResult parsed =
      config::ParseConfig(std::string("[server]\nurl = https://romm.lan\n") + text);
  return parsed.value;
}

void Notes(checks::Checks& c) {
  retroarch::Settings syncing;
  syncing.cloud_sync = true;

  // auto, the default: RetroArch syncs itself, so this client stays out.
  const config::Config automatic = WithSources("");
  c.Expect(automatic.sources.retroarch == config::SourceMode::kAuto, "auto is the default");
  c.Expect(!automatic.sources.retroarch_content_dir, "content folders are opt-in");
  c.Expect(!retroarch::Enabled(automatic, syncing),
           "with RetroArch's Cloud Sync on, its folders are left to it");
  c.Expect(retroarch::Enabled(automatic, retroarch::Settings{}), "and read when it is off");
  const std::vector<config::Diagnostic> left = retroarch::Notes(automatic, syncing);
  c.Expect(Mentions(left, "RetroArch syncs itself", config::Severity::kNotice),
           "the overlay is told RetroArch syncs itself: " + Described(left));
  c.Expect(Mentions(left, "[sources] retroarch = on", config::Severity::kNotice),
           "and how to sync it here anyway");
  c.Expect(retroarch::Notes(automatic, retroarch::Settings{}).empty(),
           "a console with nothing to say says nothing");

  retroarch::Settings configs_only = syncing;
  configs_only.cloud_sync_saves = false;
  c.Expect(retroarch::Enabled(automatic, configs_only),
           "Cloud Sync that leaves saves out leaves them to this client");

  // on: forced back on, and the two writers are named.
  const config::Config forced = WithSources("[sources]\nretroarch = on\n");
  c.Expect(forced.sources.retroarch == config::SourceMode::kOn, "on parses");
  c.Expect(retroarch::Enabled(forced, syncing), "[sources] retroarch = on forces it back on");
  c.Expect(Mentions(retroarch::Notes(forced, syncing), "Cloud Sync", config::Severity::kWarning),
           "and two writers to one save are a warning");

  // off: nothing of RetroArch's is read, Cloud Sync or not.
  const config::Config off = WithSources("[sources]\nretroarch = off\n");
  c.Expect(!retroarch::Enabled(off, retroarch::Settings{}), "off is off");
  c.Expect(retroarch::Notes(off, syncing).empty(), "and a user who said so is not told twice");

  // Saves beside the content: read only on opt-in, and said otherwise.
  retroarch::Settings beside;
  beside.savefiles_in_content_dir = true;
  c.Expect(Mentions(retroarch::Notes(automatic, beside), "retroarch_content_dir",
                    config::Severity::kWarning),
           "saves beside the roms are not read without opting in, and the user is told how");
  const config::Config opted =
      WithSources("[sources]\nretroarch = auto\nretroarch_content_dir = yes\n");
  c.Expect(opted.sources.retroarch_content_dir, "the opt-in parses as a boolean");
  c.Expect(retroarch::Notes(opted, beside).empty(), "and once opted in there is nothing to say");
  retroarch::Settings beside_states;
  beside_states.savestates_in_content_dir = true;
  c.Expect(retroarch::Notes(automatic, beside_states).empty(),
           "states beside the content are nothing to mention while state sync is off");

  // Sort-by-content is a layout this client does not read.
  retroarch::Settings by_content;
  by_content.sort_savefiles_by_content = true;
  c.Expect(Mentions(retroarch::Notes(automatic, by_content), "sort_savefiles_by_content_enable",
                    config::Severity::kWarning),
           "a per-content-directory layout is said not to be read");

  // ...and says what to change, in RetroArch's own words (#264): the menu path
  // and the label are `intl/msg_hash_us.h` at v1.22.2, the build #230 pinned.
  // The player is reading this on the console, with RetroArch's menu one
  // button away and no retroarch.cfg in sight.
  c.Expect(Mentions(retroarch::Notes(automatic, by_content),
                    "turn off Settings > Saving > Sort Saves into Folders by Content Directory",
                    config::Severity::kWarning),
           "the saves warning names the RetroArch setting to turn off");
  retroarch::Settings states_by_content;
  states_by_content.sort_savestates_by_content = true;
  const config::Config with_states = WithSources("[sync]\nstates = true\n");
  c.Expect(Mentions(retroarch::Notes(with_states, states_by_content),
                    "turn off Settings > Saving > Sort Save States into Folders by Content "
                    "Directory",
                    config::Severity::kWarning),
           "and the states warning names its own");
  // The settings screen gets these through `GetConfig`, which cuts a message at
  // `ipc::kMaxDiagnosticTextBytes` -- and the cut would land on the remedy.
  retroarch::Settings both = by_content;
  both.sort_savestates_by_content = true;
  for (const config::Diagnostic& note : retroarch::Notes(with_states, both)) {
    c.Expect(note.message.size() <= ipc::kMaxDiagnosticTextBytes,
             "reaches the overlay uncut (" + std::to_string(note.message.size()) +
                 " bytes): " + note.message);
  }

  // A value [sources] does not know is a warning, and the default stands.
  const config::LoadResult typo =
      config::ParseConfig("[server]\nurl = https://romm.lan\n[sources]\nretroarch = sometimes\n");
  c.Expect(typo.value.sources.retroarch == config::SourceMode::kAuto,
           "an unknown mode keeps auto");
  c.Expect(Mentions(typo.diagnostics, "retroarch", config::Severity::kWarning),
           "and says so: " + typo.DescribeDiagnostics());
  const config::LoadResult unknown =
      config::ParseConfig("[server]\nurl = https://romm.lan\n[sources]\ntico = off\n");
  c.Expect(Mentions(unknown.diagnostics, "unknown key", config::Severity::kWarning),
           "a key [sources] does not have is ignored, out loud");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "parse";
  checks::Checks c;
  if (scenario == "parse") {
    Parse(c);
  } else if (scenario == "fallback") {
    Fallback(c);
  } else if (scenario == "notes") {
    Notes(c);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }
  return c.failures() == 0 ? 0 : 1;
}

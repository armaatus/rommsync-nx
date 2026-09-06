// `config::ApplyEdit` -- what the overlay's settings screen does to `config.ini`.
//
// The claim under test is narrow and it is the whole point: the one setting
// changes and **every other byte of the file is the same byte**. A user's
// comments, their blank lines, their platform ordering, a section this build has
// never heard of, a BOM a Windows editor left behind and CRLF endings all come
// through an edit untouched, because the alternative -- regenerating the file
// from the parsed struct -- deletes all of it the first time somebody moves a
// switch in the overlay.
//
// The second claim is that this path *refuses*, where the boot path may not: a
// value `LoadConfig` would clamp or drop is a setting that looks saved and is
// not, and there is a person watching the overlay who can be told instead.
//
// Pure text in and text out, so nothing here skips.
//
//   roundtrip -- an edit lands and the rest of the file is byte-identical
//   rejects   -- a refused edit changes nothing at all
//   absent    -- a missing key, and a missing section, both land where they read
//   removes   -- `remove` drops every occurrence and the default comes back
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/config.hpp"

namespace config = rommsync::config;

namespace {

/// A file with everything an edit has to preserve: a BOM, CRLF endings, a
/// comment above and a comment beside a value, a key set twice, a section this
/// build does not know, an unknown key inside one it does, and a folder map.
std::string Busy() {
  return std::string("\xEF\xBB\xBF") +
         "; rommsync, the box in the cupboard\r\n"
         "\r\n"
         "[server]\r\n"
         "url = https://romm.example.com/   ; trailing slash on purpose\r\n"
         "\r\n"
         "[sync]\r\n"
         "enabled       = yes\r\n"
         "interval_min  = 45\r\n"
         "interval_min  = 60\r\n"
         "nonsense      = 3\r\n"
         "\r\n"
         "[experimental]\r\n"
         "future = 1\r\n"
         "\r\n"
         "; the folder map\r\n"
         "[platform.snes]\r\n"
         "roms   = /tico/roms/snes\r\n"
         "saves  = /retroarch/saves, /tico/saves/snes\r\n";
}

config::Edit One(std::string section, std::string key, std::string value) {
  config::Edit edit;
  edit.assignments.push_back({std::move(section), std::move(key), std::move(value), false});
  return edit;
}

config::Edit Remove(std::string section, std::string key) {
  config::Edit edit;
  edit.assignments.push_back({std::move(section), std::move(key), "", true});
  return edit;
}

/// Byte-for-byte, and says where they part company when they are not.
void ExpectSame(checks::Checks& c, const std::string& actual, const std::string& expected,
                const std::string& what) {
  if (actual == expected) {
    return;
  }
  std::size_t at = 0;
  while (at < actual.size() && at < expected.size() && actual[at] == expected[at]) {
    ++at;
  }
  std::cerr << "  FAIL: " << what << " -- first difference at byte " << at << "\n    expected: "
            << expected.substr(at, 60) << "\n    got:      " << actual.substr(at, 60) << "\n";
  c.Expect(false, what);
}

bool Mentions(const std::vector<config::Diagnostic>& diagnostics, const std::string& fragment) {
  for (const config::Diagnostic& entry : diagnostics) {
    if (entry.Describe().find(fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string Applied(checks::Checks& c, const std::string& before, const config::Edit& edit,
                    const std::string& what) {
  std::string after;
  std::vector<config::Diagnostic> diagnostics;
  if (config::ApplyEdit(before, edit, &after, &diagnostics)) {
    return after;
  }
  config::LoadResult refusal;
  refusal.diagnostics = diagnostics;
  c.Expect(false, what + " was refused: " + refusal.DescribeDiagnostics());
  return before;
}

// --- an edit lands, and nothing else moves -----------------------------------

void Roundtrip(checks::Checks& c) {
  const std::string before = Busy();

  // The key is set twice. The *last* line is the one `ParseConfig` resolves to,
  // so it is the one that has to change -- rewriting the first would leave the
  // edit shadowed by the second and the overlay showing a value the engine is
  // not using.
  const std::string after = Applied(c, before, One("sync", "interval_min", "90"), "interval_min");
  std::string expected = before;
  const std::size_t last = expected.rfind("interval_min  = 60");
  expected.replace(last, std::string("interval_min  = 60").size(), "interval_min  = 90");
  ExpectSame(c, after, expected, "only the last interval_min line changed");
  c.ExpectEq(config::ParseConfig(after).value.sync.interval_min, 90, "and it took effect");

  // Everything the file carries that a serialiser would have thrown away.
  c.Expect(after.rfind("\xEF\xBB\xBF", 0) == 0, "the BOM survived");
  c.Expect(after.find("[experimental]\r\nfuture = 1") != std::string::npos,
           "a section this build does not know survived, contents and all");
  c.Expect(after.find("nonsense      = 3") != std::string::npos,
           "an unknown key inside a known section survived");
  c.Expect(after.find("; rommsync, the box in the cupboard") != std::string::npos,
           "and the comments survived");
  std::size_t bare = 0;
  for (std::size_t i = 0; i < after.size(); ++i) {
    if (after[i] == '\n' && (i == 0 || after[i - 1] != '\r')) {
      ++bare;
    }
  }
  c.ExpectEq(bare, std::size_t{0}, "every line ending is still CRLF");

  // A value is normalised on the way in, so the card holds what the engine
  // loads: the trailing slash goes, and the comment beside it does not.
  const std::string moved =
      Applied(c, before, One("server", "url", "https://romm.example.com:8443/romm/"),
              "server.url");
  c.Expect(moved.find("url = https://romm.example.com:8443/romm   ; trailing slash on purpose") !=
               std::string::npos,
           "the URL is stored normalised and its trailing comment is kept");
  c.ExpectEq(config::ParseConfig(moved).value.server.url,
             std::string("https://romm.example.com:8443/romm"), "and it reads back that way");

  // A boolean written in any of its spellings is stored in one of them.
  const std::string flagged = Applied(c, before, One("SYNC", "Enabled", "OFF"), "sync.enabled");
  c.Expect(flagged.find("enabled       = false") != std::string::npos,
           "the section and key are case-insensitive and the alignment is kept");
  c.Expect(!config::ParseConfig(flagged).value.sync.enabled, "and the switch moved");

  // A folder list is normalised, deduplicated and re-joined.
  const std::string mapped =
      Applied(c, before,
              One("platform.snes", "roms", " /tico//roms/snes/ , /roms/snes , /roms/snes "),
              "platform.snes roms");
  c.Expect(mapped.find("roms   = /tico/roms/snes, /roms/snes\r\n") != std::string::npos,
           "the paths are normalised, deduplicated, and the indentation is kept");

  // A whole edit at once, since that is what a settings screen sends.
  config::Edit several;
  several.assignments.push_back({"sync", "interval_min", "15", false});
  several.assignments.push_back({"downloads", "verify_hash", "no", false});
  const std::string batch = Applied(c, before, several, "a batch");
  const config::Config parsed = config::ParseConfig(batch).value;
  c.ExpectEq(parsed.sync.interval_min, 15, "both halves of one edit landed (interval)");
  c.Expect(!parsed.downloads.verify_hash, "both halves of one edit landed (verify_hash)");
}

// --- a refused edit changes nothing ------------------------------------------

void Rejects(checks::Checks& c) {
  const std::string before = Busy();

  struct Case {
    const char* what;
    config::Edit edit;
  };
  std::vector<Case> cases;
  cases.push_back({"a URL that names no host", One("server", "url", "https://:8080")});
  cases.push_back({"a URL with no scheme", One("server", "url", "romm.lan")});
  cases.push_back({"a '..' in a folder path", One("platform.snes", "roms", "/tico/../roms")});
  cases.push_back({"a relative folder path", One("platform.snes", "saves", "saves/snes")});
  cases.push_back({"a negative interval", One("sync", "interval_min", "-5")});
  cases.push_back({"an interval past the ceiling", One("sync", "interval_min", "20000")});
  cases.push_back({"an interval that is not a number", One("sync", "interval_min", "30 minutes")});
  cases.push_back({"a boolean that is not one", One("sync", "states", "maybe")});
  cases.push_back({"a section this build does not have", One("experimental", "future", "2")});
  cases.push_back({"a key this build does not have", One("sync", "nonsense", "4")});
  cases.push_back({"a folder whose name would read back as a comment",
                   One("platform.snes", "roms", "/tico/roms/great #2")});
  cases.push_back({"an empty entry in a folder list",
                   One("platform.snes", "roms", "/a, , /b")});
  cases.push_back({"an edit that changes nothing", config::Edit{}});

  config::Edit twice;
  twice.assignments.push_back({"sync", "interval_min", "10", false});
  twice.assignments.push_back({"Sync", "INTERVAL_MIN", "20", false});
  cases.push_back({"the same key set twice in one edit", twice});

  config::Edit half_bad;
  half_bad.assignments.push_back({"sync", "interval_min", "10", false});
  half_bad.assignments.push_back({"server", "url", "ftp://romm.lan", false});
  cases.push_back({"a batch with one bad value in it", half_bad});

  config::Edit crowded;
  for (std::size_t i = 0; i <= config::kMaxEditAssignments; ++i) {
    crowded.assignments.push_back({"platform.p" + std::to_string(i), "roms", "/roms", false});
  }
  cases.push_back({"more assignments than one edit may carry", crowded});

  for (const Case& one : cases) {
    std::string after = "untouched";
    std::vector<config::Diagnostic> diagnostics;
    c.Expect(!config::ApplyEdit(before, one.edit, &after, &diagnostics),
             std::string("refused: ") + one.what);
    c.ExpectEq(after, std::string("untouched"),
               std::string("nothing was written for: ") + one.what);
    c.Expect(!diagnostics.empty(), std::string("and it said why: ") + one.what);
  }

  // The one rule about what a diagnostic on this path may say: never the URL.
  // A user will write a password into that field, and this sentence reaches a
  // log and the overlay (config.hpp).
  std::string after;
  std::vector<config::Diagnostic> diagnostics;
  config::ApplyEdit(before, One("server", "url", "https://me:hunter2@romm.lan"), &after,
                    &diagnostics);
  c.Expect(!diagnostics.empty(), "a URL carrying a credential is refused");
  c.Expect(!Mentions(diagnostics, "hunter2"), "and the password is not in the diagnostic");
  c.Expect(!Mentions(diagnostics, "romm.lan"), "nor is the host");
  c.Expect(!Mentions(diagnostics, "://"), "nor any of the URL at all");

  diagnostics.clear();
  config::ApplyEdit(before, One("server", "url", "https://:8080"), &after, &diagnostics);
  c.Expect(!Mentions(diagnostics, "8080"), "a rejected URL is never quoted back either");

  // A `config.ini` too large to read is not a file to rebuild from a fragment.
  const std::string enormous(config::kMaxConfigBytes + 1, 'x');
  diagnostics.clear();
  after = "untouched";
  c.Expect(!config::ApplyEdit(enormous, One("sync", "saves", "true"), &after, &diagnostics),
           "an oversized config.ini is refused rather than replaced");
  c.ExpectEq(after, std::string("untouched"), "and nothing was written");
}

// --- a key, or a whole section, that is not there yet -------------------------

void Absent(checks::Checks& c) {
  const std::string before = Busy();

  // The key is missing and its section is not: it goes after that section's last
  // assignment, which is `nonsense = 3` here -- ahead of the blank line and the
  // `; the folder map` comment, which introduce what comes next.
  const std::string added = Applied(c, before, One("sync", "states", "yes"), "sync.states");
  c.Expect(added.find("nonsense      = 3\r\nstates = true\r\n\r\n[experimental]") !=
               std::string::npos,
           "a new key lands at the end of its own section, not under the next one's comment");
  c.Expect(config::ParseConfig(added).value.sync.states, "and it reads back");

  // The section is missing too: it goes at the end, with a blank line in front.
  const std::string sectioned =
      Applied(c, before, One("downloads", "resume", "off"), "downloads.resume");
  c.Expect(sectioned.find("/tico/saves/snes\r\n\r\n[downloads]\r\nresume = false\r\n") !=
               std::string::npos,
           "a new section is appended with a blank line before it");
  c.Expect(!config::ParseConfig(sectioned).value.downloads.resume, "and it reads back");

  // A `[platform.x]` section replaces that platform's built-in mapping whole
  // (docs/CONFIG.md), so a section created to set *one* key would silently unmap
  // the other two. The change lands on what was in force instead: the built-in
  // mapping is written out with the edit applied on top, and the user is told.
  std::string platform;
  std::vector<config::Diagnostic> notes;
  c.Expect(config::ApplyEdit(before, One("platform.gba", "roms", "/roms/gba"), &platform, &notes),
           "a folder for a platform with no section of its own");
  const config::Config parsed = config::ParseConfig(platform).value;
  const config::PlatformFolders* gba = parsed.Platform("gba");
  c.Expect(gba != nullptr, "the new platform section is there");
  if (gba != nullptr) {
    c.ExpectEq(gba->roms.size(), std::size_t{1}, "with the folder that was set");
    c.ExpectEq(gba->roms.front(), std::string("/roms/gba"), "which is the one asked for");
    c.Expect(gba->saves == config::DefaultPlatforms().at("gba").saves,
             "and the save folders it already had, rather than none");
    c.Expect(gba->states == config::DefaultPlatforms().at("gba").states, "the states too");
  }
  c.Expect(!notes.empty(), "and the user is told the default mapping was written out");

  // A platform this build ships no default for has nothing to carry across, so
  // the section is exactly what was asked for.
  std::string unknown;
  notes.clear();
  c.Expect(config::ApplyEdit(before, One("platform.jaguar", "roms", "/roms/jaguar"), &unknown,
                             &notes),
           "a platform this build does not map by default");
  const config::PlatformFolders* jaguar = config::ParseConfig(unknown).value.Platform("jaguar");
  c.Expect(jaguar != nullptr && jaguar->saves.empty() && jaguar->states.empty(),
           "gets the one key it was given and nothing invented around it");

  // A console that has never been configured: no file at all.
  const std::string fresh =
      Applied(c, "", One("server", "url", "https://romm.lan"), "the first setting ever written");
  ExpectSame(c, fresh, "[server]\nurl = https://romm.lan\n", "an empty file grows one section");
  c.Expect(config::ParseConfig(fresh).value.configured(), "and the console is configured");

  // A file with no final newline does not gain one.
  const std::string ragged = "[sync]\nsaves = true";
  const std::string grown = Applied(c, ragged, One("sync", "on_boot", "no"), "sync.on_boot");
  ExpectSame(c, grown, "[sync]\nsaves = true\non_boot = false", "and still has no final newline");

  // A header the parser cannot key -- no closing bracket, or `[platform.]` with
  // no slug -- ends the section above it just as surely as a real one. Lines
  // under it configure nothing, so an edit must not find one of them and
  // rewrite it into a place the next boot ignores.
  const std::string shielded =
      "[sync]\nsaves = true\n[platform.]\nsaves = false\n[nope\nsaves = false\n";
  const std::string edited = Applied(c, shielded, One("sync", "saves", "false"), "sync.saves");
  ExpectSame(c, edited,
             "[sync]\nsaves = false\n[platform.]\nsaves = false\n[nope\nsaves = false\n",
             "only the line inside the real section changed");

  // The spacing around the `=` is the user's, including its absence.
  const std::string tight = Applied(c, "[sync]\ninterval_min=60\n",
                                    One("sync", "interval_min", "90"), "a tightly written line");
  ExpectSame(c, tight, "[sync]\ninterval_min=90\n", "no space is added where there was none");

  // The same section written twice is merged by the parser, so a new key goes
  // into the last of them -- anywhere earlier would be shadowed.
  const std::string split = "[sync]\nsaves = true\n\n[sync]\nstates = true\n";
  const std::string merged = Applied(c, split, One("sync", "on_boot", "false"), "into the last");
  ExpectSame(c, merged, "[sync]\nsaves = true\n\n[sync]\nstates = true\non_boot = false\n",
             "a repeated section takes the new key in its last instance");
}

// --- removing a key ------------------------------------------------------------

void Removes(checks::Checks& c) {
  const std::string before = Busy();

  // Both lines go. Removing only the last would hand the earlier value straight
  // back on the next boot, which is the opposite of "reset to default".
  const std::string gone = Applied(c, before, Remove("sync", "interval_min"), "interval_min");
  c.Expect(gone.find("interval_min") == std::string::npos, "every occurrence of the key is gone");
  c.ExpectEq(config::ParseConfig(gone).value.sync.interval_min, 30,
             "and the built-in default is back");
  c.Expect(gone.find("nonsense      = 3") != std::string::npos, "the rest of the section stayed");
  c.Expect(gone.find("enabled       = yes") != std::string::npos, "including the line above it");

  // Removing the server is allowed and leaves the console idle, which is a
  // state the overlay already draws -- not something to refuse.
  const std::string unset = Applied(c, before, Remove("server", "url"), "server.url");
  c.Expect(!config::ParseConfig(unset).value.configured(),
           "removing the URL leaves the console unconfigured");
  c.Expect(unset.find("[server]\r\n\r\n[sync]") != std::string::npos,
           "and the empty section is left where it was");

  // A key that is not in the file is already at its default: nothing to do, and
  // not a failure.
  std::string after;
  std::vector<config::Diagnostic> diagnostics;
  c.Expect(config::ApplyEdit(before, Remove("sync", "conflict_show"), &after, &diagnostics),
           "removing a key that is not there is not a failure");
  ExpectSame(c, after, before, "and it changes nothing");

  // A key this build does not have is still refused, `remove` or not: an
  // overlay asking to drop `[experimental] future` is asking about a setting
  // this client does not own.
  after = "untouched";
  diagnostics.clear();
  c.Expect(!config::ApplyEdit(before, Remove("experimental", "future"), &after, &diagnostics),
           "removing an unknown key is refused");
  c.ExpectEq(after, std::string("untouched"), "and writes nothing");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "roundtrip";
  checks::Checks checks;

  if (scenario == "roundtrip") {
    Roundtrip(checks);
  } else if (scenario == "rejects") {
    Rejects(checks);
  } else if (scenario == "absent") {
    Absent(checks);
  } else if (scenario == "removes") {
    Removes(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "config.write." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

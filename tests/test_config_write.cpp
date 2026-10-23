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
#include <utility>
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

/// The `Config` `text` parses to, **by value**.
///
/// Named because the shape that is not written here is
/// `ParseConfig(text).value.Platform(slug)`: the `Config` is a temporary, so the
/// pointer that comes back outlives the map it points into.
config::Config Parsed(const std::string& text) { return config::ParseConfig(text).value; }

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
  // A section is written into the file when it is not there yet, and a
  // `platform.` slug is whatever arrived over IPC -- `DecodeConfigEdit` only
  // checks it is not empty. A slug carrying a newline and a `[` would otherwise
  // be a section header of somebody else's writing.
  cases.push_back({"a slug that would forge a section header",
                   One("platform.snes]\n[sync]\nenabled = false\n;", "roms", "/roms/snes")});
  cases.push_back({"a slug with a comment marker in it",
                   One("platform.snes ; and the rest", "roms", "/roms/snes")});

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
  const config::Config parsed = Parsed(platform);
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

  // ...and which of the same edit's assignments got to the section first must
  // not decide the outcome. A `remove` and a `set` on one absent platform is the
  // case: whichever runs first creates the section, and carrying a default over
  // the *other* one would make the overlay's field order the deciding factor.
  config::Edit forwards;
  forwards.assignments.push_back({"platform.gba", "roms", "", true});
  forwards.assignments.push_back({"platform.gba", "saves", "/saves/gba", false});
  config::Edit backwards;
  backwards.assignments.push_back({"platform.gba", "saves", "/saves/gba", false});
  backwards.assignments.push_back({"platform.gba", "roms", "", true});
  const std::string one = Applied(c, before, forwards, "remove then set");
  const std::string other = Applied(c, before, backwards, "set then remove");
  ExpectSame(c, one, other, "an edit is a unit: its assignments' order changes nothing");
  const config::Config after_edit = Parsed(one);
  const config::PlatformFolders* gba_edit = after_edit.Platform("gba");
  c.Expect(gba_edit != nullptr && gba_edit->roms.empty(), "the removed folder is gone");
  c.Expect(gba_edit != nullptr && gba_edit->saves.size() == 1, "and the set one is there");

  // A platform this build ships no default for has nothing to carry across, so
  // the section is exactly what was asked for.
  std::string unknown;
  notes.clear();
  c.Expect(config::ApplyEdit(before, One("platform.jaguar", "roms", "/roms/jaguar"), &unknown,
                             &notes),
           "a platform this build does not map by default");
  const config::Config mapped_unknown = Parsed(unknown);
  const config::PlatformFolders* jaguar = mapped_unknown.Platform("jaguar");
  c.Expect(jaguar != nullptr && jaguar->saves.empty() && jaguar->states.empty(),
           "gets the one key it was given and nothing invented around it");

  // A console that has never been configured: no file at all.
  const std::string fresh =
      Applied(c, "", One("server", "url", "https://romm.lan"), "the first setting ever written");
  ExpectSame(c, fresh, "version = 1\n\n[server]\nurl = https://romm.lan\n",
             "an empty file grows its version line and one section (#205)");
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

  // Emptying a folder key is the documented way to map a platform nowhere, and
  // it has to be *possible*: `ParseConfig` drops a platform whose section maps
  // nothing at all, so the read-back check has to read an absent platform as an
  // empty list rather than as a value it could not find.
  const std::string nowhere =
      Applied(c, "[platform.snes]\nroms = /roms/snes\n", One("platform.snes", "roms", ""),
              "emptying the last folder of a platform");
  ExpectSame(c, nowhere, "[platform.snes]\nroms =\n", "the key is left with no value");
  c.Expect(config::ParseConfig(nowhere).value.Platform("snes") == nullptr,
           "and the platform maps nothing, which is what was asked for");

  config::Edit both;
  both.assignments.push_back({"platform.snes", "roms", "", false});
  both.assignments.push_back({"platform.snes", "saves", "", false});
  const std::string emptied =
      Applied(c, "[platform.snes]\nroms = /a\nsaves = /b\n", both, "emptying two keys at once");
  c.Expect(config::ParseConfig(emptied).value.Platform("snes") == nullptr,
           "a whole platform can be switched off in one edit");

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

// --- the emulator override ------------------------------------------------------

void Emulator(checks::Checks& c) {
  const std::string before = Busy();

  // Set in an existing section: one line, and the folders stay as written.
  const std::string snes =
      Applied(c, before, One("platform.snes", "emulator", "snes9x"), "platform.snes emulator");
  const config::Config snes_parsed = Parsed(snes);
  c.ExpectEq(std::string(snes_parsed.EmulatorOverride("snes")), std::string("snes9x"),
             "an override set from the overlay reads back");
  c.Expect(snes_parsed.Platform("snes") != nullptr &&
               snes_parsed.Platform("snes")->saves == Parsed(before).Platform("snes")->saves,
           "and the section's folders are untouched");

  // A section that does not exist yet gets the override and nothing else: an
  // emulator-only section keeps the built-in folders, so there is nothing to
  // carry and nothing to tell the user about.
  std::string gb;
  std::vector<config::Diagnostic> notes;
  c.Expect(config::ApplyEdit(before, One("platform.gb", "emulator", "mgba"), &gb, &notes),
           "an override for a platform with no section");
  c.Expect(gb.find("[platform.gb]\r\nemulator = mgba") != std::string::npos ||
               gb.find("[platform.gb]\nemulator = mgba") != std::string::npos,
           "is a new section with the one line in it");
  c.Expect(gb.find("/tico/roms/gb") == std::string::npos, "and no folders written out");
  c.Expect(!Mentions(notes, "written out"), "and nothing carried, so no notice");
  const config::Config gb_parsed = Parsed(gb);
  c.Expect(gb_parsed.Platform("gb") != nullptr &&
               gb_parsed.Platform("gb")->roms == config::Defaults().Platform("gb")->roms,
           "gb keeps its built-in folders");

  // ...and a folder set later in that emulator-only section must not unmap the
  // other two: the section's folders in force are the built-in ones, and the
  // edit lands on those, exactly as it does for a section that is not there.
  std::string moved;
  notes.clear();
  c.Expect(config::ApplyEdit(gb, One("platform.gb", "saves", "/tico/saves/gb"), &moved, &notes),
           "a folder in an emulator-only section");
  const config::Config moved_parsed = Parsed(moved);
  const config::PlatformFolders* moved_gb = moved_parsed.Platform("gb");
  c.Expect(moved_gb != nullptr && moved_gb->roms == config::Defaults().Platform("gb")->roms,
           "the roms folder in force is carried, not dropped");
  c.Expect(moved_gb != nullptr && moved_gb->saves == std::vector<std::string>{"/tico/saves/gb"},
           "the saves folder is the one that was set");
  c.ExpectEq(std::string(moved_parsed.EmulatorOverride("gb")), std::string("mgba"),
             "and the override is still there");
  c.Expect(Mentions(notes, "written out"), "and the user is told what was carried");

  // A section that is there and maps nothing on purpose is not given folders back.
  const std::string off = "[server]\nurl = https://romm.example.com\n[platform.psp]\n";
  const std::string psp =
      Applied(c, off, One("platform.psp", "saves", "/tico/saves/psp"), "platform.psp saves");
  const config::Config psp_parsed = Parsed(psp);
  c.Expect(psp_parsed.Platform("psp") != nullptr && psp_parsed.Platform("psp")->roms.empty(),
           "an emptied section stays empty but for what was set");

  // Refused, with nothing written, for an id RomM would refuse.
  std::string untouched = "unchanged";
  notes.clear();
  c.Expect(!config::ApplyEdit(before, One("platform.snes", "emulator", "snes/9x"), &untouched,
                              &notes),
           "an id with a slash is refused");
  c.ExpectEq(untouched, std::string("unchanged"), "and nothing was written");
  c.Expect(!notes.empty() && notes.back().key == "emulator", "the refusal names the key");

  // Reset to default removes it.
  const std::string reset =
      Applied(c, snes, Remove("platform.snes", "emulator"), "platform.snes emulator removed");
  c.ExpectEq(std::string(Parsed(reset).EmulatorOverride("snes")), std::string(),
             "removing the override puts the built-in id back");
}

// --- the M10 settings a player changes from the overlay (#264) -------------------

void Sources(checks::Checks& c) {
  const std::string before = Busy();

  // `[sync] on_server_delete` lands in the section that is there, one line.
  const std::string keep =
      Applied(c, before, One("sync", "on_server_delete", "KEEP"), "on_server_delete = keep");
  c.Expect(Parsed(keep).sync.on_server_delete == config::OnServerDelete::kKeep,
           "on_server_delete set from the overlay reads back");
  c.Expect(keep.find("on_server_delete = keep\r\n") != std::string::npos,
           "stored lowercase, the way the parser's own word is spelled");
  const std::string removed =
      Applied(c, keep, One("sync", "on_server_delete", "remove"), "on_server_delete = remove");
  c.Expect(Parsed(removed).sync.on_server_delete == config::OnServerDelete::kRemove,
           "and back to remove");

  // `[sources]` is not in the file at all: the section is appended.
  const std::string on =
      Applied(c, before, One("sources", "retroarch", "on"), "[sources] retroarch = on");
  c.Expect(Parsed(on).sources.retroarch == config::SourceMode::kOn, "retroarch = on reads back");
  c.Expect(on.find("[sources]\r\nretroarch = on") != std::string::npos,
           "in a [sources] section of its own");
  const std::string off = Applied(c, on, One("sources", "retroarch", "no"), "retroarch = off");
  c.Expect(Parsed(off).sources.retroarch == config::SourceMode::kOff,
           "a boolean spelling of off reads back as off");
  c.Expect(off.find("retroarch = off\r\n") != std::string::npos,
           "and is stored as the word the setting documents");
  const std::string automatic =
      Applied(c, off, One("sources", "retroarch", " Auto "), "retroarch = auto");
  c.Expect(Parsed(automatic).sources.retroarch == config::SourceMode::kAuto, "auto reads back");

  const std::string beside = Applied(c, automatic, One("sources", "retroarch_content_dir", "yes"),
                                     "retroarch_content_dir = true");
  c.Expect(Parsed(beside).sources.retroarch_content_dir, "retroarch_content_dir reads back");
  c.Expect(beside.find("retroarch_content_dir = true") != std::string::npos,
           "stored as true, the way every other flag is");

  // Refused, with nothing written: a word neither setting has.
  const std::pair<config::Edit, const char*> refused[] = {
      {One("sync", "on_server_delete", "delete"), "on_server_delete"},
      {One("sync", "on_server_delete", "true"), "on_server_delete"},
      {One("sources", "retroarch", "sometimes"), "retroarch"},
      {One("sources", "retroarch_content_dir", "auto"), "retroarch_content_dir"},
      {One("sources", "tico", "off"), "tico"},
  };
  for (const auto& [edit, key] : refused) {
    std::string after = "untouched";
    std::vector<config::Diagnostic> diagnostics;
    c.Expect(!config::ApplyEdit(before, edit, &after, &diagnostics),
             std::string("refused: [") + edit.assignments[0].section + "] " + key + " = " +
                 edit.assignments[0].value);
    c.ExpectEq(after, std::string("untouched"), std::string("nothing written for ") + key);
    c.Expect(!diagnostics.empty() && diagnostics.back().key == key,
             std::string("and the refusal names ") + key);
  }

  // Reset to default removes the line, and the default is back.
  const std::string reset = Applied(c, beside, Remove("sources", "retroarch_content_dir"),
                                    "retroarch_content_dir removed");
  c.Expect(!Parsed(reset).sources.retroarch_content_dir, "removing it turns it back off");
}

/// #271: the settings screen's account picker and `play_with`. `[sync] slot`
/// stays refused -- it moves every save to a new slot on the server -- and
/// `map_state_slots` is the flag the write path already took.
void TicoAndPlayWith(checks::Checks& c) {
  const std::string before = Busy();
  const std::string kUid = "0123456789abcdef0123456789abcdef";

  // `[tico]` is not in the file: the section is appended, and an account id in
  // either case is stored lowercase, the way Tico names its folder.
  const std::string named = Applied(
      c, before, One("tico", "account", " 0123456789ABCDEF0123456789abcdef "), "account = <uid>");
  c.Expect(Parsed(named).tico.account == config::TicoAccount::kNamed &&
               Parsed(named).tico.uid == kUid,
           "a named account reads back");
  c.Expect(named.find("[tico]\r\naccount = " + kUid) != std::string::npos,
           "in a [tico] section of its own, lowercase");
  const std::string automatic = Applied(c, named, One("tico", "account", "AUTO"), "account = auto");
  c.Expect(Parsed(automatic).tico.account == config::TicoAccount::kAuto, "auto reads back");
  c.Expect(automatic.find("account = auto") != std::string::npos,
           "stored as the word the setting documents");
  const std::string legacy =
      Applied(c, automatic, One("tico", "account", "legacy"), "account = legacy");
  c.Expect(Parsed(legacy).tico.account == config::TicoAccount::kLegacy, "legacy reads back");

  // `play_with` lands in the platform's section, and its folders stay.
  const std::string retro = Applied(c, before, One("platform.snes", "play_with", "RetroArch"),
                                    "[platform.snes] play_with = retroarch");
  const config::Config retro_config = Parsed(retro);
  c.Expect(retro_config.PlayWithFor("snes") == config::PlayWith::kRetroArch,
           "play_with = retroarch reads back");
  c.Expect(retro.find("play_with = retroarch\r\n") != std::string::npos, "stored lowercase");
  const config::PlatformFolders* snes = retro_config.Platform("snes");
  c.Expect(snes != nullptr && snes->roms == std::vector<std::string>{"/tico/roms/snes"},
           "and the platform's folders are the ones it had");
  const std::string tico =
      Applied(c, retro, One("platform.snes", "play_with", "tico"), "play_with = tico");
  c.Expect(Parsed(tico).PlayWithFor("snes") == config::PlayWith::kTico, "and back to tico");

  // A platform with no section yet: one line, and the built-in folders stay in
  // force -- a section that sets only `play_with` is not a folder map.
  const std::string gb = Applied(c, before, One("platform.gb", "play_with", "retroarch"),
                                 "[platform.gb] play_with");
  const config::Config gb_config = Parsed(gb);
  c.Expect(gb_config.PlayWithFor("gb") == config::PlayWith::kRetroArch, "gb plays in RetroArch");
  c.Expect(gb_config.Platform("gb") != nullptr && !gb_config.Platform("gb")->roms.empty(),
           "and keeps its built-in folders");

  // Empty is a value, the way `emulator =` is: the default is back.
  const std::string unset =
      Applied(c, retro, One("platform.snes", "play_with", ""), "play_with = (empty)");
  const config::Config unset_config = Parsed(unset);
  c.Expect(unset_config.play_with.find("snes") == unset_config.play_with.end(),
           "an empty play_with reads back as no line");
  c.Expect(unset_config.PlayWithFor("snes") == config::PlayWith::kTico,
           "and the default is in force again");

  // Refused, with nothing written.
  const std::pair<config::Edit, const char*> refused[] = {
      {One("tico", "account", "0123456789abcdef"), "account"},
      {One("tico", "account", "../../saves"), "account"},
      {One("tico", "uid", kUid), "uid"},
      {One("platform.snes", "play_with", "mgba"), "play_with"},
      {One("sync", "slot", "mine"), "slot"},
  };
  for (const auto& [edit, key] : refused) {
    std::string after = "untouched";
    std::vector<config::Diagnostic> diagnostics;
    c.Expect(!config::ApplyEdit(before, edit, &after, &diagnostics),
             std::string("refused: [") + edit.assignments[0].section + "] " + key + " = " +
                 edit.assignments[0].value);
    c.ExpectEq(after, std::string("untouched"), std::string("nothing written for ") + key);
    c.Expect(!diagnostics.empty() && diagnostics.back().key == key,
             std::string("and the refusal names ") + key);
  }

  // `map_state_slots` is the flag it already was.
  const std::string mapped =
      Applied(c, before, One("sync", "map_state_slots", "on"), "map_state_slots = true");
  c.Expect(Parsed(mapped).sync.map_state_slots, "map_state_slots reads back");
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
  } else if (scenario == "emulator") {
    Emulator(checks);
  } else if (scenario == "sources") {
    Sources(checks);
  } else if (scenario == "tico") {
    TicoAndPlayWith(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() == 0) {
    std::cout << "config.write." << scenario << " ok\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

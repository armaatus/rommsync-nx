// `config.ini`: the defaults, the overrides, and every way a file can be wrong.
//
// Pure parsing and one temp file, so this never skips -- and it must not, since
// the guarantee it exists for is that a bad config.ini still boots a working
// client. That is a claim about the *rejections*, so most of what is below is
// broken input, and the thing asserted about each is twofold: the value the
// client ends up using, and that it was told which line was wrong.
//
// The one test here that reads outside itself is `TheDocumentedExampleParses`.
// It parses the block in docs/CONFIG.md rather than a copy of it, because a
// documented example that the parser rejects is the failure a user meets first
// and the one no test of a private string literal can see.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/config.hpp"

namespace config = rommsync::config;

namespace {

/// The diagnostic about `section`/`key`, or nullptr. Diagnostics are prose, so
/// tests ask which line was complained about rather than matching the sentence.
const config::Diagnostic* Complaint(const config::LoadResult& result, const std::string& section,
                                    const std::string& key) {
  for (const config::Diagnostic& entry : result.diagnostics) {
    if (entry.section == section && entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

bool Mentions(const config::LoadResult& result, const std::string& fragment) {
  return result.DescribeDiagnostics().find(fragment) != std::string::npos;
}

std::size_t CountOf(const config::LoadResult& result, config::Severity severity) {
  std::size_t total = 0;
  for (const config::Diagnostic& entry : result.diagnostics) {
    if (entry.severity == severity) {
      ++total;
    }
  }
  return total;
}

/// A config that is valid apart from whatever the caller is testing, so a test
/// of `[sync]` is not also a test of the missing-server error.
std::string WithServer(const std::string& rest) {
  return "[server]\nurl = https://romm.example.com\n" + rest;
}

// --- defaults ----------------------------------------------------------------

void DefaultsAreTheDocumentedOnes(checks::Checks& c) {
  const config::Config defaults = config::Defaults();

  c.Expect(!defaults.configured(), "a console with no config has no server");
  c.Expect(defaults.sync.enabled, "sync is on by default");
  c.ExpectEq(defaults.sync.interval_min, 30, "the default interval");
  c.Expect(defaults.sync.on_boot, "syncing on boot is on by default");
  c.Expect(defaults.sync.saves, "saves sync by default");
  // The one default that is off, and the reason is in docs/CONFIG.md: a state
  // is a core memory snapshot and does not survive a different core.
  c.Expect(!defaults.sync.states, "states do NOT sync by default");
  c.Expect(defaults.sync.conflict_show, "conflicts are surfaced by default");
  c.Expect(defaults.downloads.enabled && defaults.downloads.verify_hash &&
               defaults.downloads.resume,
           "downloads, hash verification and resume are all on by default");

  // Every platform docs/CONFIG.md promises a built-in mapping for.
  const char* const kMapped[] = {"nes", "snes",      "gb",  "gbc", "gba", "n64",
                                 "genesis", "psx", "nds", "dreamcast", "psp"};
  for (const char* slug : kMapped) {
    const config::PlatformFolders* folders = defaults.Platform(slug);
    c.Expect(folders != nullptr, std::string("a built-in mapping for ") + slug);
    if (folders == nullptr) {
      continue;
    }
    c.Expect(!folders->roms.empty(), std::string(slug) + " has somewhere to download to");
    c.Expect(!folders->saves.empty(), std::string(slug) + " has somewhere to scan for saves");
  }

  // The heavy systems, absent on purpose: nothing on the console runs them, so
  // the client has to skip them rather than guess a folder.
  const char* const kUnmapped[] = {"ps2", "ps3", "ps4", "wii", "ngc", "3ds", "switch"};
  for (const char* slug : kUnmapped) {
    c.Expect(defaults.Platform(slug) == nullptr,
             std::string(slug) + " is deliberately unmapped");
    c.Expect(defaults.RomTarget(slug).empty(),
             std::string("...so there is no download target for ") + slug);
  }

  c.ExpectEq(defaults.RomTarget("snes"), std::string("/tico/roms/snes"),
             "the write target is the first roms entry");

  // RetroArch keeps one flat saves/ for every system, so the same directory is
  // listed under every platform. Deduping it is the whole point of the helper:
  // a scanner that walked the map naively would read it eleven times.
  const std::vector<std::string> save_dirs = defaults.SaveScanDirs();
  std::size_t retroarch = 0;
  for (const std::string& dir : save_dirs) {
    if (dir == "/retroarch/saves") {
      ++retroarch;
    }
  }
  c.ExpectEq(retroarch, std::size_t{1}, "RetroArch's flat saves/ is scanned once, not per platform");
  c.Expect(save_dirs.size() > 1, "and the per-platform save folders are there too");
  c.Expect(defaults.StateScanDirs().size() > 1, "the same for states");
}

// --- the happy path ----------------------------------------------------------

void AWholeFileParses(checks::Checks& c) {
  const config::LoadResult result = config::ParseConfig(R"(; a comment
[server]
url = https://romm.example.com/    ; trailing slash and an inline comment

[sync]
enabled       = true
interval_min  = 45
on_boot       = false
saves         = yes
states        = ON
conflict_show = 0

[downloads]
enabled     = true
verify_hash = false
resume      = 1

[platform.snes]
roms   = /tico/roms/snes
saves  = /retroarch/saves, /tico/saves/snes
states = /retroarch/states, /tico/states/snes
)");

  c.Expect(result.ok(), "a good file has no errors: " + result.DescribeDiagnostics());
  c.ExpectEq(result.value.server.url, std::string("https://romm.example.com"),
             "the trailing slash is dropped");
  c.ExpectEq(result.value.sync.interval_min, 45, "interval_min");
  c.Expect(!result.value.sync.on_boot, "on_boot = false");
  c.Expect(result.value.sync.saves, "saves = yes");
  c.Expect(result.value.sync.states, "states = ON is case-insensitive");
  c.Expect(!result.value.sync.conflict_show, "conflict_show = 0");
  c.Expect(!result.value.downloads.verify_hash, "verify_hash = false");
  c.Expect(result.value.downloads.resume, "resume = 1");

  const config::PlatformFolders* snes = result.value.Platform("snes");
  c.Expect(snes != nullptr, "the overridden platform is there");
  if (snes != nullptr) {
    c.ExpectEq(snes->roms.size(), std::size_t{1}, "one rom folder");
    c.ExpectEq(snes->saves.size(), std::size_t{2}, "two save folders");
    c.ExpectEq(snes->saves[0], std::string("/retroarch/saves"), "in the order written");
  }
  // Restating the built-in mapping changes nothing: the other platforms are
  // still mapped, because config.ini carries overrides rather than the map.
  c.Expect(result.value.Platform("gba") != nullptr, "the platforms not named keep their defaults");

  // Every boolean spelling, since a user copying an example off the internet
  // will use all of them.
  bool flag = false;
  const char* const kTrue[] = {"true", "TRUE", "yes", "On", "1"};
  for (const char* text : kTrue) {
    flag = false;
    c.Expect(config::ParseBool(text, &flag) && flag, std::string(text) + " is true");
  }
  const char* const kFalse[] = {"false", "No", "OFF", "0"};
  for (const char* text : kFalse) {
    flag = true;
    c.Expect(config::ParseBool(text, &flag) && !flag, std::string(text) + " is false");
  }
  c.Expect(!config::ParseBool("maybe", &flag), "and nothing else is a boolean");
  c.Expect(!config::ParseBool("", &flag), "nor is an empty value");
}

void ItSurvivesHowPeopleActuallyWriteFiles(checks::Checks& c) {
  // CRLF from a Windows editor, a UTF-8 BOM in front of the first section, and
  // a last line with no newline after it. Each of these on its own has been the
  // whole reason a config "did nothing".
  const std::string text =
      "\xEF\xBB\xBF[server]\r\nurl = https://romm.example.com\r\n\r\n[sync]\r\n"
      "interval_min = 5\r\n[downloads]\r\nresume = false";
  const config::LoadResult result = config::ParseConfig(text);
  c.Expect(result.ok(), "BOM + CRLF + no trailing newline: " + result.DescribeDiagnostics());
  c.ExpectEq(result.value.server.url, std::string("https://romm.example.com"), "the URL survived");
  c.ExpectEq(result.value.sync.interval_min, 5, "the interval survived");
  c.Expect(!result.value.downloads.resume, "the last line, with no newline after it, survived");

  // Comments both ways, and the marker that is *not* one: a '#' with no space
  // in front of it is part of the value, or a folder called `disc#2` silently
  // becomes `disc`.
  const config::LoadResult hashes = config::ParseConfig(WithServer(
      "# a full-line comment\n[platform.psx]\nroms = /tico/roms/disc#2 ; the real comment\n"));
  const config::PlatformFolders* psx = hashes.value.Platform("psx");
  c.Expect(psx != nullptr && !psx->roms.empty(), "a psx mapping");
  if (psx != nullptr && !psx->roms.empty()) {
    c.ExpectEq(psx->roms[0], std::string("/tico/roms/disc#2"),
               "a '#' inside a word is part of the path, not a comment");
  }
}

// --- what a bad file does ------------------------------------------------------

void ABadFileStillBoots(checks::Checks& c) {
  // Not one line of this is usable. The client still comes up on defaults --
  // "never block boot" (CLAUDE.md) is what this whole module is for.
  const config::LoadResult result = config::ParseConfig(
      "\x01\x02 garbage\n[[[\nkey without a section = 1\n= no key\nnot a pair\n[sync]\n"
      "interval_min = soon\nenabled = perhaps\nunknown_key = 1\n[nonsense]\nx = 1\n");

  c.ExpectEq(result.value.sync.interval_min, 30, "the interval kept its default");
  c.Expect(result.value.sync.enabled, "and so did enabled");
  c.Expect(result.value.Platform("snes") != nullptr, "and the folder map is intact");
  c.Expect(CountOf(result, config::Severity::kWarning) >= 6,
           "every unusable line was named, not just the first");

  // The line number is the whole value of a diagnostic on a console with no
  // keyboard: "invalid boolean" is a hunt, "line 8" is a fix.
  const config::Diagnostic* enabled = Complaint(result, "sync", "enabled");
  c.Expect(enabled != nullptr, "the bad boolean was reported");
  if (enabled != nullptr) {
    c.ExpectEq(enabled->line, 8, "...against the line it is on");
    c.Expect(enabled->Describe().find("[sync] enabled") != std::string::npos,
             "...and names the section and key: " + enabled->Describe());
  }
  const config::Diagnostic* interval = Complaint(result, "sync", "interval_min");
  c.Expect(interval != nullptr && interval->line == 7, "so was the bad number");

  c.Expect(Mentions(result, "unknown key"), "an unknown key is called one");
  c.Expect(Mentions(result, "not a section this client knows"), "so is an unknown section");
  // A key under a section that was already rejected is not complained about
  // twice: the section header said why, and repeating it per line buries it.
  c.Expect(Complaint(result, "nonsense", "x") == nullptr,
           "keys under a rejected section are not each re-reported");
}

void NumbersAndTheirBounds(checks::Checks& c) {
  const config::LoadResult zero = config::ParseConfig(WithServer("[sync]\ninterval_min = 0\n"));
  c.ExpectEq(zero.value.sync.interval_min, 0, "0 is legal: boot and manual syncs only");
  c.Expect(zero.ok(), "and not a complaint: " + zero.DescribeDiagnostics());

  const config::LoadResult negative = config::ParseConfig(WithServer("[sync]\ninterval_min = -5\n"));
  c.ExpectEq(negative.value.sync.interval_min, 30, "a negative interval keeps the default");
  c.Expect(Complaint(negative, "sync", "interval_min") != nullptr, "...and says so");

  // Clamped rather than rejected. Falling back to 30 minutes would sync far
  // MORE often than someone asking for a year, which is the one direction they
  // certainly did not mean.
  const config::LoadResult huge =
      config::ParseConfig(WithServer("[sync]\ninterval_min = 525600\n"));
  c.ExpectEq(huge.value.sync.interval_min, config::kMaxIntervalMinutes,
             "an absurd interval is clamped down, never back up to the default");

  // The two shapes strtol would have accepted, each of which turns a typo into
  // a number that looks deliberate.
  for (const char* text : {"30 minutes", "0x1e", "1e3", "3.5", "", "+", "999999999999999999999"}) {
    const config::LoadResult bad =
        config::ParseConfig(WithServer(std::string("[sync]\ninterval_min = ") + text + "\n"));
    c.ExpectEq(bad.value.sync.interval_min, 30,
               std::string("'") + text + "' is not a number of minutes");
  }
  const config::LoadResult plus = config::ParseConfig(WithServer("[sync]\ninterval_min = +15\n"));
  c.ExpectEq(plus.value.sync.interval_min, 15, "a signed positive is still a number");
}

void DuplicatesResolveToTheLastLine(checks::Checks& c) {
  const config::LoadResult result = config::ParseConfig(WithServer(
      "[sync]\ninterval_min = 10\ninterval_min = 20\n[platform.gba]\nroms = /a\n"
      "[platform.gba]\nsaves = /b\nroms = /c\n"));

  c.ExpectEq(result.value.sync.interval_min, 20, "the last assignment wins");
  c.Expect(Mentions(result, "set more than once"), "...and the shadowed one is reported");

  const config::PlatformFolders* gba = result.value.Platform("gba");
  c.Expect(gba != nullptr, "the twice-declared platform is there");
  if (gba != nullptr) {
    c.ExpectEq(gba->roms.size(), std::size_t{1}, "one rom folder");
    c.ExpectEq(gba->roms[0], std::string("/c"), "and it is the last one written");
    c.ExpectEq(gba->saves.size(), std::size_t{1}, "the second section merged rather than replaced");
  }
  c.Expect(Mentions(result, "appears more than once"), "the repeated section is reported");

  // `[SYNC]` and `[sync]` are the same section, so a key set in both is set
  // twice -- and a diagnostic that named them separately would let that pass.
  const config::LoadResult cased =
      config::ParseConfig(WithServer("[SYNC]\ninterval_min = 10\n[sync]\ninterval_min = 20\n"));
  c.ExpectEq(cased.value.sync.interval_min, 20, "the last one still wins");
  c.Expect(Complaint(cased, "sync", "interval_min") != nullptr,
           "the section is reported under one canonical name");
  c.Expect(Mentions(cased, "set more than once"), "...and the duplicate is seen across the two");
  c.Expect(Mentions(cased, "appears more than once"),
           "a repeated [sync] is reported like a repeated platform, not silently");
}

// --- the folder map --------------------------------------------------------------

void PlatformSectionsReplaceTheBuiltInEntry(checks::Checks& c) {
  // The rule docs/CONFIG.md states: a platform with roms and no saves downloads
  // fine and syncs no saves. It only holds if a section replaces the built-in
  // entry rather than merging over it -- otherwise the default saves folders
  // would quietly stay in force and saves would never be skipped.
  const config::LoadResult result =
      config::ParseConfig(WithServer("[platform.snes]\nroms = /roms/snes\n"));

  const config::PlatformFolders* snes = result.value.Platform("snes");
  c.Expect(snes != nullptr, "snes is mapped");
  if (snes != nullptr) {
    c.ExpectEq(snes->roms.size(), std::size_t{1}, "the rom folder is the one written");
    c.Expect(snes->saves.empty(), "and the built-in saves folders are NOT merged back in");
  }
  // The footgun is real, so it is said out loud rather than left to be
  // discovered when a save does not come back.
  const config::Diagnostic* dropped = Complaint(result, "platform.snes", "saves");
  c.Expect(dropped != nullptr, "dropping the default saves folders is reported");
  c.Expect(result.ok(), "but it is a notice, not an error: " + result.DescribeDiagnostics());
  // Every other platform is untouched: this replaced one entry, not the map.
  c.Expect(result.value.Platform("gba") != nullptr, "gba keeps its built-in mapping");

  // An empty section is how a user removes a mapping for a system they do not
  // have. `Platform()` has one answer for "skip this", and it is nullptr.
  const config::LoadResult emptied =
      config::ParseConfig(WithServer("[platform.psp]\nroms =\nsaves =\nstates =\n"));
  c.Expect(emptied.value.Platform("psp") == nullptr, "a section that maps nothing unmaps it");
  c.Expect(emptied.value.RomTarget("psp").empty(), "so there is nowhere to download psp to");
  c.Expect(Mentions(emptied, "skipped entirely"), "...and that is reported");

  // A single typo'd key empties the section, because the header already
  // replaced the built-in entry -- which is the same end state as a deliberate
  // removal and must not read like one.
  const config::LoadResult typo = config::ParseConfig(WithServer("[platform.snes]\nrom = /x\n"));
  c.Expect(typo.value.Platform("snes") == nullptr, "a typo'd key leaves snes unmapped");
  const config::Diagnostic* wiped = Complaint(typo, "platform.snes", "");
  c.Expect(wiped != nullptr, "and that is reported");
  if (wiped != nullptr) {
    c.Expect(wiped->severity == config::Severity::kWarning,
             "as a warning, not the notice a deliberate removal gets");
  }
  const config::Diagnostic* deliberate = Complaint(emptied, "platform.psp", "");
  c.Expect(deliberate != nullptr && deliberate->severity == config::Severity::kNotice,
           "...and emptying a section on purpose stays a notice");

  // The map is bounded like the diagnostic list is: a corrupt card region full
  // of section headers is well inside the byte limit and still builds tens of
  // thousands of entries on a heap that does not have them.
  std::string many;
  for (std::size_t i = 0; i < config::kMaxPlatformSections + 50; ++i) {
    many += "[platform.p" + std::to_string(i) + "]\nroms = /r/" + std::to_string(i) + "\n";
  }
  const config::LoadResult flood = config::ParseConfig(WithServer(many));
  c.Expect(flood.value.Platform("p10") != nullptr, "the platforms up to the cap are mapped");
  c.Expect(flood.value.Platform("p300") == nullptr, "and the ones past it are not");
  c.Expect(flood.value.platforms.size() <=
               config::kMaxPlatformSections + config::DefaultPlatforms().size(),
           "so the map cannot be grown without bound by a corrupt file");
  // The cap message itself is one of the diagnostics the *diagnostic* cap has
  // already dropped by then, which is the right order: sixty-four reports and a
  // count of the rest is what a user can act on.
  c.Expect(Mentions(flood, "further problems"), "...and the report is bounded too");

  std::string wide = "[platform.gba]\nroms = ";
  for (std::size_t i = 0; i < config::kMaxPathsPerKey + 20; ++i) {
    wide += (i == 0 ? "" : ", ");
    wide += "/d/" + std::to_string(i);
  }
  const config::LoadResult long_list = config::ParseConfig(WithServer(wide + "\n"));
  const config::PlatformFolders* gba = long_list.value.Platform("gba");
  c.Expect(gba != nullptr && gba->roms.size() == config::kMaxPathsPerKey,
           "so is one key's directory list");
  c.Expect(Mentions(long_list, "directories; the rest are ignored"), "...and says so");

  // A slug the build ships no default for is used as written. Dropping it would
  // silently discard a mapping for a platform RomM knows about and we do not,
  // which is a folder map that does nothing and says nothing.
  const config::LoadResult unknown =
      config::ParseConfig(WithServer("[platform.wonderswan]\nroms = /tico/roms/ws\n"));
  c.ExpectEq(unknown.value.RomTarget("wonderswan"), std::string("/tico/roms/ws"),
             "an unrecognised slug is honoured");
  c.Expect(Mentions(unknown, "not a platform this build maps by default"),
           "...and flagged, because a typo here maps nothing");
  c.Expect(unknown.ok(), "a notice, not an error");

  // The slug is a directory name on RomM's filesystem, so its case is data.
  const config::LoadResult cased =
      config::ParseConfig(WithServer("[Platform.SNES]\nroms = /roms/SNES\n"));
  c.ExpectEq(cased.value.RomTarget("SNES"), std::string("/roms/SNES"),
             "the section keyword is case-insensitive, the slug is not");
  c.Expect(cased.value.Platform("snes") != nullptr, "and lowercase snes keeps its own default");
}

void PathsAreCheckedBeforeTheyAreUsed(checks::Checks& c) {
  std::string path;
  std::string why;

  c.Expect(config::NormalizeSdPath("/tico/roms/snes/", &path, &why) && path == "/tico/roms/snes",
           "a trailing slash is dropped");
  c.Expect(config::NormalizeSdPath("//tico//roms///snes", &path, &why) &&
               path == "/tico/roms/snes",
           "repeated separators collapse");
  c.Expect(config::NormalizeSdPath("/tico/./roms", &path, &why) && path == "/tico/roms",
           "a '.' segment is dropped");
  c.Expect(config::NormalizeSdPath("/", &path, &why) && path == "/", "the card root is a folder");
  c.Expect(config::NormalizeSdPath("  /tico/roms  ", &path, &why) && path == "/tico/roms",
           "surrounding whitespace is not part of a path");

  const char* const kRefused[] = {
      "tico/roms",              // relative: there is no working directory on a card
      "",                       // nothing
      "/tico/../../etc",        // '..' is refused, not resolved
      "C:\\roms",               // a Windows habit; FAT32 has no name with a backslash in it
      "/tico/roms\x01",         // a control character
  };
  for (const char* raw : kRefused) {
    why.clear();
    c.Expect(!config::NormalizeSdPath(raw, &path, &why),
             std::string("'") + raw + "' is not an SD path");
    c.Expect(!why.empty(), "...with a reason a user can act on");
  }
  // A NUL is the one that matters: every path API downstream stops at it, so
  // the folder written to would not be the folder that was validated.
  c.Expect(!config::NormalizeSdPath(std::string_view("/tico\0/roms", 11), &path, &why),
           "an embedded NUL is refused");
  c.Expect(!config::NormalizeSdPath("/" + std::string(config::kMaxPathLength, 'a'), &path, &why),
           "a path longer than the console can open is refused here, not there");

  // ...and in a real file: a bad entry drops out of the list, the good ones stay.
  const config::LoadResult result = config::ParseConfig(WithServer(
      "[platform.n64]\nroms = /tico/roms/n64/, relative/path, , /tico/roms/n64\n"));
  const config::PlatformFolders* n64 = result.value.Platform("n64");
  c.Expect(n64 != nullptr, "n64 is mapped");
  if (n64 != nullptr) {
    c.ExpectEq(n64->roms.size(), std::size_t{1},
               "the relative path and the empty entry are dropped, and the duplicate deduped");
    c.ExpectEq(n64->roms[0], std::string("/tico/roms/n64"), "the survivor is normalised");
  }
  c.Expect(Mentions(result, "is not absolute"), "the relative entry was named");
  c.Expect(Mentions(result, "empty entry"), "so was the empty one");
  c.Expect(Mentions(result, "listed twice"), "so was the duplicate");
}

// --- the server URL ---------------------------------------------------------------

void TheServerUrlIsAnOriginOrNothing(checks::Checks& c) {
  std::string url;
  std::string why;

  c.Expect(config::NormalizeServerUrl("https://romm.example.com/", &url, &why) &&
               url == "https://romm.example.com",
           "a trailing slash is dropped");
  c.Expect(config::NormalizeServerUrl("HTTPS://RomM.Example.COM:8443", &url, &why) &&
               url == "https://romm.example.com:8443",
           "the scheme and host are lowercased, and a port is kept");
  c.Expect(config::NormalizeServerUrl("https://romm.lan/RomM/", &url, &why) &&
               url == "https://romm.lan/RomM",
           "a path prefix is kept, and its case with it -- RomM behind a proxy is normal");
  c.Expect(config::NormalizeServerUrl("http://127.0.0.1:8080", &url, &why),
           "plain http is accepted; the warning about it is the caller's");

  const char* const kRefused[] = {
      "romm.example.com",             // no scheme: nothing knows what to do with it
      "ftp://romm.example.com",       // not a scheme this speaks
      "https://",                     // no host
      "https://romm.lan?token=x",     // a query: this is an address, not a link
      "https://romm.lan/#fragment",   // ...nor a fragment
      "https://romm .lan",            // a space
      "",
  };
  for (const char* raw : kRefused) {
    why.clear();
    c.Expect(!config::NormalizeServerUrl(raw, &url, &why),
             std::string("'") + raw + "' is not a server address");
    c.Expect(!why.empty(), "...with a reason");
  }

  // Credentials in the URL are refused rather than stripped, and the reason
  // never quotes the input: RomM authenticates with a bearer token, so a
  // password here is never sent anywhere -- it would only ride along into every
  // log line and diagnostic naming the server.
  why.clear();
  c.Expect(!config::NormalizeServerUrl("https://me:hunter2@romm.lan", &url, &why),
           "a URL carrying credentials is refused");
  c.Expect(why.find("hunter2") == std::string::npos, "and the reason does not repeat the password");

  const config::LoadResult leaky =
      config::ParseConfig("[server]\nurl = https://me:hunter2@romm.lan\n");
  c.Expect(!leaky.ok(), "...which leaves the client with no server, an error");
  c.Expect(!leaky.value.configured(), "and configured() says so");
  c.Expect(!Mentions(leaky, "hunter2"), "and no diagnostic repeats the password");

  // Plain http works, and says why it is a bad idea, once.
  const config::LoadResult insecure = config::ParseConfig("[server]\nurl = http://romm.lan:8080\n");
  c.Expect(insecure.ok(), "plain http is not an error -- a LAN RomM is a real deployment");
  c.ExpectEq(insecure.value.server.url, std::string("http://romm.lan:8080"), "and is used");
  c.Expect(Mentions(insecure, "clear"), "but the risk is named: " + insecure.DescribeDiagnostics());

  // A URL that never reaches ApplyServer at all: a colon instead of an equals
  // is an ordinary slip, and the line is then reported as "not a key = value"
  // -- quoted, because quoting it is what makes the report useful. So the
  // credential is removed rather than the quote.
  const config::LoadResult mistyped =
      config::ParseConfig("[server]\nurl: https://me:hunter2@romm.lan\n");
  c.Expect(!Mentions(mistyped, "hunter2"), "a credential in an unparseable line is redacted too");
  c.Expect(Mentions(mistyped, "romm.lan"), "...and the rest of the line still reaches the user");

  // A host is not the same thing as a non-empty authority. Each of these
  // normalises cleanly, would report configured(), and reaches nothing.
  for (const char* raw : {"https://:8080", "https://host:", "https://host:80x", "https://.",
                          "https://[::1", "https://[::1]x"}) {
    why.clear();
    c.Expect(!config::NormalizeServerUrl(raw, &url, &why),
             std::string("'") + raw + "' names no host");
  }
  c.Expect(config::NormalizeServerUrl("http://[::1]:8080", &url, &why) &&
               url == "http://[::1]:8080",
           "...but a bracketed IPv6 literal is a host: " + why);
  c.Expect(config::NormalizeServerUrl("http://10.0.0.2", &url, &why), "and so is an address");

  // A stale duplicate line must not idle a console that has a working server.
  // Every other key keeps its previous value on a bad line, with a warning;
  // this one is no different, and reporting "there is no server" while holding
  // a good one would be false.
  const config::LoadResult stale = config::ParseConfig(
      "[server]\nurl = https://good.example.com\nurl = ftp://bad\n");
  c.ExpectEq(stale.value.server.url, std::string("https://good.example.com"),
             "the usable URL stands");
  c.Expect(stale.value.configured(), "so the client is configured");
  c.Expect(stale.ok(), "and it is a warning, not an error: " + stale.DescribeDiagnostics());
  c.Expect(Mentions(stale, "does not configure a server"), "...saying which line was dropped");

  // ...and the same the other way round: a bad first line that a good second
  // one replaces leaves a configured client, so it cannot leave an error behind.
  const config::LoadResult fixed = config::ParseConfig(
      "[server]\nurl = ftp://bad\nurl = https://good.example.com\n");
  c.ExpectEq(fixed.value.server.url, std::string("https://good.example.com"), "the later one wins");
  c.Expect(fixed.ok(), "and no error survives it: " + fixed.DescribeDiagnostics());

  // No server at all is the one thing with no sensible default, so it is an
  // error rather than a warning -- and exactly one, not one per cause.
  const config::LoadResult none = config::ParseConfig("[sync]\nenabled = true\n");
  c.Expect(!none.ok(), "a config with no server is an error");
  c.ExpectEq(CountOf(none, config::Severity::kError), std::size_t{1}, "said once");
  c.Expect(none.value.sync.enabled, "and the rest of the file is still in force");

  // ...and a *broken* URL is reported as the broken URL, not additionally as a
  // missing one. Two errors for one typo is how a user stops reading them.
  const config::LoadResult broken = config::ParseConfig("[server]\nurl = romm.example.com\n");
  c.ExpectEq(CountOf(broken, config::Severity::kError), std::size_t{1},
             "one error for one bad line: " + broken.DescribeDiagnostics());
}

// --- the file on disk ---------------------------------------------------------------

void LoadingFromDisk(checks::Checks& c) {
  const std::filesystem::path dir = std::filesystem::path(ROMMSYNC_TEST_SCRATCH) / "config";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "config.ini").string();
  std::filesystem::remove(path);

  // Missing is not broken: it is a console nobody has configured yet, so the
  // folder map is still there and only the server is missing.
  const config::LoadResult absent = config::LoadConfig(path);
  c.Expect(!absent.value.configured(), "a missing file leaves no server");
  c.Expect(absent.value.Platform("snes") != nullptr, "...but the built-in folder map is in force");
  c.Expect(Mentions(absent, "does not exist"), "...and it is named as missing, not as broken");
  c.ExpectEq(CountOf(absent, config::Severity::kWarning), std::size_t{0},
             "a first boot is not a warning: " + absent.DescribeDiagnostics());

  {
    std::ofstream out(path, std::ios::binary);
    out << "[server]\nurl = https://romm.example.com\n[sync]\ninterval_min = 7\n";
  }
  const config::LoadResult loaded = config::LoadConfig(path);
  c.Expect(loaded.ok(), "a real file loads: " + loaded.DescribeDiagnostics());
  c.ExpectEq(loaded.value.sync.interval_min, 7, "and its values are in force");

  // The one moment config.ini legitimately does not exist: io::WriteAtomically
  // moves the record already in place to `.old` before renaming the new one on,
  // so a sysmodule interrupted there leaves the user's settings one filename
  // away. token_store and device_identity recover from the same window.
  std::filesystem::remove(path);
  {
    std::ofstream out(path + ".old", std::ios::binary);
    out << "[server]\nurl = https://recovered.example.com\n";
  }
  const config::LoadResult recovered = config::LoadConfig(path);
  c.ExpectEq(recovered.value.server.url, std::string("https://recovered.example.com"),
             "an interrupted write is read from the record it moved aside");
  c.Expect(Mentions(recovered, "was interrupted"), "...and the user is told it happened");
  std::filesystem::remove(path + ".old");

  // A card that was pulled mid-write hands this a file of whatever was in those
  // sectors. It is refused by size before it is read into a sysmodule heap that
  // could not hold it -- "never block boot" (CLAUDE.md).
  {
    std::ofstream out(path, std::ios::binary);
    const std::string filler(64 * 1024, 'x');
    for (int i = 0; i < 8; ++i) {
      out << filler;
    }
  }
  const config::LoadResult oversized = config::LoadConfig(path);
  c.Expect(!oversized.ok(), "an oversized file is an error");
  c.Expect(Mentions(oversized, "larger than"), "...named as one");
  c.Expect(oversized.value.Platform("snes") != nullptr, "...and the client still has a folder map");
  c.ExpectEq(oversized.value.sync.interval_min, 30, "...on defaults");
  std::remove(path.c_str());
}

void NothingCrashesOnAPartialFile(checks::Checks& c) {
  // Every prefix of a valid file, which is what an interrupted write leaves.
  // The assertion is deliberately weak -- a truncated file has no right answer
  // -- and the point is that all of them return, with a usable config.
  const std::string whole =
      "[server]\nurl = https://romm.example.com\n[sync]\ninterval_min = 12\n"
      "[platform.gbc]\nroms = /tico/roms/gbc\nsaves = /retroarch/saves\n";
  bool all_usable = true;
  for (std::size_t length = 0; length <= whole.size(); ++length) {
    const config::LoadResult result = config::ParseConfig(std::string_view(whole).substr(0, length));
    if (result.value.sync.interval_min < 0 || result.diagnostics.size() > config::kMaxDiagnostics + 1) {
      all_usable = false;
    }
  }
  c.Expect(all_usable, "every prefix of a config file parses into a usable one");

  // A file of nothing but bad lines is bounded: one diagnostic per line of a
  // megabyte of garbage is an outage dressed up as a report.
  std::string noise;
  for (int i = 0; i < 5000; ++i) {
    noise += "[sync]\nnope = 1\n";
  }
  const config::LoadResult flood = config::ParseConfig(noise);
  c.Expect(flood.diagnostics.size() <= config::kMaxDiagnostics + 1,
           "the diagnostic list is capped");
  c.Expect(Mentions(flood, "further problems"), "...and says how many it dropped");
}

// --- the document -------------------------------------------------------------------

/// The first ```ini block of docs/CONFIG.md.
std::string ReadDocumentedExample(const std::string& path, bool* found) {
  std::ifstream in(path);
  std::string line;
  std::string block;
  bool inside = false;
  while (std::getline(in, line)) {
    if (line.rfind("```", 0) == 0) {
      if (inside) {
        *found = true;
        return block;
      }
      inside = line.find("ini") != std::string::npos;
      continue;
    }
    if (inside) {
      block += line;
      block += "\n";
    }
  }
  return block;
}

void TheDocumentedExampleParses(checks::Checks& c) {
  bool found = false;
  const std::string example = ReadDocumentedExample(ROMMSYNC_CONFIG_DOC, &found);
  c.Expect(found, "docs/CONFIG.md still has an ```ini example in it");
  if (!found) {
    return;
  }

  const config::LoadResult result = config::ParseConfig(example);
  // The example is the file a user copies. If the parser complains about any of
  // it, that complaint is what they meet on their first boot -- and no test of
  // a string literal in this file would ever see it.
  c.ExpectEq(CountOf(result, config::Severity::kError), std::size_t{0},
             "the documented example has no errors: " + result.DescribeDiagnostics());
  c.ExpectEq(CountOf(result, config::Severity::kWarning), std::size_t{0},
             "...and no warnings either: " + result.DescribeDiagnostics());
  c.Expect(result.value.configured(), "it configures a server");
  c.ExpectEq(result.value.sync.interval_min, 30, "the interval the document shows");
  c.Expect(!result.value.sync.states, "states off, as the document shows");
  c.ExpectEq(result.value.RomTarget("snes"), std::string("/tico/roms/snes"),
             "and the snes mapping the document shows");
}

}  // namespace

int main() {
  checks::Checks c;
  DefaultsAreTheDocumentedOnes(c);
  AWholeFileParses(c);
  ItSurvivesHowPeopleActuallyWriteFiles(c);
  ABadFileStillBoots(c);
  NumbersAndTheirBounds(c);
  DuplicatesResolveToTheLastLine(c);
  PlatformSectionsReplaceTheBuiltInEntry(c);
  PathsAreCheckedBeforeTheyAreUsed(c);
  TheServerUrlIsAnOriginOrNothing(c);
  LoadingFromDisk(c);
  NothingCrashesOnAPartialFile(c);
  TheDocumentedExampleParses(c);
  return c.failures() == 0 ? 0 : 1;
}

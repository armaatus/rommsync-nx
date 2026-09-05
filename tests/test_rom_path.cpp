// M3-1: a rom turned into the absolute SD path it downloads to.
//
// The folder map itself is `core.config`'s; what is checked here is the join
// that hangs off it, and it splits the way the risk does.
//
// Five scenarios need no server, and must stay checked with docker stopped,
// because what they pin is a *refusal*. A `fs_name` is a name on someone else's
// filesystem: nothing between RomM's scanner and this client has ever looked at
// it, and the file it lands in is under a folder the user mapped. A name
// carrying `../..` that this code resolves rather than refuses is not a failed
// download -- it is a write outside the mapped folder, and the folders next
// door on a modded Switch are `atmosphere/` and `bootloader/`.
//
// The last needs the real RomM 5.2.0, because the one thing a literal in this
// file cannot be right about is which *field* the destination is keyed on.
// `platform_slug` and `platform_fs_slug` both read `gba` on the seeded library
// and only the second is the directory name the map uses, so the fixtures alone
// cannot tell a correct client from one that will map nothing the day a library
// calls its PlayStation folder `playstation`.
//
//   defaults  -- the built-in map, and that the key is the fs slug
//   override  -- a `[platform.gba]` section moves the destination with it
//   unmapped  -- a skip with a reason, never a guessed folder
//   hostile   -- separators, `..`, control characters, NUL, 800 characters
//   existing  -- "already on the card?" reads every roms entry, writes the first
//   library   -- the seeded roms, resolved from the fields RomM actually sends
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "harness.hpp"
#include "rommsync/config.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/json.hpp"

namespace config = rommsync::config;
namespace http = rommsync::http;
namespace json = rommsync::json;

namespace {

/// A parsed config with a usable server, so no scenario is also a test of the
/// missing-server error.
config::Config Parse(const std::string& sections) {
  return config::ParseConfig("[server]\nurl = https://romm.example.com\n" + sections).value;
}

/// The refusal a hostile name earns, asserted as a whole: an empty path, a
/// reason, and -- the part worth stating -- a reason that does not carry the
/// bytes it is complaining about into whatever log or overlay row prints it.
void ExpectRefused(checks::Checks& c, const config::Config& config, std::string_view fs_name,
                   std::string_view what) {
  const config::RomDestination destination = config.DestinationFor({"gba", fs_name});
  c.Expect(!destination.ok(), std::string(what) + " has no destination");
  c.ExpectEq(destination.path, std::string(), std::string(what) + " resolves to nothing at all");
  c.Expect(!destination.reason.empty(), std::string(what) + " says why");
  // An empty name is the one refusal with nothing to leak, and `find("")`
  // matches everything, so it is exempt rather than special-cased in the code.
  c.Expect(fs_name.empty() || destination.reason.find(fs_name) == std::string::npos,
           std::string(what) + " does not quote the name back into the log");
  c.Expect(config.ExistingRomPaths({"gba", fs_name}).empty(),
           std::string(what) + " is not looked for on the card either");
}

// --- the built-in map ---------------------------------------------------------

void Defaults(checks::Checks& c) {
  const config::Config defaults = config::Defaults();

  c.ExpectEq(defaults.DestinationFor({"gba", "synthetic-large.gba"}).path,
             std::string("/tico/roms/gba/synthetic-large.gba"),
             "the seeded gba rom lands under the built-in gba folder");
  c.ExpectEq(defaults.DestinationFor({"nes", "240pee.nes"}).path,
             std::string("/tico/roms/nes/240pee.nes"), "and the nes one under nes");

  // Spaces and no extension. Whether a disc set is *downloaded* is M3-4's
  // decision; that it resolves is this one's, and a resolver that quietly
  // sanitised the spaces would send the bytes to a path RomM never named.
  const config::RomDestination discs = defaults.DestinationFor({"psx", "Synthetic Two Disc Game"});
  c.ExpectEq(discs.path, std::string("/tico/roms/psx/Synthetic Two Disc Game"),
             "the seeded two-disc rom resolves verbatim, spaces and all");
  c.ExpectEq(discs.reason, std::string(), "a resolved destination carries no reason");

  // The key is `platform_fs_slug` -- RomM's *directory* name -- and nothing
  // else. On the seeded library the two slugs agree, so this is the assertion
  // no fixture can make: a library that keeps its PlayStation roms in a
  // directory called `playstation` reports `platform_slug: psx` and
  // `platform_fs_slug: playstation` for every rom in it, and a client keyed on
  // the wrong one downloads into the folder mapped for a directory the server
  // does not have.
  const config::Config renamed = Parse(
      "[platform.playstation]\n"
      "roms = /sdcard/games/playstation\n"
      "[platform.psx]\n"
      "roms =\n"
      "saves =\n"
      "states =\n");
  c.ExpectEq(renamed.DestinationFor({"playstation", "Game.chd"}).path,
             std::string("/sdcard/games/playstation/Game.chd"),
             "a platform is found by the name its RomM directory has");
  c.Expect(!renamed.DestinationFor({"psx", "Game.chd"}).ok(),
           "and not by the `platform_slug` that same library also reports");
}

// --- an override --------------------------------------------------------------

void Override(checks::Checks& c) {
  const config::Config config = Parse(
      "[platform.gba]\n"
      "roms = /sdcard/games/gba, /tico/roms/gba\n");

  c.ExpectEq(config.DestinationFor({"gba", "synthetic-large.gba"}).path,
             std::string("/sdcard/games/gba/synthetic-large.gba"),
             "the override is where the download goes");
  c.ExpectEq(config.DestinationFor({"nes", "240pee.nes"}).path,
             std::string("/tico/roms/nes/240pee.nes"),
             "and it moved gba alone -- nes still has its built-in folder");

  // The root is a legitimate, if odd, folder to map, and it is the one join
  // that must not produce `//`.
  const config::Config root = Parse("[platform.gba]\nroms = /\n");
  c.ExpectEq(root.DestinationFor({"gba", "a.gba"}).path, std::string("/a.gba"),
             "a rom folder at the card root joins without doubling the slash");

  // A trailing slash and a repeated one are normalised by the parser, so the
  // join inherits that rather than repeating the rule.
  const config::Config untidy = Parse("[platform.gba]\nroms = /tico//roms/gba/\n");
  c.ExpectEq(untidy.DestinationFor({"gba", "a.gba"}).path, std::string("/tico/roms/gba/a.gba"),
             "an untidily written folder still joins to one clean path");
}

// --- nowhere to put it --------------------------------------------------------

void Unmapped(checks::Checks& c) {
  const config::Config defaults = config::Defaults();

  // ps2 is unmapped on purpose (docs/CONFIG.md): nothing on the console runs
  // one, and the documented answer is a skip rather than a guessed folder.
  const config::RomDestination ps2 = defaults.DestinationFor({"ps2", "Game.iso"});
  c.Expect(!ps2.ok(), "an unmapped platform has no destination");
  c.ExpectEq(ps2.path, std::string(), "and it is empty, not a guess at one");
  c.Expect(!ps2.reason.empty(), "and it says so in a sentence");
  c.Expect(defaults.ExistingRomPaths({"ps2", "Game.iso"}).empty(),
           "there is nowhere on the card it could already be, either");

  // Mapped, but for saves only. `Platform()` answers, `RomTarget()` does not,
  // and the two are different sentences: one is "map this platform", the other
  // is "give this platform a roms folder".
  const config::Config saves_only = Parse("[platform.gba]\nsaves = /retroarch/saves\n");
  const config::RomDestination no_roms = saves_only.DestinationFor({"gba", "a.gba"});
  c.Expect(!no_roms.ok(), "a platform with saves and no roms folder downloads nowhere");
  c.Expect(no_roms.reason != ps2.reason, "and says something other than 'unmapped'");
  c.Expect(saves_only.ExistingRomPaths({"gba", "a.gba"}).empty(),
           "and has no rom folder to look in");

  // A platform switched off by emptying its section is erased from the map
  // entirely, so it reads exactly like one nobody ever mapped.
  const config::Config off = Parse("[platform.gba]\nroms =\nsaves =\nstates =\n");
  c.Expect(!off.DestinationFor({"gba", "a.gba"}).ok(), "a platform switched off downloads nothing");
}

// --- names the server chose ---------------------------------------------------

void Hostile(checks::Checks& c) {
  const config::Config defaults = config::Defaults();

  ExpectRefused(c, defaults, "../../atmosphere/x", "a traversing name");
  ExpectRefused(c, defaults, "..\\..\\atmosphere\\x", "a traversing name written with backslashes");
  ExpectRefused(c, defaults, "sub/dir.gba", "a name with a separator in it");
  ExpectRefused(c, defaults, "..", "a bare '..'");
  ExpectRefused(c, defaults, ".", "a bare '.'");
  ExpectRefused(c, defaults, "", "an empty name");
  ExpectRefused(c, defaults, "bell\x07here.gba", "a name with a control character");
  ExpectRefused(c, defaults, std::string_view("nul\0here.gba", 12), "a name with a NUL in it");
  ExpectRefused(c, defaults, std::string(800, 'a'), "an 800-character name");

  // Short enough to be a name and too long to be a path under this folder.
  // Truncating would leave a real file in the mapped folder, and a download
  // that verified its hash would then call it finished.
  const std::string just_too_long(config::kMaxPathLength - 5, 'a');
  ExpectRefused(c, defaults, just_too_long, "a name that only overflows once joined");

  // The bound is the resolved path, not the name, so the same name resolves
  // under a shorter folder.
  const config::Config shallow = Parse("[platform.gba]\nroms = /g\n");
  c.Expect(shallow.DestinationFor({"gba", just_too_long}).ok(),
           "...and the same name fits under a shorter folder");

  // The refusal is about the *name*, so it does not depend on the map: a
  // hostile name under an override is refused by the same rule.
  const config::Config elsewhere = Parse("[platform.gba]\nroms = /sdcard/games/gba\n");
  ExpectRefused(c, elsewhere, "../../atmosphere/x", "a traversing name under an override");

  // What FAT32 and exFAT reserve. Every one of these is legal on the Linux
  // filesystem RomM scanned, and `?` is how No-Intro spells a name, so this is
  // the refusal a real library reaches rather than an attacker: the card cannot
  // store it, and a skip that says so beats an `open` that fails a third of the
  // way through a 120 MiB download.
  for (const char reserved : std::string("?*:\"<>|")) {
    ExpectRefused(c, defaults, "Carmen Sandiego" + std::string(1, reserved) + ".nes",
                  std::string("a name containing '") + reserved + "'");
  }
  c.Expect(defaults.DestinationFor({"nes", "Where in Time (USA) [!].nes"}).ok(),
           "...and the punctuation a rom name actually needs still resolves");

  // The predicate the refusals are made of, checked directly -- M3-4 has the
  // same question to ask about the files inside a disc set.
  std::string why;
  c.Expect(config::ValidRomFileName("Synthetic Two Disc Game (Disc 1).bin", &why),
           "a disc file's name is a name: spaces, brackets and all");
  c.Expect(config::ValidRomFileName("...leading dots.gba", &why),
           "and a name that merely starts with dots is not a '..'");
  c.Expect(!config::ValidRomFileName("a/b", &why) && !why.empty(),
           "a rejection sets a reason to print");
}

// --- is it already on the card? -----------------------------------------------

void Existing(checks::Checks& c) {
  const config::Config config = Parse(
      "[platform.gba]\n"
      "roms = /sdcard/games/gba, /tico/roms/gba, /retroarch/roms/gba\n");

  const std::vector<std::string> paths = config.ExistingRomPaths({"gba", "a.gba"});
  c.ExpectEq(paths.size(), std::size_t{3}, "every mapped roms folder is a place it could be");
  c.ExpectEq(paths[0], std::string("/sdcard/games/gba/a.gba"), "the write target is first");
  c.ExpectEq(paths[1], std::string("/tico/roms/gba/a.gba"), "then the map's order is kept");
  c.ExpectEq(paths[2], std::string("/retroarch/roms/gba/a.gba"), "...to the end of the list");
  c.ExpectEq(paths.front(), config.DestinationFor({"gba", "a.gba"}).path,
             "and the first is exactly where a download would be written");

  // Only the first is written to, so a rom found in the second is on the card
  // and must not be downloaded again. That is the whole reason the rest are
  // read (docs/CONFIG.md).
  const config::Config single = Parse("[platform.gba]\nroms = /tico/roms/gba\n");
  c.ExpectEq(single.ExistingRomPaths({"gba", "a.gba"}).size(), std::size_t{1},
             "one mapped folder is one place to look");

  // A candidate too long to be a path is a file the card cannot be holding, so
  // it drops out of the list rather than being truncated into a real one.
  const config::Config mixed =
      Parse("[platform.gba]\nroms = /g, /" + std::string(700, 'x') + "/deep\n");
  const std::vector<std::string> some = mixed.ExistingRomPaths({"gba", std::string(300, 'a')});
  c.ExpectEq(some.size(), std::size_t{1}, "a candidate that cannot be a path is dropped, not cut");
  c.ExpectEq(some.front().substr(0, 3), std::string("/g/"), "and the one that fits is kept");

  // The order matters: when it is the *first* folder that overflows, this list
  // is not empty and its `front()` is the second folder, while `DestinationFor`
  // refuses. The two do not agree here, and a caller that took `front()` for a
  // write target would write outside the mapped write folder -- so the header
  // says `DestinationFor` is the only answer to that question, and this is the
  // configuration that makes it matter.
  const config::Config deep_first =
      Parse("[platform.gba]\nroms = /" + std::string(700, 'x') + "/deep, /g\n");
  // Named, because `RomFile` holds views: the header says it is an argument and
  // never a record, and `-Wdangling-gsl` says the same thing louder.
  const std::string big_name(300, 'a');
  const config::RomFile big{"gba", big_name};
  const std::vector<std::string> found = deep_first.ExistingRomPaths(big);
  c.ExpectEq(found.size(), std::size_t{1}, "the folder that fits is still a place to look");
  c.Expect(!deep_first.DestinationFor(big).ok(), "while the write target itself is refused");
  c.Expect(found.front() != deep_first.DestinationFor(big).path,
           "so these two answers are not interchangeable");
}

// --- against the real library -------------------------------------------------

/// Every seeded rom, resolved from the fields RomM itself sent.
///
/// The point is the field *names*. This reads `platform_fs_slug` and `fs_name`
/// out of a live `GET /api/roms` body and puts them through the same resolver
/// the download worker will, so a client keyed on `platform_slug` -- or on a
/// field RomM 5.2.0 stopped sending -- fails here rather than on a console.
void Library(checks::Checks& c, http::HttpClient& client, const std::string& base,
             const harness::Fixture& fixture) {
  const http::Result listed =
      client.Send(harness::Authed(http::Method::kGet, base + "/api/roms?limit=200", fixture));
  c.Expect(listed.successful(), "GET /api/roms");
  if (!listed.successful()) {
    return;
  }
  const json::ParseResult document = json::Parse(listed.response.body);
  const json::Value* items = document.ok() ? document.value.Find("items") : nullptr;
  c.Expect(items != nullptr, "the roms envelope has items");
  if (items == nullptr) {
    return;
  }

  const config::Config defaults = config::Defaults();
  std::size_t resolved = 0;
  for (const json::Value& item : items->elements()) {
    const std::string fs_name = harness::Field(item, "fs_name");
    const std::string fs_slug = harness::Field(item, "platform_fs_slug");
    c.Expect(!fs_name.empty(), "every rom RomM lists carries a `fs_name`");
    c.Expect(!fs_slug.empty(), "...and a `platform_fs_slug`, which is what the map is keyed by");
    if (fs_name.empty() || fs_slug.empty()) {
      continue;
    }

    const config::RomDestination destination = defaults.DestinationFor({fs_slug, fs_name});
    c.Expect(destination.ok(), "the seeded rom '" + fs_name + "' resolves to a path");
    if (!destination.ok()) {
      continue;
    }
    ++resolved;
    c.ExpectEq(destination.path, "/tico/roms/" + fs_slug + "/" + fs_name,
               "'" + fs_name + "' lands under its platform's folder, named as RomM names it");
    c.ExpectEq(defaults.ExistingRomPaths({fs_slug, fs_name}).front(), destination.path,
               "and that is the first place a card check looks for it");
  }

  // The seeded library is six roms across four platforms; a run that resolved
  // none of them would otherwise pass this scenario silently.
  c.Expect(resolved >= 6, "every seeded rom resolved (" + std::to_string(resolved) + " of 6)");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "defaults";
  const std::string base = rig::BaseUrl();
  checks::Checks checks;

  if (scenario != "library") {
    if (scenario == "defaults") {
      Defaults(checks);
    } else if (scenario == "override") {
      Override(checks);
    } else if (scenario == "unmapped") {
      Unmapped(checks);
    } else if (scenario == "hostile") {
      Hostile(checks);
    } else if (scenario == "existing") {
      Existing(checks);
    } else {
      std::cerr << "unknown scenario: " << scenario << "\n";
      return 2;
    }
    if (checks.failures() == 0) {
      std::cout << "rom." << scenario << " ok\n";
    }
    return checks.failures() == 0 ? 0 : 1;
  }

  const std::unique_ptr<http::HttpClient> client = rommsync::host::MakeCurlHttpClient();
  if (!rig::Reachable(*client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }
  rig::DisarmFault(*client, base);

  harness::Fixture fixture;
  if (!harness::LoadFixture(&fixture)) {
    return rig::kSkip;
  }
  Library(checks, *client, base, fixture);

  if (checks.failures() == 0) {
    std::cout << "rom.library ok against " << base << "\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

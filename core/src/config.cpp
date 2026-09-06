#include "rommsync/config.hpp"

#include "rommsync/atomic_file.hpp"

#include "config_text.hpp"

#include <cstddef>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::config {
namespace {

// --- text ------------------------------------------------------------------
// The rules the format is read *and written* by live in config_text.hpp, so the
// writer M5-3 added cannot disagree with this parser about what a comment is.
using text::LowerAscii;
using text::Redact;
using text::StripInlineComment;
using text::Trim;

// --- diagnostics -----------------------------------------------------------

/// Collects complaints, and stops collecting long before they can be a problem.
///
/// The cap is not tidiness. `config.ini` is on a FAT32 card, and a card that
/// was pulled mid-write hands this parser a megabyte of whatever used to be in
/// those sectors -- one diagnostic per line of it, each with a section name and
/// a message, is tens of megabytes of strings on a heap that does not have them.
class Diagnostics {
 public:
  void Add(Severity severity, int line, std::string_view section, std::string_view key,
           std::string message) {
    if (items_.size() >= kMaxDiagnostics) {
      ++dropped_;
      if (severity == Severity::kError) {
        dropped_error_ = true;
      }
      return;
    }
    Diagnostic entry;
    entry.severity = severity;
    entry.line = line;
    entry.section = std::string(section);
    entry.key = std::string(key);
    entry.message = std::move(message);
    items_.push_back(std::move(entry));
  }

  std::vector<Diagnostic> Take() {
    if (dropped_ > 0) {
      Diagnostic summary;
      summary.severity = dropped_error_ ? Severity::kError : Severity::kWarning;
      summary.message = "and " + std::to_string(dropped_) +
                        " further problems, not listed -- fix these first";
      items_.push_back(std::move(summary));
    }
    return std::move(items_);
  }

 private:
  std::vector<Diagnostic> items_;
  std::size_t dropped_ = 0;
  bool dropped_error_ = false;
};

// --- values ----------------------------------------------------------------

using text::ParseInt;
using text::SplitList;

/// `authority` is `host`, `host:port`, or `[v6]:port` with a host that could
/// name something.
///
/// "Could name something" is one alphanumeric character, which is as far as a
/// client can check without a resolver: `romm.lan`, `10.0.0.2` and `[::1]` pass,
/// `:8080`, `.` and `host:` do not.
bool HasRealHost(std::string_view authority) {
  std::string_view host = authority;
  if (!host.empty() && host.front() == '[') {
    const std::size_t close = host.find(']');
    if (close == std::string_view::npos) {
      return false;
    }
    const std::string_view after = host.substr(close + 1);
    if (!after.empty() && (after.front() != ':' || after.size() == 1)) {
      return false;
    }
    for (std::size_t i = 1; i < after.size(); ++i) {
      if (after[i] < '0' || after[i] > '9') {
        return false;
      }
    }
    host = host.substr(1, close - 1);
  } else {
    const std::size_t colon = host.find(':');
    if (colon != std::string_view::npos) {
      const std::string_view port = host.substr(colon + 1);
      if (port.empty()) {
        return false;
      }
      for (const char c : port) {
        if (c < '0' || c > '9') {
          return false;
        }
      }
      host = host.substr(0, colon);
    }
  }
  for (const char c : host) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
      return true;
    }
  }
  return false;
}

// --- the built-in folder map ------------------------------------------------

PlatformFolders TicoAndRetroArch(const std::string& slug) {
  PlatformFolders folders;
  folders.roms = {"/tico/roms/" + slug};
  folders.saves = {"/retroarch/saves", "/tico/saves/" + slug};
  folders.states = {"/retroarch/states", "/tico/states/" + slug};
  return folders;
}

std::map<std::string, PlatformFolders, std::less<>> BuildDefaultPlatforms() {
  // The systems a Switch has an emulator for, as RomM's `library/roms/` folder
  // names conventionally spell them. Anything heavier (ps2, ps3, ps4, wii, ngc,
  // 3ds) is deliberately absent: nothing on the console runs it, and a default
  // mapping would only invite downloads that cannot be played.
  const char* const kSlugs[] = {"nes",     "snes", "gb",  "gbc", "gba", "n64",
                                "genesis", "psx",  "nds", "dreamcast", "psp"};
  std::map<std::string, PlatformFolders, std::less<>> platforms;
  for (const char* slug : kSlugs) {
    platforms.emplace(slug, TicoAndRetroArch(slug));
  }
  return platforms;
}

// --- the file ---------------------------------------------------------------

/// Read at most `kMaxConfigBytes` of `path`.
///
/// The bound and its reasoning live in `io::ReadBounded` (atomic_file.hpp):
/// `config.ini` is a file a human and a card reader both get to touch, so it
/// stops at a named refusal rather than a `bad_alloc`. `state.db` needs exactly
/// the same read for the same reason, and one copy of the "one byte past the
/// bound" check is one place for it to drift.
io::BoundedRead ReadBounded(const std::string& path, std::string* out) {
  return io::ReadBounded(path, kMaxConfigBytes, out);
}

// --- the parse ---------------------------------------------------------------

enum class SectionKind { kNone, kServer, kSync, kDownloads, kPlatform, kUnknown };

/// The state one line of the file is read against.
struct ParseState {
  SectionKind kind = SectionKind::kNone;

  /// As written, brackets removed: `sync`, `platform.snes`. For diagnostics.
  std::string section;

  /// The `platform_fs_slug`, original case, when `kind` is `kPlatform`.
  std::string slug;
};

void ApplyServer(const ParseState& state, int line, std::string_view key, std::string_view value,
                 Config* config, Diagnostics* diags) {
  if (key != "url") {
    diags->Add(Severity::kWarning, line, state.section, key, "unknown key, ignored");
    return;
  }
  std::string url;
  std::string why;
  if (!NormalizeServerUrl(value, &url, &why)) {
    // A warning, like every other rejected line: this one did not take effect,
    // and whether that leaves the client with no server at all is a question
    // only the end of the file can answer. Saying "there is no server" here
    // would be false for `url = good` followed by a stale `url = ftp://bad`,
    // and saying nothing would be false the other way round.
    //
    // The message never quotes `value`: someone will write
    // `https://me:hunter2@romm.lan`, and this string reaches a log.
    diags->Add(Severity::kWarning, line, state.section, key,
               why + " -- this line does not configure a server");
    return;
  }
  config->server.url = url;
  if (url.rfind("http://", 0) == 0) {
    diags->Add(Severity::kWarning, line, state.section, key,
               "plain http: the bearer token and every save cross the network in the clear");
  }
}

void ApplySync(const ParseState& state, int line, std::string_view key, std::string_view value,
               Config* config, Diagnostics* diags) {
  const std::string quoted = "'" + std::string(value) + "'";
  bool flag = false;

  if (key == "interval_min") {
    int minutes = 0;
    if (!ParseInt(value, &minutes)) {
      diags->Add(Severity::kWarning, line, state.section, key,
                 "expected a whole number of minutes, got " + quoted + "; keeping " +
                     std::to_string(config->sync.interval_min));
      return;
    }
    if (minutes < kMinIntervalMinutes) {
      diags->Add(Severity::kWarning, line, state.section, key,
                 "cannot be negative; keeping " + std::to_string(config->sync.interval_min) +
                     " (use 0 for boot and manual syncs only)");
      return;
    }
    if (minutes > kMaxIntervalMinutes) {
      // Clamped rather than rejected: falling back to the 30-minute default
      // would sync *more* often than a user asking for months, which is the one
      // direction they clearly did not want.
      diags->Add(Severity::kWarning, line, state.section, key,
                 "longer than the " + std::to_string(kMaxIntervalMinutes) +
                     "-minute maximum; using that instead");
      minutes = kMaxIntervalMinutes;
    }
    config->sync.interval_min = minutes;
    return;
  }

  bool* target = nullptr;
  if (key == "enabled") {
    target = &config->sync.enabled;
  } else if (key == "on_boot") {
    target = &config->sync.on_boot;
  } else if (key == "saves") {
    target = &config->sync.saves;
  } else if (key == "states") {
    target = &config->sync.states;
  } else if (key == "conflict_show") {
    target = &config->sync.conflict_show;
  }
  if (target == nullptr) {
    diags->Add(Severity::kWarning, line, state.section, key, "unknown key, ignored");
    return;
  }
  if (!ParseBool(value, &flag)) {
    diags->Add(Severity::kWarning, line, state.section, key,
               "expected true or false, got " + quoted + "; keeping " +
                   std::string(*target ? "true" : "false"));
    return;
  }
  *target = flag;
}

void ApplyDownloads(const ParseState& state, int line, std::string_view key,
                    std::string_view value, Config* config, Diagnostics* diags) {
  bool* target = nullptr;
  if (key == "enabled") {
    target = &config->downloads.enabled;
  } else if (key == "verify_hash") {
    target = &config->downloads.verify_hash;
  } else if (key == "resume") {
    target = &config->downloads.resume;
  }
  if (target == nullptr) {
    diags->Add(Severity::kWarning, line, state.section, key, "unknown key, ignored");
    return;
  }
  bool flag = false;
  if (!ParseBool(value, &flag)) {
    diags->Add(Severity::kWarning, line, state.section, key,
               "expected true or false, got '" + std::string(value) + "'; keeping " +
                   std::string(*target ? "true" : "false"));
    return;
  }
  *target = flag;
}

/// `rejected` collects slugs whose section had a line the parser could not use,
/// so the end-of-parse sweep can tell a platform emptied on purpose from one
/// emptied by a typo.
void ApplyPlatform(const ParseState& state, int line, std::string_view key,
                   std::string_view value, Config* config, Diagnostics* diags,
                   std::set<std::string>* rejected) {
  std::vector<std::string>* target = nullptr;
  PlatformFolders& folders = config->platforms[state.slug];
  if (key == "roms") {
    target = &folders.roms;
  } else if (key == "saves") {
    target = &folders.saves;
  } else if (key == "states") {
    target = &folders.states;
  }
  if (target == nullptr) {
    diags->Add(Severity::kWarning, line, state.section, key, "unknown key, ignored");
    rejected->insert(state.slug);
    return;
  }

  std::vector<std::string> paths;
  if (Trim(value).empty()) {
    // `roms =` is not a list with an empty element in it; it is the way a user
    // says "nowhere". Emptying every key of a section is how a built-in mapping
    // is removed, so this has to be silent -- warning about it would make the
    // documented way to unmap a platform look like a mistake.
    *target = std::move(paths);
    return;
  }
  for (std::string_view piece : SplitList(value)) {
    const std::string_view raw = Trim(piece);
    if (raw.empty()) {
      diags->Add(Severity::kWarning, line, state.section, key,
                 "empty entry in the list, ignored");
      rejected->insert(state.slug);
      continue;
    }
    if (paths.size() >= kMaxPathsPerKey) {
      diags->Add(Severity::kWarning, line, state.section, key,
                 "lists more than " + std::to_string(kMaxPathsPerKey) +
                     " directories; the rest are ignored");
      break;
    }
    std::string path;
    std::string why;
    if (!NormalizeSdPath(raw, &path, &why)) {
      diags->Add(Severity::kWarning, line, state.section, key,
                 "'" + Redact(raw) + "' " + why + ", ignored");
      rejected->insert(state.slug);
      continue;
    }
    bool duplicate = false;
    for (const std::string& seen : paths) {
      if (seen == path) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      diags->Add(Severity::kNotice, line, state.section, key,
                 "'" + path + "' is listed twice; the second is ignored");
      continue;
    }
    paths.push_back(std::move(path));
  }
  *target = std::move(paths);
}

}  // namespace

// --- small public helpers ----------------------------------------------------

const char* ToString(Severity severity) {
  switch (severity) {
    case Severity::kNotice:
      return "notice";
    case Severity::kWarning:
      return "warning";
    case Severity::kError:
      return "error";
  }
  return "unknown";
}

std::string Diagnostic::Describe() const {
  std::string described;
  if (line > 0) {
    described += "line " + std::to_string(line) + " ";
  }
  if (!section.empty()) {
    described += "[" + section + "] ";
  }
  if (!key.empty()) {
    described += key + ": ";
  }
  described += message;
  return described;
}

bool LoadResult::ok() const {
  for (const Diagnostic& entry : diagnostics) {
    if (entry.severity == Severity::kError) {
      return false;
    }
  }
  return true;
}

std::string LoadResult::DescribeDiagnostics() const {
  std::string described;
  for (const Diagnostic& entry : diagnostics) {
    described += ToString(entry.severity);
    described += ": ";
    described += entry.Describe();
    described += "\n";
  }
  return described;
}

bool ParseBool(std::string_view text, bool* out) {
  const std::string lowered = LowerAscii(Trim(text));
  if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1") {
    *out = true;
    return true;
  }
  if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0") {
    *out = false;
    return true;
  }
  return false;
}

bool NormalizeSdPath(std::string_view raw, std::string* out, std::string* why) {
  const std::string_view path = Trim(raw);
  if (path.empty()) {
    *why = "is empty";
    return false;
  }
  if (path.size() > kMaxPathLength) {
    *why = "is longer than the " + std::to_string(kMaxPathLength) + " characters a path can be";
    return false;
  }
  if (path[0] != '/') {
    *why = "is not absolute (SD paths start at the card root, '/')";
    return false;
  }
  for (const char c : path) {
    // A NUL is the one that matters: every path this reaches -- fopen, libnx's
    // fsdev -- stops at it, so the folder that gets written to would not be the
    // folder that was validated. The rest are refused because FAT32 has no
    // names containing them, so they can only be a mistake.
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      *why = "contains a control character";
      return false;
    }
    if (c == '\\') {
      *why = "contains a backslash (SD paths use '/', and FAT32 has no name with one in it)";
      return false;
    }
  }

  std::string normalised;
  normalised.reserve(path.size());
  std::size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') {
      ++i;
    }
    const std::size_t begin = i;
    while (i < path.size() && path[i] != '/') {
      ++i;
    }
    const std::string_view segment = path.substr(begin, i - begin);
    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      // Refused, not resolved: these paths name folders a human picked, so a
      // `..` is a typo -- and resolving one silently turns that typo into a
      // directory that exists and gets downloaded into.
      *why = "contains a '..' segment";
      return false;
    }
    normalised += '/';
    normalised.append(segment);
  }
  // Everything collapsed away: the path was `/`, `//` or `/./`, which is the
  // card root and a legitimate, if odd, folder to map.
  *out = normalised.empty() ? "/" : normalised;
  return true;
}

bool ValidRomFileName(std::string_view fs_name, std::string* why) {
  if (fs_name.empty()) {
    *why = "is empty";
    return false;
  }
  if (fs_name.size() > kMaxPathLength) {
    // Bounded here as well as at the join, because this predicate is asked
    // standalone -- M3-4 has the same question about the files inside a disc
    // set -- and a caller with no folder in hand still needs an answer.
    //
    // The bound is the path's, not FAT32's 255-character limit on one name
    // component, and deliberately so: that limit counts UTF-16 characters and
    // this counts bytes, so a 255-byte check would refuse a legal name the
    // moment it carried an accent. Refusing what cannot be a path is this
    // module's job; refusing what the card will not accept is the card's
    // answer, and it comes back as a named write failure rather than a
    // download that goes somewhere else.
    *why = "is longer than the " + std::to_string(kMaxPathLength) + " characters a name can be";
    return false;
  }
  for (const char c : fs_name) {
    // The same two refusals `NormalizeSdPath` makes, for the same reasons: a
    // NUL truncates the path every writer downstream opens, so the folder
    // written to would not be the folder checked, and FAT32 has no name with a
    // control character in it.
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      *why = "contains a control character";
      return false;
    }
    // Not normalised into a subdirectory: this is one leaf, and a separator in
    // it is the server describing a path rather than naming a file.
    if (c == '/' || c == '\\') {
      *why = "contains a path separator, and a rom's name is one file, not a path";
      return false;
    }
    // The rest of what FAT32 and exFAT reserve. RomM scans a Linux filesystem,
    // where every one of these is a legal character and `?` is *conventional* --
    // `Where in Time Is Carmen Sandiego? (USA).nes` is how No-Intro spells it.
    // The card cannot hold that name, so the choice is between a skip that says
    // why and an `open` that fails halfway through a download and leaves a
    // `.part` behind for a retry that will fail the same way forever.
    //
    // Refused rather than rewritten: the name is also the key to "is it already
    // on the card", so a client that sanitised it would re-download the rom on
    // every tick, having looked for it under a name it never wrote.
    if (c == '?' || c == '*' || c == ':' || c == '"' || c == '<' || c == '>' || c == '|') {
      *why = "contains a character the SD card's filesystem cannot store in a name";
      return false;
    }
  }
  // Unreachable through a separator, since one is refused above -- but a bare
  // `..` is still a directory this client must not name. The sentence spells
  // neither of them out: it goes into a log beside a name it must not quote,
  // and a reason that echoes its own subject makes that promise untestable.
  if (fs_name == "." || fs_name == "..") {
    *why = "is a relative directory reference rather than a file name";
    return false;
  }
  return true;
}

bool NormalizeServerUrl(std::string_view raw, std::string* out, std::string* why) {
  const std::string_view url = Trim(raw);
  if (url.empty()) {
    *why = "is empty";
    return false;
  }
  if (url.size() > kMaxPathLength) {
    *why = "is implausibly long";
    return false;
  }
  for (const char c : url) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
      *why = "contains a space or a control character";
      return false;
    }
  }

  const std::string lowered = LowerAscii(url);
  std::size_t scheme = 0;
  if (lowered.rfind("https://", 0) == 0) {
    scheme = 8;
  } else if (lowered.rfind("http://", 0) == 0) {
    scheme = 7;
  } else {
    *why = "must start with http:// or https://";
    return false;
  }

  const std::string_view rest = url.substr(scheme);
  const std::size_t path_at = rest.find('/');
  const std::string_view authority = rest.substr(0, path_at);
  if (authority.empty()) {
    *why = "names no server";
    return false;
  }
  if (authority.find('@') != std::string_view::npos) {
    // Refused rather than stripped. RomM authenticates with a bearer token, so
    // a username and password here are never sent anywhere -- they would only
    // ride along in every log line and diagnostic that names the server, which
    // is how a password ends up in a GitHub issue.
    *why = "carries a username or password, which RomM never uses -- remove the part before '@'";
    return false;
  }
  if (rest.find('?') != std::string_view::npos || rest.find('#') != std::string_view::npos) {
    *why = "has a query or a fragment; this is a server address, not a link to a page";
    return false;
  }
  if (!HasRealHost(authority)) {
    // A non-empty authority is not a host: `https://:8080` normalises cleanly,
    // reports `configured()`, and reaches nothing -- a healthy-looking config
    // over a client that quietly does something else, which is the one outcome
    // this module exists to prevent.
    *why = "names no host (a port or a bare ':' is not a server)";
    return false;
  }

  // Scheme and host are case-insensitive and are lowercased so two spellings of
  // one server are one string -- `token.dat` records the server a token was
  // issued against, and comparing it must not turn a capital letter into a
  // forced re-pair. A path is case-sensitive and is left alone.
  std::string normalised = lowered.substr(0, scheme) + LowerAscii(authority);
  if (path_at != std::string_view::npos) {
    std::string path;
    std::string ignored;
    if (!NormalizeSdPath(rest.substr(path_at), &path, &ignored)) {
      *why = "has a path that is not usable";
      return false;
    }
    if (path != "/") {
      normalised += path;
    }
  }
  *out = normalised;
  return true;
}

// --- defaults ----------------------------------------------------------------

const std::map<std::string, PlatformFolders, std::less<>>& DefaultPlatforms() {
  static const std::map<std::string, PlatformFolders, std::less<>> kPlatforms =
      BuildDefaultPlatforms();
  return kPlatforms;
}

Config Defaults() {
  Config config;
  config.platforms = DefaultPlatforms();
  return config;
}

// --- Config accessors ---------------------------------------------------------

const PlatformFolders* Config::Platform(std::string_view slug) const {
  const auto found = platforms.find(slug);
  return found == platforms.end() ? nullptr : &found->second;
}

std::string Config::RomTarget(std::string_view slug) const {
  const PlatformFolders* folders = Platform(slug);
  if (folders == nullptr || folders->roms.empty()) {
    return {};
  }
  return folders->roms.front();
}

namespace {

/// The `roms` folders `rom`'s platform maps, or nullptr and the reason there
/// are none.
///
/// Both resolvers ask through this, so "unmapped", "mapped, but with nowhere to
/// download to" and "a name this client will not have" cannot come to mean two
/// different things depending on which one was called.
const std::vector<std::string>* RomFolders(const Config& config, const RomFile& rom,
                                           std::string* why) {
  const PlatformFolders* folders = config.Platform(rom.platform_fs_slug);
  if (folders == nullptr) {
    *why = "no folder is mapped for this rom's platform";
    return nullptr;
  }
  if (folders->roms.empty()) {
    *why = "this rom's platform maps saves or states but no roms folder";
    return nullptr;
  }
  std::string refusal;
  if (!ValidRomFileName(rom.fs_name, &refusal)) {
    *why = "the name this rom has on the server " + refusal;
    return nullptr;
  }
  return &folders->roms;
}

/// `directory` (already normalised) with `leaf` under it, or false and why not.
///
/// The length bound is the whole point of the function. Refusing is the only
/// answer: a truncated path is not a shorter name for the same file, it is a
/// different file, in the mapped folder, that a verified download would then
/// declare finished.
bool JoinRomPath(const std::string& directory, std::string_view leaf, std::string* out,
                 std::string* why) {
  std::string joined = directory;
  if (joined.empty() || joined.back() != '/') {
    joined += '/';  // every path from the map is normalised, so only "/" ends in one
  }
  joined.append(leaf);
  if (joined.size() > kMaxPathLength) {
    *why = "would be longer than the " + std::to_string(kMaxPathLength) +
           " characters a path can be";
    return false;
  }
  *out = std::move(joined);
  return true;
}

}  // namespace

RomDestination Config::DestinationFor(const RomFile& rom) const {
  RomDestination destination;
  const std::vector<std::string>* folders = RomFolders(*this, rom, &destination.reason);
  if (folders == nullptr) {
    return destination;
  }
  std::string why;
  if (!JoinRomPath(folders->front(), rom.fs_name, &destination.path, &why)) {
    destination.reason = "the path under '" + folders->front() + "' " + why;
  }
  return destination;
}

std::vector<std::string> Config::ExistingRomPaths(const RomFile& rom) const {
  std::vector<std::string> paths;
  std::string why;
  const std::vector<std::string>* folders = RomFolders(*this, rom, &why);
  if (folders == nullptr) {
    return paths;
  }
  paths.reserve(folders->size());
  for (const std::string& directory : *folders) {
    std::string joined;
    // A candidate too long to be a path is a file the card cannot be holding,
    // so it is dropped rather than reported: this answers "where might it
    // already be", and `DestinationFor` is where a refusal gets its sentence.
    if (JoinRomPath(directory, rom.fs_name, &joined, &why)) {
      paths.push_back(std::move(joined));
    }
  }
  return paths;
}

namespace {

std::vector<std::string> Union(const std::map<std::string, PlatformFolders, std::less<>>& platforms,
                               std::vector<std::string> PlatformFolders::*which) {
  std::vector<std::string> all;
  std::set<std::string> seen;
  for (const auto& entry : platforms) {
    for (const std::string& dir : entry.second.*which) {
      if (seen.insert(dir).second) {
        all.push_back(dir);
      }
    }
  }
  return all;
}

}  // namespace

std::vector<std::string> Config::SaveScanDirs() const {
  return Union(platforms, &PlatformFolders::saves);
}

std::vector<std::string> Config::StateScanDirs() const {
  return Union(platforms, &PlatformFolders::states);
}

// --- parsing ------------------------------------------------------------------

LoadResult ParseConfig(std::string_view text) {
  LoadResult result;
  result.value = Defaults();
  Diagnostics diags;

  if (text.size() > kMaxConfigBytes) {
    diags.Add(Severity::kError, 0, "", "",
              "is larger than the " + std::to_string(kMaxConfigBytes) +
                  " bytes a configuration can be; the built-in defaults are in use");
    result.diagnostics = diags.Take();
    return result;
  }

  // A UTF-8 BOM is what a Windows text editor leaves in front of the first
  // `[`, and without this the first section header is `\xEF\xBB\xBF[server]`
  // and the whole file is "unknown section".
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
    text.remove_prefix(3);
  }

  ParseState state;
  std::set<std::string> sections_seen;       // canonical section names already opened
  std::set<std::string> platform_sections;   // which slugs the file has claimed
  std::set<std::string> platform_rejected;   // ...and which had a line that was dropped
  std::set<std::string> assigned;            // "section\x1fkey" pairs already set
  bool platform_cap_reported = false;

  int line_number = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t newline = text.find('\n', begin);
    const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
    const std::string_view line = text.substr(begin, end - begin);
    begin = end + 1;
    ++line_number;

    const std::string_view content = Trim(StripInlineComment(line));
    if (content.empty()) {
      if (newline == std::string_view::npos) {
        break;
      }
      continue;
    }

    // --- a section header ---
    if (content.front() == '[') {
      if (content.back() != ']' || content.size() < 3) {
        diags.Add(Severity::kWarning, line_number, "", "",
                  "'" + Redact(content) + "' is not a section header; everything under it "
                  "is ignored until the next one");
        state = ParseState{};
        state.kind = SectionKind::kUnknown;
        continue;
      }
      const std::string_view name = Trim(content.substr(1, content.size() - 2));
      const std::string lowered = LowerAscii(name);

      // The section a diagnostic names is the *canonical* spelling, not the one
      // that was written: `[SYNC]` and `[sync]` are one section, and reporting
      // them under two names would let the same key be set twice without the
      // duplicate being noticed. An unrecognised section keeps its spelling,
      // because there the point is to show the user their typo.
      state = ParseState{};
      state.section = lowered;
      if (lowered == "server") {
        state.kind = SectionKind::kServer;
      } else if (lowered == "sync") {
        state.kind = SectionKind::kSync;
      } else if (lowered == "downloads") {
        state.kind = SectionKind::kDownloads;
      } else if (lowered.rfind("platform.", 0) == 0) {
        // The slug keeps its case: it is RomM's `platform_fs_slug`, which is a
        // directory name on the server's filesystem, and lowercasing it would
        // stop `[platform.SNES]` from ever matching the folder it names.
        state.slug = std::string(name.substr(std::string_view("platform.").size()));
        state.section = "platform." + state.slug;
        if (state.slug.empty()) {
          diags.Add(Severity::kWarning, line_number, state.section, "",
                    "names no platform; everything under it is ignored");
          state.kind = SectionKind::kUnknown;
          continue;
        }
        state.kind = SectionKind::kPlatform;
        if (platform_sections.find(state.slug) == platform_sections.end()) {
          if (platform_sections.size() >= kMaxPlatformSections) {
            // The same bound `kMaxDiagnostics` puts on what a corrupt file can
            // make this allocate, applied to the map: a card region full of
            // `[platform.a1]` headers is well inside the byte limit.
            if (!platform_cap_reported) {
              diags.Add(Severity::kWarning, line_number, state.section, "",
                        "is past the " + std::to_string(kMaxPlatformSections) +
                            " platforms a configuration may map; it and any after it are ignored");
              platform_cap_reported = true;
            }
            state.kind = SectionKind::kUnknown;
            continue;
          }
          platform_sections.insert(state.slug);
          if (DefaultPlatforms().find(state.slug) == DefaultPlatforms().end()) {
            diags.Add(Severity::kNotice, line_number, state.section, "",
                      "is not a platform this build maps by default; it is used as written -- "
                      "check it matches the folder name under RomM's library/roms/, because a "
                      "slug that matches nothing maps nothing");
          }
          // A platform section *replaces* the built-in entry for that platform
          // rather than adding to it, so what the file says is what the client
          // does -- a map you can read off the file you wrote. The cost is that
          // overriding `roms` alone drops the default `saves`, which is exactly
          // the "downloads work, saves are skipped" case docs/CONFIG.md
          // describes. The sweep at the end of the parse says so out loud.
          result.value.platforms[state.slug] = PlatformFolders{};
        }
      } else {
        state.section = std::string(name);
        diags.Add(Severity::kWarning, line_number, state.section, "",
                  "is not a section this client knows; everything under it is ignored");
        state.kind = SectionKind::kUnknown;
      }
      // Every section, not only a platform one: `[sync]` written twice is the
      // same shadowing hazard, and reporting one and not the other is how a
      // reader concludes the other is fine.
      if (state.kind != SectionKind::kUnknown && !sections_seen.insert(state.section).second) {
        diags.Add(Severity::kWarning, line_number, state.section, "",
                  "appears more than once; the sections are merged, and a key set in both "
                  "takes its later value");
      }
      continue;
    }

    // --- a key = value ---
    const std::size_t equals = content.find('=');
    if (equals == std::string_view::npos) {
      diags.Add(Severity::kWarning, line_number, state.section, "",
                "'" + Redact(content) + "' is not a 'key = value' line, ignored");
      continue;
    }
    const std::string_view key_text = Trim(content.substr(0, equals));
    const std::string_view value = Trim(content.substr(equals + 1));
    if (key_text.empty()) {
      diags.Add(Severity::kWarning, line_number, state.section, "",
                "a line with no key before the '=', ignored");
      continue;
    }
    if (state.kind == SectionKind::kNone) {
      diags.Add(Severity::kWarning, line_number, "", std::string(key_text),
                "is above the first section header, so there is nothing it configures");
      continue;
    }
    if (state.kind == SectionKind::kUnknown) {
      continue;  // the section header already said why; one message is enough
    }

    const std::string key = LowerAscii(key_text);
    const std::string pair = state.section + "\x1f" + key;
    if (!assigned.insert(pair).second) {
      diags.Add(Severity::kWarning, line_number, state.section, key,
                "is set more than once; the last usable line wins");
    }

    switch (state.kind) {
      case SectionKind::kServer:
        ApplyServer(state, line_number, key, value, &result.value, &diags);
        break;
      case SectionKind::kSync:
        ApplySync(state, line_number, key, value, &result.value, &diags);
        break;
      case SectionKind::kDownloads:
        ApplyDownloads(state, line_number, key, value, &result.value, &diags);
        break;
      case SectionKind::kPlatform:
        ApplyPlatform(state, line_number, key, value, &result.value, &diags,
                      &platform_rejected);
        break;
      case SectionKind::kNone:
      case SectionKind::kUnknown:
        break;
    }

    if (newline == std::string_view::npos) {
      break;
    }
  }

  // What a platform section dropped by not restating it. Said once per platform,
  // after the whole file, because a section is only complete at the end of it --
  // and said at all because "I only changed the rom folder" is the single most
  // likely way a user stops their own saves from syncing.
  for (const std::string& slug : platform_sections) {
    const std::string section = "platform." + slug;
    const PlatformFolders& now = result.value.platforms[slug];
    if (now.empty()) {
      // Emptying a section on purpose is how a user removes a built-in mapping
      // for a system they do not have, so on its own that is a notice. A section
      // the parser had to drop a line from is a different thing wearing the same
      // shape: `rom = /x` for `roms` unmaps snes entirely, and reporting that as
      // calmly as a deliberate removal is how it goes unnoticed.
      const bool by_accident = platform_rejected.find(slug) != platform_rejected.end();
      diags.Add(by_accident ? Severity::kWarning : Severity::kNotice, 0, section, "",
                by_accident
                    ? "maps no folders because every line in it was dropped, so " + slug +
                          " is skipped entirely -- see the warnings above for which"
                    : "maps no folders, so " + slug + " is skipped entirely");
      continue;
    }
    const auto defaults = DefaultPlatforms().find(slug);
    if (defaults == DefaultPlatforms().end()) {
      continue;
    }
    if (now.roms.empty() && !defaults->second.roms.empty()) {
      diags.Add(Severity::kNotice, 0, section, "roms",
                "is not set, and a platform section replaces the built-in map for that platform "
                "rather than adding to it, so " + slug + " roms have nowhere to download to");
    }
    if (now.saves.empty() && !defaults->second.saves.empty()) {
      diags.Add(Severity::kNotice, 0, section, "saves",
                "is not set, and a platform section replaces the built-in map for that platform "
                "rather than adding to it, so " + slug + " saves will not sync");
    }
  }

  // An entry that maps nothing is the same thing as no entry: `Platform()` has
  // one answer for "skip this platform", and it is nullptr.
  for (auto it = result.value.platforms.begin(); it != result.value.platforms.end();) {
    it = it->second.empty() ? result.value.platforms.erase(it) : std::next(it);
  }

  // Exactly one error for "no server", whatever the cause: absent, or present
  // and unusable. The line that was unusable already has its own warning, and a
  // second error repeating it is how a user stops reading them.
  if (!result.value.configured()) {
    diags.Add(Severity::kError, 0, "server", "url",
              "has no usable value, so there is no RomM to sync with");
  }

  result.diagnostics = diags.Take();
  return result;
}

LoadResult LoadConfig(const std::string& path) {
  std::string contents;
  const io::BoundedRead outcome = ReadBounded(path, &contents);
  if (outcome == io::BoundedRead::kOk) {
    return ParseConfig(contents);
  }

  if (outcome == io::BoundedRead::kMissing) {
    // The one moment `config.ini` legitimately does not exist is the window
    // `io::WriteAtomically` opens: the record already in place is renamed to
    // `.old` before the new one is renamed on. `token_store` and
    // `device_identity` recover from it the same way, and only from a *missing*
    // file -- answering a transient failure with the previous record is a
    // different and worse thing (see atomic_file.hpp).
    std::string previous;
    if (ReadBounded(io::PreviousPathFor(path), &previous) == io::BoundedRead::kOk) {
      LoadResult recovered = ParseConfig(previous);
      Diagnostics diags;
      diags.Add(Severity::kWarning, 0, "", "",
                path + " is missing and was read from " + io::PreviousPathFor(path) +
                    " instead -- a write of it was interrupted");
      std::vector<Diagnostic> all = diags.Take();
      all.insert(all.end(), recovered.diagnostics.begin(), recovered.diagnostics.end());
      recovered.diagnostics = std::move(all);
      return recovered;
    }
  }

  LoadResult result;
  result.value = Defaults();
  Diagnostics diags;
  switch (outcome) {
    case io::BoundedRead::kMissing:
      // Not a failure: it is a console nobody has configured yet. The client
      // still has a folder map, and `configured()` is what says it has no
      // server -- reported below as the error it is, once, rather than twice.
      diags.Add(Severity::kNotice, 0, "", "",
                path + " does not exist; the built-in defaults are in use");
      break;
    case io::BoundedRead::kUnreadable:
      diags.Add(Severity::kError, 0, "", "",
                path + " exists and could not be read; the built-in defaults are in use and "
                       "your settings are not");
      break;
    case io::BoundedRead::kTooLarge:
      diags.Add(Severity::kError, 0, "", "",
                path + " is larger than the " + std::to_string(kMaxConfigBytes) +
                    " bytes a configuration can be; the built-in defaults are in use");
      break;
    case io::BoundedRead::kOk:
      break;
  }
  diags.Add(Severity::kError, 0, "server", "url",
            "has no usable value, so there is no RomM to sync with");
  result.diagnostics = diags.Take();
  return result;
}

}  // namespace rommsync::config

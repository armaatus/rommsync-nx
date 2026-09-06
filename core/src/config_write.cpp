// `config::ApplyEdit` -- the write half of `config.ini`, and the reason there is
// no `Config` -> text serialiser anywhere in this module.
//
// The file is the one thing on the card a human edits by hand, and a serialiser
// would rebuild it from a struct: the comments they wrote, the blank lines they
// grouped their platforms with, the order of their `[platform.x]` sections and
// every section this build has never heard of would all be gone the first time
// the overlay moved a switch. So this edits *text*. One line changes; every
// other byte -- BOM, CRLF, trailing comment, the lot -- is carried through.
//
// The other half of the job is that validation here refuses, where the parser
// cannot. `LoadConfig` may never fail (nothing blocks boot), so it clamps and
// drops and carries on; this runs with a person watching the overlay, who can be
// told that a week is the ceiling. Writing a value the next boot would drop is a
// setting that looks saved and is not, which is worse than a refusal.
#include "rommsync/config.hpp"

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config_text.hpp"

namespace rommsync::config {
namespace {

using text::IsSpace;
using text::LowerAscii;
using text::ParseInt;
using text::Redact;
using text::SplitList;
using text::StripInlineComment;
using text::Trim;

void Complain(std::vector<Diagnostic>* diagnostics, Severity severity, std::string section,
              std::string key, std::string message) {
  Diagnostic entry;
  entry.severity = severity;
  // Zero: this complaint is about the edit that was asked for, not about a line
  // of the file -- the line it would have gone on does not exist yet, and
  // naming one that does would send the user to somebody else's setting.
  entry.line = 0;
  entry.section = std::move(section);
  entry.key = std::move(key);
  entry.message = std::move(message);
  diagnostics->push_back(std::move(entry));
}

// --- the file as lines --------------------------------------------------------

/// One line and the terminator it had, kept apart so a CRLF file stays a CRLF
/// file and a file with no final newline does not silently gain one.
struct Line {
  std::string text;
  std::string terminator;  ///< "\r\n", "\n", or "" on a last line with none
};

/// A `config.ini` taken apart far enough to change one line of it.
struct Document {
  /// The UTF-8 BOM a Windows editor leaves in front of the first `[`, or empty.
  /// Carried rather than dropped: removing it would be a byte this edit did not
  /// ask to change, and `ParseConfig` already knows to step over one.
  std::string bom;

  std::vector<Line> lines;

  /// What a *new* line ends with: the file's own ending, so an edit to a file
  /// written on Windows does not leave one lone Unix line in the middle of it.
  std::string terminator = "\n";

  std::string Render() const {
    std::string out = bom;
    for (const Line& line : lines) {
      out += line.text;
      out += line.terminator;
    }
    return out;
  }
};

Document Split(std::string_view text) {
  Document doc;
  if (text.rfind("\xEF\xBB\xBF", 0) == 0) {
    doc.bom = "\xEF\xBB\xBF";
    text.remove_prefix(3);
  }
  // One `\r\n` anywhere makes this a CRLF file. A mixed one is already
  // inconsistent, and matching the majority would only make which ending a new
  // line gets depend on how many lines happened to be around it.
  doc.terminator = text.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";

  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t newline = text.find('\n', begin);
    Line line;
    if (newline == std::string_view::npos) {
      line.text = std::string(text.substr(begin));
      doc.lines.push_back(std::move(line));
      return doc;
    }
    std::size_t end = newline;
    if (end > begin && text[end - 1] == '\r') {
      --end;
    }
    line.text = std::string(text.substr(begin, end - begin));
    line.terminator = std::string(text.substr(end, newline + 1 - end));
    doc.lines.push_back(std::move(line));
    begin = newline + 1;
  }
  return doc;
}

/// The name `ParseConfig` would key this section under, or empty when it is not
/// a section header at all.
///
/// The rule is the parser's, restated nowhere else: a section name is
/// case-insensitive, and the slug of a `platform.` one is *not*, because it is a
/// directory name on RomM's own filesystem.
std::string CanonicalSection(std::string_view written) {
  const std::string_view name = Trim(written);
  const std::string lowered = LowerAscii(name);
  if (lowered.rfind("platform.", 0) == 0) {
    const std::string_view slug = name.substr(std::string_view("platform.").size());
    if (slug.empty()) {
      return {};  // `[platform.]` names no platform; the parser ignores it too
    }
    return "platform." + std::string(slug);
  }
  return lowered;
}

/// Whether `line` opens a section, and which one.
///
/// `*section` is left **empty** for a header this build cannot key -- `[sync`
/// with no bracket, `[platform.]` with no slug. That is not the same as "not a
/// header", and the difference matters: `ParseConfig` ignores everything under
/// one of those, so a walk that let the previous section carry on through it
/// would find a `saves = ...` line that configures nothing and rewrite it.
/// A target section is never empty, so an empty `*section` matches nothing.
bool OpensSection(const std::string& line, std::string* section) {
  const std::string_view content = Trim(StripInlineComment(line));
  if (content.empty() || content.front() != '[') {
    return false;
  }
  section->clear();
  if (content.size() >= 3 && content.back() == ']') {
    *section = CanonicalSection(content.substr(1, content.size() - 2));
  }
  return true;
}

/// The key a `key = value` line sets, lowercased, or empty when it sets none.
std::string KeyOf(const std::string& line) {
  const std::string_view content = Trim(StripInlineComment(line));
  if (content.empty() || content.front() == '[') {
    return {};
  }
  const std::size_t equals = content.find('=');
  if (equals == std::string_view::npos) {
    return {};
  }
  return LowerAscii(Trim(content.substr(0, equals)));
}

/// Every line in `section` that assigns `key`, in file order.
std::vector<std::size_t> Assignments(const Document& doc, const std::string& section,
                                     const std::string& key) {
  std::vector<std::size_t> hits;
  std::string current;
  for (std::size_t i = 0; i < doc.lines.size(); ++i) {
    if (OpensSection(doc.lines[i].text, &current)) {
      continue;
    }
    if (current == section && KeyOf(doc.lines[i].text) == key) {
      hits.push_back(i);
    }
  }
  return hits;
}

/// Where a new key goes inside `section`: after its last `key = value`, or after
/// its header when it has none. False when the file has no such section.
///
/// Trailing blank and comment lines are stepped back over deliberately. The
/// comment at the bottom of a section is nearly always the *next* section's
/// preamble -- docs/CONFIG.md's own example ends `[platform.gba]` with
/// `; ... nes, gbc, gb` -- and appending under it would file the new key with
/// the wrong explanation.
bool InsertPoint(const Document& doc, const std::string& section, std::size_t* at) {
  bool found = false;
  std::string current;
  for (std::size_t i = 0; i < doc.lines.size(); ++i) {
    if (OpensSection(doc.lines[i].text, &current)) {
      if (current == section) {
        found = true;
        *at = i + 1;
      }
      continue;
    }
    if (current == section && !KeyOf(doc.lines[i].text).empty()) {
      *at = i + 1;
    }
  }
  return found;
}

void Insert(Document* doc, std::size_t at, std::string text) {
  Line line;
  line.text = std::move(text);
  if (at == doc->lines.size() && !doc->lines.empty() && doc->lines.back().terminator.empty()) {
    // The file ended without a final newline. It still does: the line that was
    // last gets the terminator it now needs, and the new last line has none.
    doc->lines.back().terminator = doc->terminator;
  } else {
    line.terminator = doc->terminator;
  }
  doc->lines.insert(doc->lines.begin() + static_cast<std::ptrdiff_t>(at), std::move(line));
}

void Erase(Document* doc, std::size_t at) {
  if (at + 1 == doc->lines.size() && doc->lines[at].terminator.empty() && at > 0) {
    // ...and the same the other way: dropping the last line of a file that had
    // no final newline must not give it one.
    doc->lines[at - 1].terminator.clear();
  }
  doc->lines.erase(doc->lines.begin() + static_cast<std::ptrdiff_t>(at));
}

/// `line` with its value replaced, and everything else about it kept.
///
/// The indentation, the spacing around the `=` and any trailing `; comment` are
/// the user's, and an edit that reformatted them would make a one-setting change
/// look like a rewrite of the file in any diff they ever take of their card.
std::string Rewritten(const std::string& line, const std::string& value) {
  const std::string_view whole = line;
  const std::string_view content = StripInlineComment(whole);
  const std::string_view comment = whole.substr(content.size());
  const std::size_t equals = content.find('=');
  const std::string_view head = content.substr(0, equals + 1);
  const std::string_view after = content.substr(equals + 1);

  std::size_t lead = 0;
  while (lead < after.size() && IsSpace(after[lead])) {
    ++lead;
  }
  std::size_t trail = after.size();
  while (trail > lead && IsSpace(after[trail - 1])) {
    --trail;
  }

  std::string rebuilt(head);
  if (lead > 0) {
    rebuilt += std::string(after.substr(0, lead));
  } else if (after.empty()) {
    rebuilt += " ";  // the line was `key =`, and `key =true` is not an improvement
  }
  rebuilt += value;
  rebuilt += std::string(after.substr(trail));
  rebuilt += std::string(comment);
  if (comment.empty() && value.empty()) {
    // Nothing left to hold the spacing open for, so emptying a value leaves
    // `roms =` rather than a line with a space hanging off the end of it.
    while (!rebuilt.empty() && IsSpace(rebuilt.back())) {
      rebuilt.pop_back();
    }
  }
  return rebuilt;
}

/// A folder list as `config.ini` writes one.
std::string Join(const std::vector<std::string>& paths) {
  std::string joined;
  for (const std::string& path : paths) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += path;
  }
  return joined;
}

std::string NewAssignment(const std::string& key, const std::string& value) {
  std::string line = key + " =";
  if (!value.empty()) {
    line += " " + value;
  }
  return line;
}

void AppendSection(Document* doc, const std::string& section,
                   const std::vector<std::pair<std::string, std::string>>& body) {
  if (!doc->lines.empty() && !Trim(doc->lines.back().text).empty()) {
    Insert(doc, doc->lines.size(), "");
  }
  Insert(doc, doc->lines.size(), "[" + section + "]");
  for (const auto& [key, value] : body) {
    Insert(doc, doc->lines.size(), NewAssignment(key, value));
  }
}

// --- what a value is allowed to be --------------------------------------------

enum class ValueKind { kUnknown, kBool, kMinutes, kUrl, kPathList };

ValueKind KindOf(const std::string& section, const std::string& key) {
  if (section == "server") {
    return key == "url" ? ValueKind::kUrl : ValueKind::kUnknown;
  }
  if (section == "sync") {
    if (key == "interval_min") {
      return ValueKind::kMinutes;
    }
    if (key == "enabled" || key == "on_boot" || key == "saves" || key == "states" ||
        key == "conflict_show") {
      return ValueKind::kBool;
    }
    return ValueKind::kUnknown;
  }
  if (section == "downloads") {
    if (key == "enabled" || key == "verify_hash" || key == "resume") {
      return ValueKind::kBool;
    }
    return ValueKind::kUnknown;
  }
  if (section.rfind("platform.", 0) == 0) {
    if (key == "roms" || key == "saves" || key == "states") {
      return ValueKind::kPathList;
    }
  }
  return ValueKind::kUnknown;
}

/// The `[sync]`/`[downloads]` flag `key` names, or nullptr.
///
/// One table read two ways: `KindOf` says a key is a boolean and this says which
/// one, so the read-back check below cannot be looking at a different field from
/// the one the write set.
const bool* BoolField(const Config& config, const std::string& section,
                      const std::string& key) {
  if (section == "sync") {
    if (key == "enabled") return &config.sync.enabled;
    if (key == "on_boot") return &config.sync.on_boot;
    if (key == "saves") return &config.sync.saves;
    if (key == "states") return &config.sync.states;
    if (key == "conflict_show") return &config.sync.conflict_show;
    return nullptr;
  }
  if (section == "downloads") {
    if (key == "enabled") return &config.downloads.enabled;
    if (key == "verify_hash") return &config.downloads.verify_hash;
    if (key == "resume") return &config.downloads.resume;
  }
  return nullptr;
}

const std::vector<std::string>* PathField(const Config& config, const std::string& section,
                                          const std::string& key) {
  const PlatformFolders* folders =
      config.Platform(std::string_view(section).substr(std::string_view("platform.").size()));
  if (folders == nullptr) {
    return nullptr;
  }
  if (key == "roms") return &folders->roms;
  if (key == "saves") return &folders->saves;
  if (key == "states") return &folders->states;
  return nullptr;
}

/// One assignment, canonically keyed and with its value already normalised.
struct Resolved {
  std::string section;
  std::string key;
  std::string value;
  bool remove = false;
  ValueKind kind = ValueKind::kUnknown;
};

/// True when writing `value` after an `=` would read back as `value`.
///
/// The hazard is real rather than theoretical: `/roms/great #2` is a perfectly
/// legal SD path and `NormalizeSdPath` returns it unchanged, but a `#` after a
/// space starts a comment, so the next boot would map that platform to
/// `/roms/great` and download into a folder nobody named. Refusing is the only
/// answer -- quoting or escaping it would invent a syntax `config.ini` does not
/// have and no hand edit would ever use.
bool SurvivesReadBack(const std::string& value) {
  for (const char c : value) {
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      return false;
    }
  }
  return StripInlineComment(value) == value && Trim(value) == value;
}

bool NormalizeValue(const Resolved& target, std::string_view raw, std::string* out,
                    std::vector<Diagnostic>* diagnostics) {
  switch (target.kind) {
    case ValueKind::kUrl: {
      std::string url;
      std::string why;
      if (!NormalizeServerUrl(raw, &url, &why)) {
        // `why` never quotes its input, which is the whole reason this is not
        // assembled here: a URL is the one configured value that can carry a
        // credential, and this sentence reaches a log and the overlay.
        Complain(diagnostics, Severity::kError, target.section, target.key,
                 why + " -- the server was not changed");
        return false;
      }
      *out = std::move(url);
      if (out->rfind("http://", 0) == 0) {
        Complain(diagnostics, Severity::kWarning, target.section, target.key,
                 "plain http: the bearer token and every save cross the network in the clear");
      }
      return true;
    }
    case ValueKind::kMinutes: {
      const std::string_view trimmed = Trim(raw);
      int minutes = 0;
      if (!ParseInt(trimmed, &minutes)) {
        Complain(diagnostics, Severity::kError, target.section, target.key,
                 "expected a whole number of minutes, got '" + Redact(trimmed) + "'");
        return false;
      }
      if (minutes < kMinIntervalMinutes) {
        Complain(diagnostics, Severity::kError, target.section, target.key,
                 "cannot be negative; use 0 for boot and manual syncs only");
        return false;
      }
      if (minutes > kMaxIntervalMinutes) {
        // The read path clamps this and says so, because a file already on the
        // card has to boot. Here there is somebody looking at the number, so
        // they get told what the ceiling is instead of watching their value
        // change by itself after they saved it.
        Complain(diagnostics, Severity::kError, target.section, target.key,
                 "is longer than the " + std::to_string(kMaxIntervalMinutes) +
                     "-minute (one week) maximum");
        return false;
      }
      *out = std::to_string(minutes);
      return true;
    }
    case ValueKind::kBool: {
      bool flag = false;
      if (!ParseBool(raw, &flag)) {
        Complain(diagnostics, Severity::kError, target.section, target.key,
                 "expected true or false, got '" + Redact(Trim(raw)) + "'");
        return false;
      }
      *out = flag ? "true" : "false";
      return true;
    }
    case ValueKind::kPathList: {
      std::vector<std::string> paths;
      if (!Trim(raw).empty()) {
        for (const std::string_view piece : SplitList(raw)) {
          const std::string_view entry = Trim(piece);
          if (entry.empty()) {
            Complain(diagnostics, Severity::kError, target.section, target.key,
                     "has an empty entry in its list; leave the whole value empty to map "
                     "nowhere");
            return false;
          }
          if (paths.size() >= kMaxPathsPerKey) {
            Complain(diagnostics, Severity::kError, target.section, target.key,
                     "lists more than the " + std::to_string(kMaxPathsPerKey) +
                         " directories one key may have");
            return false;
          }
          std::string path;
          std::string why;
          if (!NormalizeSdPath(entry, &path, &why)) {
            Complain(diagnostics, Severity::kError, target.section, target.key,
                     "'" + Redact(entry) + "' " + why);
            return false;
          }
          bool duplicate = false;
          for (const std::string& seen : paths) {
            duplicate = duplicate || seen == path;
          }
          if (duplicate) {
            // A notice and not a refusal: the same folder written twice has one
            // obvious right answer, and the read path already resolves it this
            // way. Refusing would be strictness with nothing behind it.
            Complain(diagnostics, Severity::kNotice, target.section, target.key,
                     "'" + path + "' is listed twice; the second is dropped");
            continue;
          }
          paths.push_back(std::move(path));
        }
      }
      *out = Join(paths);
      return true;
    }
    case ValueKind::kUnknown:
      break;
  }
  return false;
}

/// The value `target` set, read back off the config the new text parses to.
bool ReadsBackAs(const Config& config, const Resolved& target) {
  switch (target.kind) {
    case ValueKind::kUrl:
      return config.server.url == target.value;
    case ValueKind::kMinutes:
      return std::to_string(config.sync.interval_min) == target.value;
    case ValueKind::kBool: {
      const bool* field = BoolField(config, target.section, target.key);
      return field != nullptr && *field == (target.value == "true");
    }
    case ValueKind::kPathList: {
      const std::vector<std::string>* field = PathField(config, target.section, target.key);
      if (field == nullptr) {
        return false;
      }
      return Join(*field) == target.value;
    }
    case ValueKind::kUnknown:
      break;
  }
  return false;
}

/// The lines a section that did not exist starts life with.
///
/// For anything but a platform that is the one assignment. For a platform this
/// build maps by default it is the **whole built-in mapping with the assignment
/// applied on top**, and that is not tidiness: a `[platform.x]` section replaces
/// that platform's defaults rather than adding to them (docs/CONFIG.md), so a
/// section created to point `saves` somewhere would otherwise unmap the
/// platform's `roms` folder as a side effect of one edit. Precedence is
/// defaults, then the file, then the overlay's change -- which means the change
/// lands on what was in force, not on nothing.
///
/// Emptying a key on purpose is still `roms =`, the documented way to map a
/// platform nowhere; that is an assignment with an empty value and reaches here
/// as one.
std::vector<std::pair<std::string, std::string>> NewSectionBody(const Resolved& target,
                                                                bool* carried) {
  std::vector<std::pair<std::string, std::string>> body;
  const auto found = target.kind == ValueKind::kPathList
                         ? DefaultPlatforms().find(
                               target.section.substr(std::string_view("platform.").size()))
                         : DefaultPlatforms().end();
  if (found == DefaultPlatforms().end()) {
    body.push_back({target.key, target.value});
    return body;
  }
  const PlatformFolders& folders = found->second;
  const std::pair<const char*, const std::vector<std::string>*> keys[] = {
      {"roms", &folders.roms}, {"saves", &folders.saves}, {"states", &folders.states}};
  for (const auto& [name, paths] : keys) {
    if (target.key == name) {
      body.push_back({target.key, target.value});
    } else if (!paths->empty()) {
      body.push_back({name, Join(*paths)});
      *carried = true;
    }
  }
  return body;
}

}  // namespace

bool ApplyEdit(std::string_view current_text, const Edit& edit, std::string* out_text,
               std::vector<Diagnostic>* diagnostics) {
  if (edit.assignments.empty()) {
    Complain(diagnostics, Severity::kError, "", "", "an edit that changes nothing was refused");
    return false;
  }
  if (edit.assignments.size() > kMaxEditAssignments) {
    Complain(diagnostics, Severity::kError, "", "",
             "an edit may set at most " + std::to_string(kMaxEditAssignments) + " values at once");
    return false;
  }
  if (current_text.size() > kMaxConfigBytes) {
    Complain(diagnostics, Severity::kError, "", "",
             "the configuration on the card is larger than the " +
                 std::to_string(kMaxConfigBytes) + " bytes one can be; it was not edited");
    return false;
  }

  // --- resolve and validate the whole edit before a byte of it is applied ---
  //
  // A unit, so a settings screen that sends four values and gets one of them
  // wrong does not leave three of them on the card and the fourth not.
  std::vector<Resolved> resolved;
  resolved.reserve(edit.assignments.size());
  std::set<std::string> seen;
  for (const Assignment& assignment : edit.assignments) {
    Resolved target;
    target.section = CanonicalSection(assignment.section);
    target.key = LowerAscii(Trim(assignment.key));
    target.remove = assignment.remove;
    if (target.section.empty() || target.key.empty()) {
      Complain(diagnostics, Severity::kError, assignment.section, assignment.key,
               "names no section and key to set");
      return false;
    }
    target.kind = KindOf(target.section, target.key);
    if (target.kind == ValueKind::kUnknown) {
      Complain(diagnostics, Severity::kError, target.section, target.key,
               "is not a setting this client has; nothing was changed");
      return false;
    }
    if (!seen.insert(target.section + "\x1f" + target.key).second) {
      // Two values for one key in one edit has no right answer -- taking the
      // last would make the overlay's field order decide the outcome.
      Complain(diagnostics, Severity::kError, target.section, target.key,
               "is set twice in one edit; nothing was changed");
      return false;
    }
    if (!target.remove) {
      if (!NormalizeValue(target, assignment.value, &target.value, diagnostics)) {
        return false;
      }
      if (!SurvivesReadBack(target.value)) {
        Complain(diagnostics, Severity::kError, target.section, target.key,
                 "cannot be written to config.ini: a ';' or a '#' after a space would be read "
                 "back as a comment");
        return false;
      }
    }
    resolved.push_back(std::move(target));
  }

  // --- apply ---
  Document doc = Split(current_text);
  for (const Resolved& target : resolved) {
    const std::vector<std::size_t> hits = Assignments(doc, target.section, target.key);
    if (target.remove) {
      // Every occurrence, not the last: leaving an earlier one in place would
      // hand the value straight back on the next boot.
      for (std::size_t i = hits.size(); i > 0; --i) {
        Erase(&doc, hits[i - 1]);
      }
      continue;
    }
    if (!hits.empty()) {
      // The last, because that is the one `ParseConfig` resolves to. Rewriting
      // any earlier one would leave the edit shadowed by a line further down.
      Line& line = doc.lines[hits.back()];
      line.text = Rewritten(line.text, target.value);
      continue;
    }
    std::size_t at = 0;
    if (InsertPoint(doc, target.section, &at)) {
      Insert(&doc, at, NewAssignment(target.key, target.value));
      continue;
    }
    bool carried = false;
    const std::vector<std::pair<std::string, std::string>> body =
        NewSectionBody(target, &carried);
    AppendSection(&doc, target.section, body);
    if (carried) {
      Complain(diagnostics, Severity::kNotice, target.section, target.key,
               "a platform section replaces this build's default folders for that platform, so "
               "the ones it already had were written out beside your change");
    }
  }

  std::string written = doc.Render();
  if (written.size() > kMaxConfigBytes) {
    Complain(diagnostics, Severity::kError, "", "",
             "the edit would make config.ini larger than the " +
                 std::to_string(kMaxConfigBytes) + " bytes one can be");
    return false;
  }

  // --- and prove it says what it was asked to say ---
  //
  // The invariant this module exists for is that the file on the card is what
  // the engine loads back, and the cheapest honest way to hold it is to load it
  // back. Every rule above is a reason this cannot fail; this is what makes that
  // a fact rather than a claim, and it costs one parse of a few hundred bytes on
  // a command a user presses by hand.
  const Config reparsed = ParseConfig(written).value;
  const Document check = Split(written);
  for (const Resolved& target : resolved) {
    const bool held = target.remove ? Assignments(check, target.section, target.key).empty()
                                    : ReadsBackAs(reparsed, target);
    if (!held) {
      Complain(diagnostics, Severity::kError, target.section, target.key,
               "could not be written to config.ini in a form that reads back the same; "
               "nothing was changed");
      return false;
    }
  }

  *out_text = std::move(written);
  return true;
}

}  // namespace rommsync::config

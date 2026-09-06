// The little text rules `config.ini` is read *and written* by.
//
// These were `config.cpp`'s own until M5-3 (#30) added a writer. They are here
// rather than duplicated because every one of them is a rule about the file
// format itself, and a value written under one copy of `StripInlineComment` and
// read back under another is a `config.ini` that silently means something else
// -- a folder called `disc#2` surviving one and not the other, say.
//
// Internal to the config module: nothing outside it needs these, and
// `config.hpp` is what the module is used through. It lives here rather than
// beside the two `.cpp` files that include it because `core/` includes only
// standard headers and `rommsync/` ones -- the rule `core/AGENTS.md` states and
// the `static` CI job greps for -- so a private header under `core/src` is not
// a shape this project has.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rommsync::config::text {

/// Horizontal whitespace. `\n` is deliberately absent: lines are split first,
/// and a `\r` left on the end of one by a CRLF file is whitespace here.
inline bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

inline std::string_view Trim(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() && IsSpace(text[begin])) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && IsSpace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

inline std::string LowerAscii(std::string_view text) {
  std::string lowered(text);
  for (char& c : lowered) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return lowered;
}

/// Everything before the first `;` or `#` that starts a comment.
///
/// A comment marker counts only at the start of the region or after
/// whitespace, so `https://romm.lan/#anchor` and a folder called `disc#2`
/// survive -- a rule that costs nothing and removes the one way this parser
/// could silently truncate a value a user typed deliberately.
inline std::string_view StripInlineComment(std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != ';' && text[i] != '#') {
      continue;
    }
    if (i == 0 || IsSpace(text[i - 1])) {
      return text.substr(0, i);
    }
  }
  return text;
}

/// `text` with any `user:password@` in a URL replaced by `***@`.
///
/// Every diagnostic that quotes a line the parser could not use goes through
/// this. `ApplyServer` is careful never to echo a URL, but a user who writes
/// `url: https://me:hunter2@romm.lan` -- a colon instead of an equals, the most
/// ordinary slip there is -- never reaches `ApplyServer` at all: the line is not
/// a `key = value` and is reported as one. Quoting it is what makes that report
/// useful, so the credential is removed rather than the quote.
inline std::string Redact(std::string_view text) {
  std::string redacted;
  std::size_t begin = 0;
  while (true) {
    const std::size_t scheme = text.find("://", begin);
    if (scheme == std::string_view::npos) {
      redacted.append(text.substr(begin));
      return redacted;
    }
    const std::size_t authority = scheme + 3;
    std::size_t end = text.find('/', authority);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::size_t at = text.substr(authority, end - authority).find('@');
    redacted.append(text.substr(begin, authority - begin));
    if (at == std::string_view::npos) {
      begin = authority;
      continue;
    }
    redacted.append("***");
    begin = authority + at;  // keep the '@' itself, so the shape is still legible
  }
}

/// `text` as an `int`, refusing anything that is not exactly one integer.
///
/// Hand-rolled rather than `strtol`, because `strtol` reads `30 minutes` as 30
/// and saturates an out-of-range value onto `LONG_MAX` -- both of which turn a
/// typo into a plausible number instead of a message. Overflow is detected
/// against the bound rather than by wrapping.
inline bool ParseInt(std::string_view text, int* out) {
  if (text.empty()) {
    return false;
  }
  std::size_t i = 0;
  bool negative = false;
  if (text[i] == '+' || text[i] == '-') {
    negative = text[i] == '-';
    ++i;
  }
  if (i == text.size()) {
    return false;
  }
  long long value = 0;
  // Every integer in this file is a count of minutes, so a billion is already
  // absurd -- and the bound has to leave the result inside an `int`, since
  // saturating onto something that merely fits is the behaviour this refuses.
  const long long value_bound = 1000000000LL;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    value = value * 10 + (text[i] - '0');
    if (value > value_bound) {
      return false;
    }
  }
  *out = static_cast<int>(negative ? -value : value);
  return true;
}

/// Split a comma-separated list, keeping the pieces untrimmed.
inline std::vector<std::string_view> SplitList(std::string_view text) {
  std::vector<std::string_view> pieces;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    if (comma == std::string_view::npos) {
      pieces.push_back(text.substr(begin));
      return pieces;
    }
    pieces.push_back(text.substr(begin, comma - begin));
    begin = comma + 1;
  }
}

}  // namespace rommsync::config::text

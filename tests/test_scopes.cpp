// The scopes this client asks for are exactly the documented minimum.
//
// A token's blast radius on an SD card anything can read *is* its scope list
// (docs/SECURITY.md), so "least privilege" has to be a checked fact rather than
// an intention in a comment. The check is against docs/API_CONTRACT.md rather
// than against a second copy of the list here: a test that restated the scopes
// would go green while the document said something else, and the document is
// what a reviewer reads.
//
// `me.write` is the case worth having a test for. It is in the document,
// qualified with a `#` comment, and this client calls nothing it guards -- so it
// must be granted to nobody. A scope that is requested and never used is blast
// radius bought for nothing.
//
// It used to be qualified "only if recording play sessions", which was wrong on
// both counts: `me.write` guards `PUT /api/users/{id}`, the client-token family
// and device approve/deny, and M7-4 (#39) records play sessions through
// `roms.user.write` and `devices.write`, both of which are unconditional above.
// The test does not read the comment's text -- only that a qualified scope
// exists and is not requested -- so a corrected reason keeps it green.
//
// Pure file reading and string comparison: no rig, so it never skips.
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/pairing.hpp"

namespace auth = rommsync::auth;

namespace {

/// The scopes block of docs/API_CONTRACT.md, split into the ones that are
/// unconditional and the ones the document qualifies with a `#` comment.
struct Documented {
  std::vector<std::string> required;
  std::vector<std::string> conditional;
  bool found = false;
};

Documented ReadDocumentedScopes(const std::string& path) {
  Documented documented;
  std::ifstream in(path);
  std::string line;
  bool in_section = false;
  bool in_block = false;
  while (std::getline(in, line)) {
    if (line.rfind("### Scopes to request", 0) == 0) {
      in_section = true;
      continue;
    }
    if (!in_section) {
      continue;
    }
    if (line.rfind("```", 0) == 0) {
      if (in_block) {
        break;  // the end of the block, and of anything worth reading
      }
      in_block = true;
      documented.found = true;
      continue;
    }
    if (!in_block) {
      // A heading before the fence means the section has no block at all, which
      // is a broken document rather than an empty scope list.
      if (line.rfind("#", 0) == 0) {
        break;
      }
      continue;
    }

    const std::string::size_type comment = line.find('#');
    const bool qualified = comment != std::string::npos;
    std::string scopes = qualified ? line.substr(0, comment) : line;

    std::string word;
    for (const char letter : scopes + " ") {
      if (letter == ' ' || letter == '\t' || letter == '\r') {
        if (!word.empty()) {
          (qualified ? documented.conditional : documented.required).push_back(word);
          word.clear();
        }
        continue;
      }
      word.push_back(letter);
    }
  }
  return documented;
}

std::vector<std::string> Sorted(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return values;
}

std::string Join(const std::vector<std::string>& values) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) {
      out += " ";
    }
    out += value;
  }
  return out;
}

}  // namespace

int main() {
  checks::Checks c;

  const Documented documented = ReadDocumentedScopes(ROMMSYNC_API_CONTRACT);
  c.Expect(documented.found,
           std::string("the scopes block is where the test expects it: ") + ROMMSYNC_API_CONTRACT);
  c.Expect(!documented.required.empty(), "and it lists scopes");

  const std::vector<std::string> requested = auth::MinimumScopes();
  c.ExpectEq(Join(Sorted(requested)), Join(Sorted(documented.required)),
             "the requested scopes are exactly the documented minimum");

  // The qualified ones are the point of the exercise: documented, and not for
  // this client.
  c.Expect(!documented.conditional.empty(),
           "the document still qualifies at least one scope -- otherwise this test checks nothing");
  for (const std::string& conditional : documented.conditional) {
    c.Expect(std::find(requested.begin(), requested.end(), conditional) == requested.end(),
             "a conditional scope is not requested: " + conditional);
  }

  // A list that repeats a scope, or carries a blank one, is a request body RomM
  // reads differently from the way this file reads.
  std::vector<std::string> unique = Sorted(requested);
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  c.ExpectEq(unique.size(), requested.size(), "no scope is requested twice");
  for (const std::string& scope : requested) {
    c.Expect(!scope.empty(), "no scope is blank");
  }

  // The default is the minimum. A `PairingConfig` that someone forgot to fill
  // in must ask for least privilege, not for nothing and not for everything.
  const auth::PairingConfig config;
  c.ExpectEq(Join(config.requested_scopes), Join(requested),
             "a default PairingConfig requests the minimum");

  return c.failures() == 0 ? 0 : 1;
}

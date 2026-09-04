// The assertion harness the C++ tests share.
//
// Deliberately not a framework: a test here is a `main` that counts failures
// and returns non-zero, so CTest is the only dependency and a red run prints
// the reason rather than a stack of matcher templates.
#pragma once

#include <iostream>
#include <string>
#include <string_view>

namespace checks {

class Checks {
 public:
  void Expect(bool condition, std::string_view what) {
    if (!condition) {
      Fail(what);
    }
  }

  template <typename T, typename U>
  void ExpectEq(const T& actual, const U& expected, std::string_view what) {
    if (!(actual == expected)) {
      std::cerr << "  FAIL: " << what << " -- expected " << expected << ", got " << actual
                << "\n";
      ++failures_;
    }
  }

  int failures() const { return failures_; }

 protected:
  void Fail(std::string_view what) {
    std::cerr << "  FAIL: " << what << "\n";
    ++failures_;
  }

 private:
  int failures_ = 0;
};

}  // namespace checks

// See boot_wait.hpp for why the boot policy is here rather than in main.cpp.
#include "boot_wait.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

namespace rommsync::sysmodule::boot {

Outcome WaitFor(const char* service, Waiter& waiter, Policy policy) {
  Outcome outcome;
  while (true) {
    ++outcome.polls;
    if (waiter.Ready(service)) {
      outcome.ready = true;
      return outcome;
    }
    // Not "no": "no answer". Nothing is learned by asking again.
    if (!waiter.Probed()) return outcome;
    if (outcome.waited + policy.interval > policy.budget) return outcome;
    waiter.Sleep(policy.interval);
    outcome.waited += policy.interval;
  }
}

namespace {

/// The next line's storage, or nullptr when the journal is full -- in which case
/// the drop is counted, because a note nobody knows was lost is worse than one
/// that says so.
char* Claim(Journal& journal) {
  if (journal.count >= kMaxNotes) {
    ++journal.dropped;
    return nullptr;
  }
  return journal.notes[journal.count++];
}

}  // namespace

std::size_t Append(char* line, std::size_t capacity, std::size_t at, std::string_view text) {
  if (capacity == 0) return 0;
  at = std::min(at, capacity - 1);
  const std::size_t taken = std::min(text.size(), capacity - 1 - at);
  std::memcpy(line + at, text.data(), taken);
  line[at + taken] = '\0';
  return at + taken;
}

std::size_t AppendNumber(char* line, std::size_t capacity, std::size_t at,
                         std::uint64_t value, int base) {
  char digits[20];
  const std::to_chars_result written = std::to_chars(digits, digits + sizeof(digits), value, base);
  return Append(line, capacity, at,
                std::string_view(digits, static_cast<std::size_t>(written.ptr - digits)));
}

void Note(Journal& journal, const char* what) {
  char* line = Claim(journal);
  if (line == nullptr) return;
  Append(line, kMaxNoteBytes, 0, what);
}

void Note(Journal& journal, const char* what, std::uint32_t code) {
  char* line = Claim(journal);
  if (line == nullptr) return;
  std::size_t at = Append(line, kMaxNoteBytes, 0, what);
  at = Append(line, kMaxNoteBytes, at, ": 0x");
  AppendNumber(line, kMaxNoteBytes, at, code, 16);
}

const char* NoteAt(const Journal& journal, std::size_t index) {
  return index < journal.count ? journal.notes[index] : nullptr;
}

}  // namespace rommsync::sysmodule::boot

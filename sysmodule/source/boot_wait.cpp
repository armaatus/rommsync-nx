// See boot_wait.hpp for why the boot policy is here rather than in main.cpp.
#include "boot_wait.hpp"

#include <cstdio>
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

void Note(Journal& journal, const char* what) {
  char* line = Claim(journal);
  if (line == nullptr) return;
  std::snprintf(line, kMaxNoteBytes, "%s", what);
}

void Note(Journal& journal, const char* what, std::uint32_t code) {
  char* line = Claim(journal);
  if (line == nullptr) return;
  std::snprintf(line, kMaxNoteBytes, "%s: 0x%x", what, static_cast<unsigned>(code));
}

const char* NoteAt(const Journal& journal, std::size_t index) {
  return index < journal.count ? journal.notes[index] : nullptr;
}

}  // namespace rommsync::sysmodule::boot

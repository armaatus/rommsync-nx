// What the sysmodule's boot path does off a console (M9-1, #195).
//
// Two halves, because #195 is one twenty-line function.
//
// **Name resolution**, driven through `posix_connection.cpp` -- the connector
// the console itself uses for an `http://` origin, not a twin of it
// (tests/tcp_connector.hpp). A `server.url` naming a host takes the
// `getaddrinfo` branch, and that branch is the one an `smExit()` in `__appInit`
// breaks on Horizon: libnx re-opens `sfdnsres` off the `sm` session on every
// call. Nothing off a console has an `sm` session to close, so what runs here is
// the resolution, and `boot.dns` is what holds the session open above it -- the
// two together are the decision #195 asked for, rather than a comment.
//
// **The bounded wait**, `sysmodule/source/boot_wait.cpp`, which names no libnx
// type for exactly this reason: how long `__appInit` may wait for a service, and
// what it leaves behind when it runs out, is the part that can be got wrong, and
// the `Waiter` it asks is an interface so a test can be the `sm` that never
// answers.
//
// One scenario per CTest entry, selected by argv[1], so a failure names the
// behaviour.
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>

#include "boot_wait.hpp"
#include "checks.hpp"
#include "http/posix_connection.hpp"
#include "loopback_server.hpp"

namespace {

namespace boot = rommsync::sysmodule::boot;
namespace http = rommsync::http;
using rommsync::sysmodule::ConnectTo;
using rommsync::sysmodule::Origin;

/// `ConnectTo` reaches a loopback server addressed **by name**.
///
/// The point is the branch, not the connection: `inet_pton` answers a bare IPv4
/// literal without any name service at all, so `127.0.0.1` would prove nothing
/// about the path every `romm.local`, NAS name and DDNS name takes.
///
/// `localhost` and not some other name, because row 8 of the M0 exit gate says
/// no test in this suite reaches off this machine (docs/TESTING.md) -- and a
/// name chosen to *fail* would be the one that leaves: a query for a name that
/// does not resolve is a query that goes to a resolver. `localhost` is answered
/// from the host's own table, and it is the name `policy.loopback_only` already
/// treats as loopback. So the branch is executed and nothing leaves.
int Resolve(checks::Checks& checks) {
  rig::LoopbackServer server;
  if (!server.Start(1, [](int fd, std::size_t, const std::string&) {
        rig::WriteAll(fd, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
      })) {
    std::cerr << "  FAIL: could not start the loopback server\n";
    return 1;
  }

  Origin origin;
  origin.host = "localhost";
  origin.port = server.port();
  origin.tls = false;

  http::Error error = http::Error::kNone;
  std::string message;
  const int fd = ConnectTo(origin, std::chrono::milliseconds(2000),
                           std::chrono::milliseconds(2000), &error, &message);
  checks.Expect(fd >= 0, "connect to `localhost` -- " + message);
  if (fd >= 0) ::close(fd);
  server.Stop();
  return checks.failures();
}

/// An `sm` that answers after `appears_at` questions, and never sleeps: the
/// budget is spent in accounting rather than in wall-clock, so a test of a
/// ten-second bound takes no time at all.
class FakeSm final : public boot::Waiter {
 public:
  FakeSm(unsigned appears_at, bool answers) : appears_at_(appears_at), answers_(answers) {}

  bool Ready(const char*) override { return answers_ && ++asked_ >= appears_at_; }
  bool Probed() const override { return answers_; }
  void Sleep(std::chrono::milliseconds slice) override { slept_ += slice; }

  unsigned asked() const { return asked_; }
  std::chrono::milliseconds slept() const { return slept_; }

 private:
  unsigned appears_at_;
  bool answers_;
  unsigned asked_ = 0;
  std::chrono::milliseconds slept_{0};
};

/// The bound, from both ends: a service that is already there costs nothing, and
/// one that never arrives costs the budget and then stops.
int Wait(checks::Checks& checks) {
  const boot::Policy policy{std::chrono::milliseconds(1000), std::chrono::milliseconds(100)};

  FakeSm present(1, true);
  const boot::Outcome first = boot::WaitFor("fsp-srv", present, policy);
  checks.Expect(first.ready, "a registered service is ready");
  checks.ExpectEq(first.polls, 1u, "a registered service is asked for once");
  checks.ExpectEq(present.slept().count(), 0L, "a registered service costs no sleep");

  FakeSm late(4, true);
  const boot::Outcome then = boot::WaitFor("nifm:u", late, policy);
  checks.Expect(then.ready, "a service that registers during the wait is ready");
  checks.ExpectEq(then.polls, 4u, "...on the poll it appeared");
  checks.ExpectEq(then.waited.count(), 300L, "...having waited three intervals");

  // The whole point. Before #195 this was `smGetService`, which sm parks and
  // never answers, so the process sat here for the life of the console.
  FakeSm absent(0xFFFF, true);
  const boot::Outcome never = boot::WaitFor("ssl", absent, policy);
  checks.Expect(!never.ready, "a service that never registers is not ready");
  checks.Expect(never.waited <= policy.budget, "the wait never exceeds its budget");
  checks.ExpectEq(never.waited.count(), 1000L, "...and spends it");
  checks.ExpectEq(absent.asked(), 11u, "eleven questions in a one-second budget at 100 ms");

  // An `sm` that cannot be asked -- one that is not Atmosphere's, so command
  // 65100 is not there. Waiting out the budget to re-learn that there is no
  // answer helps nobody.
  FakeSm mute(1, false);
  const boot::Outcome unasked = boot::WaitFor("time:s", mute, policy);
  checks.Expect(!unasked.ready, "an sm that cannot answer is not a service that is ready");
  checks.ExpectEq(unasked.polls, 1u, "an unanswerable question is asked once");
  checks.ExpectEq(mute.slept().count(), 0L, "...and waited on not at all");

  return checks.failures();
}

/// What `__appInit` leaves behind when it has nowhere to write yet.
int JournalNotes(checks::Checks& checks) {
  boot::Journal journal{};
  checks.ExpectEq(journal.count, static_cast<std::size_t>(0), "a fresh journal is empty");
  checks.Expect(boot::NoteAt(journal, 0) == nullptr, "an empty journal has no line 0");

  boot::Note(journal, "rommsync: time:s never registered");
  boot::Note(journal, "rommsync: timeInitialize", 0x1015u);
  checks.ExpectEq(journal.count, static_cast<std::size_t>(2), "two notes");
  checks.ExpectEq(std::string(boot::NoteAt(journal, 0)),
                  std::string("rommsync: time:s never registered"), "the first note");
  checks.ExpectEq(std::string(boot::NoteAt(journal, 1)),
                  std::string("rommsync: timeInitialize: 0x1015"),
                  "a result code is written the way a crash report quotes it");

  // Full, and the earliest note is the one that explains the rest -- so the
  // journal keeps the cause and counts the consequences, rather than the other
  // way round.
  for (std::size_t i = journal.count; i < boot::kMaxNotes + 3; ++i) {
    boot::Note(journal, "filler");
  }
  checks.ExpectEq(journal.count, boot::kMaxNotes, "the journal stops at its cap");
  checks.ExpectEq(journal.dropped, static_cast<std::size_t>(3), "and counts what it dropped");
  checks.ExpectEq(std::string(boot::NoteAt(journal, 0)),
                  std::string("rommsync: time:s never registered"),
                  "a full journal still holds the first note");
  checks.Expect(boot::NoteAt(journal, boot::kMaxNotes) == nullptr,
                "nothing past the cap");

  // Longer than a line, because a service name and a message can be. It has to
  // truncate rather than run off the end of the row.
  boot::Journal narrow{};
  std::string long_note(boot::kMaxNoteBytes * 2, 'x');
  boot::Note(narrow, long_note.c_str());
  checks.Expect(std::strlen(boot::NoteAt(narrow, 0)) == boot::kMaxNoteBytes - 1,
                "an over-long note is truncated to the row");

  return checks.failures();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "";
  checks::Checks checks;

  if (scenario == "resolve") return Resolve(checks) == 0 ? 0 : 1;
  if (scenario == "wait") return Wait(checks) == 0 ? 0 : 1;
  if (scenario == "journal") return JournalNotes(checks) == 0 ? 0 : 1;

  std::cerr << "unknown scenario: " << scenario << "\n";
  return 2;
}

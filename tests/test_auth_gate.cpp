// `auth::Gate` — what a rejected call means once you count them.
//
// The whole of M1-4's decision lives here and needs no server: RomM issues no
// refresh token, so there is nothing to exercise a refresh against, and what is
// left is arithmetic over answers. The behaviour a live server *does* have to
// prove -- that one 401 mid-flow is followed by the same token being accepted --
// is `harness.expired`, and it is the reason this file counts at all.
//
//   answers  -- a status becomes an `Answer`, and every error enum agrees
//   counts   -- three consecutive rejections is a verdict; two is not
//   backoff  -- the wait doubles, caps, and is zero while nothing is wrong
//   persists -- the verdict round-trips through `auth.json`, and is cleared
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "checks.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/auth_gate.hpp"
#include "rommsync/device_registration.hpp"
#include "rommsync/download.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/sync_execute.hpp"

namespace {

namespace auth = rommsync::auth;
namespace download = rommsync::download;
namespace http = rommsync::http;
namespace io = rommsync::io;
namespace sync = rommsync::sync;

using auth::Answer;
using auth::Block;

std::filesystem::path ScratchDir() {
  const std::filesystem::path dir = std::filesystem::path(ROMMSYNC_TEST_SCRATCH) / "auth_gate";
  std::filesystem::create_directories(dir);
  return dir;
}

/// A completed exchange that answered `status`.
http::Result Answered(int status) {
  http::Result result;
  result.error = http::Error::kNone;
  result.response.status = status;
  return result;
}

/// An exchange that never completed.
http::Result Failed(http::Error error) {
  http::Result result;
  result.error = error;
  return result;
}

// --- answers ------------------------------------------------------------------
//
// The one place a status is read for what it says about the credentials. Every
// other call site holds an error enum instead, and each of those has to agree
// with this -- a 403 that arrives as `kRejected` from one path and `kForbidden`
// from another is a console told two different things about the same pairing.

void Answers(checks::Checks& c) {
  c.Expect(auth::AnswerOf(Answered(200)) == Answer::kAccepted, "a 200 accepted the token");
  c.Expect(auth::AnswerOf(Answered(201)) == Answer::kAccepted, "and so did a 201");
  c.Expect(auth::AnswerOf(Answered(401)) == Answer::kRejected, "a 401 rejected it");
  c.Expect(auth::AnswerOf(Answered(403)) == Answer::kForbidden,
           "a 403 is a scope, not a revocation");

  // The three that say nothing. A 429 is the one worth pinning: a rate limiter
  // in front of RomM is exactly the thing that also answers 401, and letting it
  // clear the count would make a proxy that alternates the two un-countable.
  c.Expect(auth::AnswerOf(Answered(404)) == Answer::kSilent, "a 404 says nothing about the token");
  c.Expect(auth::AnswerOf(Answered(429)) == Answer::kSilent, "nor does a 429...");
  c.Expect(auth::AnswerOf(Answered(500)) == Answer::kSilent, "...nor a 500");
  c.Expect(auth::AnswerOf(Failed(http::Error::kTimeout)) == Answer::kSilent,
           "an exchange that never completed judged nothing");
  c.Expect(auth::AnswerOf(Failed(http::Error::kCanceled)) == Answer::kSilent,
           "and neither did one the caller stopped");

  // Every classified error, against the same answers.
  c.Expect(auth::AnswerOf(auth::RegistrationError::kUnauthorized) == Answer::kRejected,
           "registration: a 401 is a rejection");
  c.Expect(auth::AnswerOf(auth::RegistrationError::kForbidden) == Answer::kForbidden,
           "registration: a 403 is not");
  c.Expect(auth::AnswerOf(auth::RegistrationError::kNone) == Answer::kAccepted,
           "registration: a resolved device used the token successfully");
  c.Expect(auth::AnswerOf(auth::RegistrationError::kUnreachable) == Answer::kSilent,
           "registration: an offline console judged nothing");
  c.Expect(auth::AnswerOf(auth::RegistrationError::kSyncDisabled) == Answer::kAccepted,
           "registration: sync switched off came off a device record, so the token was read");
  c.Expect(auth::AnswerOf(auth::RegistrationError::kNoSuchDevice) == Answer::kSilent,
           "registration: a bare 404 is also what a wrong server_url answers");

  c.Expect(sync::AnswerOf(sync::NegotiateError::kUnauthorized) == Answer::kRejected,
           "negotiate: a 401 is a rejection");
  c.Expect(sync::AnswerOf(sync::NegotiateError::kForbidden) == Answer::kForbidden,
           "negotiate: a 403 is not");
  c.Expect(sync::AnswerOf(sync::NegotiateError::kNone) == Answer::kAccepted,
           "negotiate: a plan came back, so the token works");
  c.Expect(sync::AnswerOf(sync::NegotiateError::kServerError) == Answer::kSilent,
           "negotiate: a 5xx judged nothing");
  c.Expect(sync::AnswerOf(sync::NegotiateError::kUnusablePayload) == Answer::kSilent,
           "negotiate: a payload never sent judged nothing");
  c.Expect(sync::AnswerOf(sync::NegotiateError::kSyncDisabled) == Answer::kAccepted,
           "negotiate: RomM's own \"sync is disabled\" is proof it read the token");
  c.Expect(sync::AnswerOf(sync::NegotiateError::kRejected) == Answer::kSilent,
           "negotiate: a bare 4xx is an answer anything in front of RomM gives");

  c.Expect(sync::AnswerOf(sync::CompleteError::kUnauthorized) == Answer::kRejected,
           "complete: a 401 is a rejection");
  c.Expect(sync::AnswerOf(sync::CompleteError::kForbidden) == Answer::kForbidden,
           "complete: a 403 is not");
  c.Expect(sync::AnswerOf(sync::CompleteError::kAlreadyCompleted) == Answer::kAccepted,
           "complete: a session already accounted for was reached with a working token");

  c.Expect(sync::AnswerOf(sync::OperationError::kUnauthorized) == Answer::kRejected,
           "execute: a 401 is a rejection");
  c.Expect(sync::AnswerOf(sync::OperationError::kForbidden) == Answer::kForbidden,
           "execute: a 403 is not");
  c.Expect(sync::AnswerOf(sync::OperationError::kNone) == Answer::kAccepted,
           "execute: an operation that did what the plan asked used the token");
  c.Expect(sync::AnswerOf(sync::OperationError::kUnverified) == Answer::kSilent,
           "execute: bytes that were not the save judged nothing about the token");

  c.Expect(download::AnswerOf(download::DrainOutcome::kUnauthorized) == Answer::kRejected,
           "download: a 401 is a rejection");
  c.Expect(download::AnswerOf(download::DrainOutcome::kForbidden) == Answer::kForbidden,
           "download: a 403 is not");
  c.Expect(download::AnswerOf(download::DrainOutcome::kCompleted) == Answer::kAccepted,
           "download: a drained queue used the token");
  c.Expect(download::AnswerOf(download::DrainOutcome::kDisabled) == Answer::kSilent,
           "download: a drain that made no request judged nothing");
  c.Expect(download::AnswerOf(download::DrainOutcome::kCanceled) == Answer::kSilent,
           "download: nor did one the caller stopped, which can precede every request");
  c.Expect(download::AnswerOf(download::DrainOutcome::kRetryable) == Answer::kSilent,
           "download: nor did a 500 -- a proxy alternating it with 401 must stay countable");
}

// --- counts -------------------------------------------------------------------
//
// The acceptance clause this issue exists for: **a single 401 is not a verdict.**
// `harness.expired` shows a live server answering one and then accepting the
// very same token, and a client that discarded `token.dat` over it would send
// the user to a pairing screen for a proxy having a bad minute.

void Counts(checks::Checks& c) {
  {
    auth::Gate gate;
    gate.Observe(Answer::kRejected);
    c.Expect(!gate.blocked(), "one 401 is not a verdict");
    c.ExpectEq(gate.rejections(), 1, "but it is counted");
    gate.Observe(Answer::kRejected);
    c.Expect(!gate.blocked(), "and neither are two");
    gate.Observe(Answer::kRejected);
    c.Expect(gate.blocked(), "the third consecutive one is");
    c.Expect(gate.block() == Block::kRevoked, "and it is a revocation");
  }

  {
    // The recovery `harness.expired` demonstrates: the token that got the 401
    // still works, so the count starts over.
    auth::Gate gate;
    gate.Observe(Answer::kRejected);
    gate.Observe(Answer::kRejected);
    gate.Observe(Answer::kAccepted);
    c.ExpectEq(gate.rejections(), 0, "an accepted call clears the count");
    gate.Observe(Answer::kRejected);
    gate.Observe(Answer::kRejected);
    c.Expect(!gate.blocked(), "so two more are still only two");
  }

  {
    // The mistake worth its own case: an offline tick is not evidence the token
    // works. Three 401s with a 500 between them are still three 401s.
    auth::Gate gate;
    gate.Observe(Answer::kRejected);
    gate.Observe(Answer::kSilent);
    gate.Observe(Answer::kRejected);
    gate.Observe(auth::AnswerOf(Failed(http::Error::kTimeout)));
    gate.Observe(Answer::kRejected);
    c.Expect(gate.blocked(), "silence neither clears the count nor adds to it");
    c.Expect(gate.block() == Block::kRevoked, "the verdict is still the 401s'");
  }

  {
    // A 403 earns its own sentence. Both send the user back to pairing, and
    // only one of them is true of a token that was revoked.
    auth::Gate gate;
    for (int at = 0; at < 3; ++at) {
      gate.Observe(Answer::kForbidden);
    }
    c.Expect(gate.blocked(), "three 403s is a verdict too");
    c.Expect(gate.block() == Block::kScopeDenied, "...and it is not a revocation");
    c.Expect(std::string(auth::Describe(Block::kScopeDenied)) !=
                 std::string(auth::Describe(Block::kRevoked)),
             "the two say different things to the user");
    c.Expect(std::string(auth::Describe(Block::kScopeDenied)).find("revok") == std::string::npos,
             "and the scope one does not claim a revocation");
  }

  {
    // Once blocked, only pairing again lifts it: a client that is not calling
    // cannot be told its token works.
    auth::Gate gate;
    for (int at = 0; at < 3; ++at) {
      gate.Observe(Answer::kRejected);
    }
    gate.Observe(Answer::kAccepted);
    c.Expect(gate.blocked(), "an acceptance does not lift a verdict");
    gate.Reset();
    c.Expect(!gate.blocked(), "pairing again does");
    c.ExpectEq(gate.rejections(), 0, "and takes the count with it");
  }

  {
    // A run that turns from 401 into 403 ends on what the server is saying now.
    auth::Gate gate;
    gate.Observe(Answer::kRejected);
    gate.Observe(Answer::kRejected);
    gate.Observe(Answer::kForbidden);
    c.Expect(gate.block() == Block::kScopeDenied, "the most recent kind of rejection wins");
  }

  {
    // The budget is a budget: a caller that keeps asking after the verdict must
    // not be able to run the count past it.
    auth::Gate gate;
    for (int at = 0; at < 10; ++at) {
      gate.Observe(Answer::kRejected);
    }
    c.ExpectEq(gate.rejections(), gate.config().max_consecutive_rejections,
               "the count stops at the budget");
  }

  {
    // What a boot does with the file: the verdict is in force before the first
    // request, which is the point of persisting it at all.
    auth::Gate gate;
    gate.Restore(Block::kScopeDenied);
    c.Expect(gate.blocked(), "a restored verdict is in force");
    c.Expect(gate.block() == Block::kScopeDenied, "with the sentence it was stored with");
    gate.Restore(Block::kNone);
    c.Expect(!gate.blocked(), "and restoring nothing is a healthy console");
  }
}

// --- backoff ------------------------------------------------------------------
//
// "Never a tight retry loop — this runs on battery" (the issue). The wait is
// asked for rather than spent, the same way `sync::NegotiateOptions::wait` is
// injected, so this proves the pacing without taking five minutes to do it.

void Backoff(checks::Checks& c) {
  auth::GateConfig config;
  config.max_consecutive_rejections = 5;
  config.backoff = std::chrono::milliseconds{1'000};
  config.max_backoff = std::chrono::milliseconds{4'000};
  auth::Gate gate(config);

  c.Expect(gate.backoff() == std::chrono::milliseconds{0},
           "a console with nothing wrong waits for nothing");
  gate.Observe(Answer::kRejected);
  c.Expect(gate.backoff() == std::chrono::milliseconds{1'000}, "the first rejection waits once");
  gate.Observe(Answer::kRejected);
  c.Expect(gate.backoff() == std::chrono::milliseconds{2'000}, "the second doubles it");
  gate.Observe(Answer::kRejected);
  c.Expect(gate.backoff() == std::chrono::milliseconds{4'000}, "the third doubles it again");
  gate.Observe(Answer::kRejected);
  c.Expect(gate.backoff() == std::chrono::milliseconds{4'000}, "and the fourth is capped");

  gate.Observe(Answer::kAccepted);
  c.Expect(gate.backoff() == std::chrono::milliseconds{0},
           "a token that works again is not something to wait on");

  // The default is measured in tens of seconds, not the second a retry inside
  // one call uses: this is how long before the console asks at all.
  const auth::Gate stock;
  c.Expect(stock.config().backoff >= std::chrono::seconds{10},
           "the default pace is a battery's, not a retry loop's");
  c.Expect(stock.config().max_backoff >= stock.config().backoff,
           "and the cap is not below the first delay");
}

// --- persists -----------------------------------------------------------------

void Persists(checks::Checks& c) {
  const std::filesystem::path dir = ScratchDir();

  {
    const std::string path = (dir / "roundtrip.json").string();
    std::filesystem::remove(path);
    c.Expect(auth::LoadBlock(path).value == Block::kNone,
             "a console that has never been refused has no file");
    c.Expect(auth::LoadBlock(path).diagnostic.empty(), "and nothing to complain about");

    c.Expect(auth::SaveBlock(path, Block::kRevoked).ok(), "the verdict is written");
    const auth::LoadedBlock read = auth::LoadBlock(path);
    c.Expect(read.value == Block::kRevoked, "and read back");
    c.Expect(read.diagnostic.empty(), "cleanly");

    c.Expect(auth::SaveBlock(path, Block::kScopeDenied).ok(), "and it can be replaced");
    c.Expect(auth::LoadBlock(path).value == Block::kScopeDenied, "with the other kind");

    c.Expect(auth::ClearBlock(path), "pairing again clears it");
    c.Expect(auth::LoadBlock(path).value == Block::kNone, "and the console is healthy again");
    c.Expect(!io::Exists(io::PreviousPathFor(path)),
             "including the .old an interrupted commit leaves behind");
  }

  {
    // `kNone` is the absence of the file. Writing one that says "blocked by
    // nothing" would be a console the overlay cannot explain.
    const std::string path = (dir / "none.json").string();
    std::filesystem::remove(path);
    c.Expect(!auth::SaveBlock(path, Block::kNone).ok(), "there is no verdict of `none` to store");
    c.Expect(!io::Exists(path), "and nothing was written");
  }

  {
    // Never a gate on boot. Every one of these costs three requests to
    // re-derive, which is cheaper than a console that will not start.
    const std::string path = (dir / "junk.json").string();
    const auto write = [&path](const std::string& text) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << text;
    };

    write("{\"format\":\"rommsync-auth\",\"version\":1");
    c.Expect(auth::LoadBlock(path).value == Block::kNone, "a truncated file is not a verdict");
    c.Expect(!auth::LoadBlock(path).diagnostic.empty(), "and says so");

    write("{\"format\":\"rommsync-queue\",\"version\":1,\"block\":\"revoked\"}");
    c.Expect(auth::LoadBlock(path).value == Block::kNone, "nor is another file's format");

    write("{\"format\":\"rommsync-auth\",\"version\":2,\"block\":\"revoked\"}");
    c.Expect(auth::LoadBlock(path).value == Block::kNone, "nor a version this build cannot read");

    // The one worth spelling out: a state from a later release must not be
    // honoured as "some kind of blocked". A console stopped for a reason it
    // cannot name is one nobody can get working again from the overlay.
    write("{\"format\":\"rommsync-auth\",\"version\":1,\"block\":\"quarantined\"}");
    const auth::LoadedBlock unknown = auth::LoadBlock(path);
    c.Expect(unknown.value == Block::kNone, "a state this build does not know stops nothing");
    c.Expect(unknown.diagnostic.find("quarantined") != std::string::npos,
             "and names it: " + unknown.diagnostic);

    std::filesystem::remove(path);
  }

  {
    // The commit window: `WriteAtomically`'s two renames leave a moment where
    // the file is only under `.old`, and a boot in that moment must still find
    // the verdict rather than start calling a revoked server again.
    const std::string path = (dir / "commit.json").string();
    std::filesystem::remove(path);
    c.Expect(auth::SaveBlock(path, Block::kRevoked).ok(), "a verdict is on the card");
    std::filesystem::rename(path, io::PreviousPathFor(path));
    c.Expect(auth::LoadBlock(path).value == Block::kRevoked,
             "and is still found while the commit is half done");
    c.Expect(auth::ClearBlock(path), "clearing it takes the .old too");
    c.Expect(auth::LoadBlock(path).value == Block::kNone, "so nothing is left to find");
  }

  {
    // The serialised form is what the reader expects, byte for byte. A writer
    // that can produce a file its own reader discards would cost a console the
    // verdict it just reached.
    c.ExpectEq(auth::SerializeBlock(Block::kRevoked),
               std::string("{\"format\":\"rommsync-auth\",\"version\":1,\"block\":\"revoked\"}"),
               "the file is the shape the docs quote");
    c.Expect(auth::ParseBlock(auth::SerializeBlock(Block::kScopeDenied)).value ==
                 Block::kScopeDenied,
             "and what it writes it reads");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "counts";
  checks::Checks checks;

  if (scenario == "answers") {
    Answers(checks);
  } else if (scenario == "counts") {
    Counts(checks);
  } else if (scenario == "backoff") {
    Backoff(checks);
  } else if (scenario == "persists") {
    Persists(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (checks.failures() != 0) {
    std::cerr << scenario << ": " << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << scenario << ": ok\n";
  return 0;
}

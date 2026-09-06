// What a rejected call means over time: when the pairing is given up on, how
// long the client waits in the meantime, and what a reboot remembers.
//
// RomM issues no refresh token and `expires_at` is `null` on every 5.2.0
// response (docs/AUTH.md#re-pairing--revocation), so nothing here refreshes
// anything: a token does not go stale on its own, and the only remedy for a
// genuine `401` is pairing this console again. What this file adds is the step
// between one rejected call and that remedy.
//
// **One 401 is not a verdict.** The token endpoint aside, a 401 can come from
// something in *front* of RomM -- an authenticating reverse proxy, a gateway
// having a bad minute -- and the token that got it usually still works.
// `harness.expired` shows exactly that against a live server: one 401 mid-flow,
// then the very same token accepted. `sync::NeedsPairing` and
// `auth::NeedsPairing` classify one answer; neither counts, and a client that
// shredded `token.dat` on the first one would send the user to a pairing screen
// for a proxy hiccup. `Gate` is where the counting lives, the way
// `PairingConfig::max_rejected_polls` already does it for the poll loop.
//
// **A 403 is not a revocation.** RomM approves what the *user* ticked, which
// need not be what was requested, so a 403 is a scope missing from a pairing
// that is otherwise working (docs/AUTH.md#scopes-to-request). Both send the user
// back to the pairing flow and neither is retryable, but the sentences differ --
// "your pairing is gone" and "pair again and approve the scopes sync needs" --
// and only the first is true of a 401.
//
// Nothing here makes a request or reads a clock. It is fed answers and asked
// questions, so the scheduler that paces the ticks (M7-2, #37) is the one thing
// that has to own a clock.
#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "rommsync/http.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::auth {

/// What one exchange said about this console's credentials, and nothing else.
///
/// **Only an exchange that carried the bearer token may be observed.** The
/// device-code endpoints take no credentials at all (docs/AUTH.md), so a 200
/// from one of those says nothing about a token and would wrongly clear a count
/// that is two thirds of the way to a verdict.
enum class Answer {
  kAccepted,   ///< a 2xx: the server took the token
  kRejected,   ///< 401 -- revoked, or something in front of RomM having a bad minute
  kForbidden,  ///< 403 -- a scope this pairing was not granted
  /// The exchange said nothing about the credentials: a transport failure, a
  /// 404, a 500, a 429. **Not the same as `kAccepted`** -- an offline tick is no
  /// evidence that the token still works, and three 401s with a 500 between them
  /// are still three 401s.
  kSilent,
};

/// Stable, log-friendly name. Never null.
const char* ToString(Answer answer);

/// Read `result` for what it says about the credentials.
///
/// The "detect a 401 on **any** call" primitive: it takes an `http::Result` and
/// so works for a call whose failure enum nobody has written yet. Callers that
/// already hold a classified error have an `AnswerOf` overload beside that enum
/// -- `auth::AnswerOf(RegistrationError)`, `sync::AnswerOf(NegotiateError)` and
/// the rest -- so a status is read here once rather than at each call site.
Answer AnswerOf(const http::Result& result);

/// Why the client has stopped calling, or `kNone` while it has not.
///
/// Two members rather than a bool because they are two different screens and two
/// different sentences (see the header note). Both are `NeedsPairing`.
enum class Block {
  kNone,
  kRevoked,      ///< enough consecutive 401s: the server has stopped accepting the token
  kScopeDenied,  ///< enough consecutive 403s: the pairing is real and lacks a scope
};

/// Stable, log-friendly name. Never null. Also what `auth.json` stores.
const char* ToString(Block block);

/// The sentence a log or the overlay draws. Never null, empty for `kNone`.
const char* Describe(Block block);

/// How patient the gate is, and how hard it paces what is left.
struct GateConfig {
  /// Consecutive rejections before the pairing is given up on.
  ///
  /// The same budget, for the same reason, as `PairingConfig::max_rejected_polls`:
  /// a blip deserves a retry, a proxy that will answer 401 every time must not be
  /// allowed to keep a console retrying forever, and neither extreme is right.
  /// Three is small enough that a genuinely revoked token costs three requests
  /// and large enough that one bad minute costs nothing.
  int max_consecutive_rejections = 3;

  /// The delay after the *first* rejection, doubled per consecutive one and
  /// capped at `max_backoff`.
  ///
  /// Measured in tens of seconds rather than the second `CallPolicy` uses: this
  /// is not a retry inside one call, it is how long before the client asks
  /// again at all, and it runs on a battery (CLAUDE.md).
  std::chrono::milliseconds backoff{30'000};
  std::chrono::milliseconds max_backoff{300'000};
};

/// The standing of this console's credentials, as the calls that used them
/// leave it.
///
/// Not thread-safe and deliberately not synchronised: a sysmodule has one
/// scheduler, and a mutex here would suggest two workers may share one gate
/// without saying what a caller holding a stale `blocked()` should do.
class Gate {
 public:
  Gate() = default;
  explicit Gate(GateConfig config) : config_(config) {}

  const GateConfig& config() const { return config_; }

  /// Record what one exchange said.
  ///
  /// `kAccepted` clears the count but **not** the block: a blocked client makes
  /// no calls, so an acceptance while blocked came from somewhere that was not
  /// asked about this token, and treating it as a recovery would let the console
  /// slip back to a token the server has stopped accepting. `Reset()` is the
  /// only exit, and re-pairing is what calls it.
  void Observe(Answer answer);

  /// The same, reading the status off `result`. See `AnswerOf`.
  void Observe(const http::Result& result) { Observe(AnswerOf(result)); }

  Block block() const { return block_; }

  /// No further call may be made with these credentials. The remedy is pairing.
  bool blocked() const { return block_ != Block::kNone; }

  /// Consecutive rejections since the last acceptance or reset. Reaches
  /// `config().max_consecutive_rejections` and stops there.
  int rejections() const { return rejections_; }

  /// How long to wait before asking again -- zero when nothing has been
  /// rejected.
  ///
  /// Doubling per consecutive rejection and capped, so a server behind a proxy
  /// that answers 401 for a minute costs three requests spread over that minute
  /// rather than a tight loop on a battery. A gate that is `blocked()` answers
  /// `max_backoff` whatever the doubling would have reached: there is nothing
  /// left to wait for, so it is a pace for whatever loop is still turning and
  /// never a licence to call -- `blocked()` is the question that decides that.
  std::chrono::milliseconds backoff() const;

  /// Adopt a verdict that was already reached -- what a boot does with the one
  /// `auth.json` holds. `kNone` is `Reset()`.
  void Restore(Block block);

  /// Forget everything: no block, no count. What pairing this console again
  /// does, and the only thing that lifts a block.
  void Reset();

 private:
  GateConfig config_{};
  int rejections_ = 0;

  /// Which kind the rejections have been, so the block that follows carries the
  /// right sentence. The most recent one wins: a run that turned from 401 into
  /// 403 ends on the answer the server is giving now.
  Block pending_ = Block::kNone;

  Block block_ = Block::kNone;
};

// --- the verdict, on the card -------------------------------------------------
//
// A file that **exists only while the server has stopped accepting the token**.
// It is what lets the overlay draw "pair this console again" the moment it
// connects, rather than after the engine has spent three more rejected requests
// re-deriving a conclusion the last boot already reached.
//
// It is never a gate on boot: a file that will not read is no verdict at all,
// and the worst it costs is those three requests. That is the opposite call from
// `token.dat`, and rightly -- this file holds nothing that cannot be worked out
// again.

/// The file the verdict lives in, relative to `sdmc:/config/rommsync/`.
///
/// Beside `token.dat` rather than inside it: the token is the one file that
/// cannot be re-fetched without a human at a browser, and a verdict that is
/// re-derivable in three requests has no business being written into it.
inline constexpr const char* kAuthStateFileName = "auth.json";

/// The `format` string and `version` number a well-formed file opens with.
inline constexpr const char* kAuthFormatMagic = "rommsync-auth";
inline constexpr int kAuthFormatVersion = 1;

/// The largest `auth.json` that will be read. One small object; a corrupt
/// directory entry claiming megabytes is a named refusal, not a `bad_alloc`
/// (`download::kMaxQueueBytes`' reasoning, on a file two orders smaller).
inline constexpr std::size_t kMaxAuthStateBytes = 1024;

/// The whole file: `{"format":"rommsync-auth","version":1,"block":"revoked"}`,
/// ending in a newline. `kNone` has no file, so it is not serialisable -- see
/// `SaveBlock`.
std::string SerializeBlock(Block block);

struct LoadedBlock {
  Block value = Block::kNone;

  /// What was wrong with the file, or empty. Never an error: `core/` has no
  /// logger (docs/ARCHITECTURE.md), and every one of these costs three requests
  /// and nothing else.
  std::string diagnostic;
};

/// Parse the contents of an `auth.json`. Pure: no filesystem.
///
/// Anything that is not this format, this version and a `block` this build
/// knows is `kNone` with a diagnostic. A file naming a block from a future
/// release is deliberately *not* honoured as "some kind of blocked": a console
/// that stopped calling for a reason it cannot name is one nobody can get
/// working again from the overlay.
LoadedBlock ParseBlock(std::string_view text);

/// Read `path` and parse it.
///
/// A *missing* file is `kNone` with no diagnostic -- that is what every healthy
/// console looks like. It falls back to `io::PreviousPathFor(path)` first, the
/// same recovery `token_store`, `device_identity`, `config`, `state_db` and
/// `download` make: the one moment the file legitimately does not exist is the
/// window `io::WriteAtomically`'s two-rename commit opens.
LoadedBlock LoadBlock(const std::string& path);

/// Write `block` to `path`, atomically. `kNone` is `kUnusableToken` and writes
/// nothing -- clearing a verdict removes the file, which is `ClearBlock`.
StoreResult SaveBlock(const std::string& path, Block block);

/// Remove the verdict at `path`, and the `.tmp`/`.old` an interrupted commit
/// leaves beside it. True when nothing is left, including when there was
/// nothing to begin with.
///
/// Through `io::Shred`, the same primitive `DiscardToken` uses, although this
/// file names a state and never a credential: the overwrite costs one small
/// write and a second way to remove a file is a second place to forget the
/// `.old` an interrupted commit leaves behind.
bool ClearBlock(const std::string& path);

}  // namespace rommsync::auth

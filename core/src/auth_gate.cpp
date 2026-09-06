#include "rommsync/auth_gate.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "rommsync/atomic_file.hpp"
#include "rommsync/json.hpp"

namespace rommsync::auth {

const char* ToString(Answer answer) {
  switch (answer) {
    case Answer::kAccepted:
      return "accepted";
    case Answer::kRejected:
      return "rejected";
    case Answer::kForbidden:
      return "forbidden";
    case Answer::kSilent:
      return "silent";
  }
  return "silent";
}

Answer AnswerOf(const http::Result& result) {
  if (!result.ok()) {
    // A transport failure never reached the point where credentials were
    // judged. Reading it as anything else would let an offline console either
    // clear a count or walk into a verdict.
    return Answer::kSilent;
  }
  const int status = result.response.status;
  if (status == 401) {
    return Answer::kRejected;
  }
  if (status == 403) {
    return Answer::kForbidden;
  }
  if (result.successful()) {
    return Answer::kAccepted;
  }
  // Every other status -- a 404, a 422, a 429, a 5xx -- is the server answering
  // about the *request*, having already accepted the token to get that far. It
  // is not evidence either way, and in particular a 429 must not clear a count:
  // a rate limiter in front of RomM is exactly the thing that also answers 401.
  return Answer::kSilent;
}

const char* ToString(Block block) {
  switch (block) {
    case Block::kNone:
      return "none";
    case Block::kRevoked:
      return "revoked";
    case Block::kScopeDenied:
      return "scope_denied";
  }
  return "none";
}

const char* Describe(Block block) {
  switch (block) {
    case Block::kNone:
      return "";
    case Block::kRevoked:
      return "the server has stopped accepting this console's token; pair this console again";
    case Block::kScopeDenied:
      return "this pairing was not granted the scopes it needs; pair this console again and "
             "approve them";
  }
  return "";
}

void Gate::Observe(Answer answer) {
  switch (answer) {
    case Answer::kSilent:
      // Deliberately nothing: see `Answer::kSilent`.
      return;
    case Answer::kAccepted:
      rejections_ = 0;
      pending_ = Block::kNone;
      return;
    case Answer::kRejected:
      pending_ = Block::kRevoked;
      break;
    case Answer::kForbidden:
      pending_ = Block::kScopeDenied;
      break;
  }

  if (blocked()) {
    // The verdict is already in, and a caller that kept asking anyway must not
    // be able to run the count up past the budget it is compared against.
    return;
  }
  ++rejections_;
  if (rejections_ >= config_.max_consecutive_rejections) {
    block_ = pending_;
  }
}

std::chrono::milliseconds Gate::backoff() const {
  if (rejections_ <= 0) {
    return std::chrono::milliseconds{0};
  }
  std::chrono::milliseconds delay = config_.backoff;
  // Doubled per rejection past the first, by repeated addition rather than a
  // shift: `rejections_` is bounded by the budget, and a shift wide enough to
  // overflow a `rep` is the one way a backoff becomes a negative wait.
  for (int doubled = 1; doubled < rejections_; ++doubled) {
    if (delay >= config_.max_backoff) {
      break;
    }
    delay += delay;
  }
  return delay < config_.max_backoff ? delay : config_.max_backoff;
}

void Gate::Restore(Block block) {
  if (block == Block::kNone) {
    Reset();
    return;
  }
  block_ = block;
  pending_ = block;
  // The count that produced it is not in the file and does not need to be: a
  // blocked gate is asked `blocked()`, and the budget has already been spent.
  rejections_ = config_.max_consecutive_rejections;
}

void Gate::Reset() {
  rejections_ = 0;
  pending_ = Block::kNone;
  block_ = Block::kNone;
}

// --- the verdict, on the card -------------------------------------------------

std::string SerializeBlock(Block block) {
  std::string out("{\"format\":");
  out += json::Quote(kAuthFormatMagic);
  out += ",\"version\":";
  out += std::to_string(kAuthFormatVersion);
  out += ",\"block\":";
  out += json::Quote(ToString(block));
  out += "}";
  return out;
}

LoadedBlock ParseBlock(std::string_view text) {
  LoadedBlock loaded;
  const json::ParseResult document = json::Parse(text);
  if (!document.ok()) {
    loaded.diagnostic = "auth state: " + document.error.Describe();
    return loaded;
  }

  std::string format;
  std::int64_t version = 0;
  std::string block;
  json::Reader reader(document.value, "auth state");
  reader.Required("format", &format);
  reader.Required("version", &version);
  reader.Required("block", &block);
  if (!reader.ok()) {
    loaded.diagnostic = "auth state: " + reader.error().Describe();
    return loaded;
  }
  if (format != kAuthFormatMagic || version != kAuthFormatVersion) {
    loaded.diagnostic = "auth state: not a rommsync-auth v" +
                        std::to_string(kAuthFormatVersion) + " file";
    return loaded;
  }

  // Named rather than switched over, and a name this build does not know is
  // `kNone`: a console stopped for a reason it cannot put on the screen is one
  // nobody can get working again from the overlay. The three requests it costs
  // to re-derive the verdict are cheaper than that.
  if (block == ToString(Block::kRevoked)) {
    loaded.value = Block::kRevoked;
    return loaded;
  }
  if (block == ToString(Block::kScopeDenied)) {
    loaded.value = Block::kScopeDenied;
    return loaded;
  }
  loaded.diagnostic = "auth state: \"" + block + "\" is not a state this build knows";
  return loaded;
}

LoadedBlock LoadBlock(const std::string& path) {
  std::string text;
  io::BoundedRead outcome = io::ReadBounded(path, kMaxAuthStateBytes, &text);
  if (outcome == io::BoundedRead::kMissing) {
    // The commit window, and the only moment this file legitimately vanishes.
    // A *missing* `.old` beside a missing `path` is the ordinary healthy
    // console, which is why neither is a diagnostic.
    outcome = io::ReadBounded(io::PreviousPathFor(path), kMaxAuthStateBytes, &text);
    if (outcome == io::BoundedRead::kMissing) {
      return {};
    }
  }
  if (outcome != io::BoundedRead::kOk) {
    LoadedBlock loaded;
    loaded.diagnostic = path + ": the stored auth state could not be read (" +
                        io::ToString(outcome) + "); this console will find out by asking";
    return loaded;
  }
  return ParseBlock(text);
}

StoreResult SaveBlock(const std::string& path, Block block) {
  if (block == Block::kNone) {
    // Not an oversight in the format: "nothing is wrong" is the absence of the
    // file, so a caller that meant to lift a verdict wants `ClearBlock` and a
    // caller that reached here by accident would otherwise write a file saying
    // the console is blocked by nothing.
    return {StoreError::kUnusableToken,
            path + ": there is no verdict to store; use ClearBlock to lift one"};
  }
  const io::WriteResult written = io::WriteAtomically(path, SerializeBlock(block) + "\n");
  switch (written.error) {
    case io::WriteError::kNone:
      return {};
    case io::WriteError::kOpenFailed:
      return {StoreError::kOpenFailed, written.message};
    case io::WriteError::kWriteFailed:
      return {StoreError::kWriteFailed, written.message};
    case io::WriteError::kCommitFailed:
      return {StoreError::kCommitFailed, written.message};
  }
  return {StoreError::kCommitFailed, written.message};
}

bool ClearBlock(const std::string& path) { return io::Shred(path); }

}  // namespace rommsync::auth

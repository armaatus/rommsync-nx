#include "rommsync/backoff.hpp"

#include <algorithm>
#include <limits>

namespace rommsync::retry {
namespace {

/// The seed for the built-in generator. See `Backoff::state_` for why this is
/// enough and why nothing stronger is wanted here.
std::uint64_t SeedFromClock() {
  const auto ticks =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  // xorshift64* is degenerate on a zero state, and a clock that answers zero is
  // exactly the kind of thing a fresh console does.
  return ticks == 0 ? 0x9E3779B97F4A7C15ull : ticks;
}

/// xorshift64*, which is eight bytes of state and four operations.
std::uint64_t NextBits(std::uint64_t* state) {
  std::uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

}  // namespace

Backoff::Backoff(BackoffConfig config, Random random) : random_(std::move(random)) {
  Configure(config);
}

void Backoff::Configure(const BackoffConfig& config) {
  config_ = config;
  if (config_.floor < std::chrono::milliseconds::zero()) {
    config_.floor = std::chrono::milliseconds::zero();
  }
  if (config_.cap < config_.floor) {
    // A cap under the floor is a configuration that cannot be honoured: the
    // floor is the promise ("never sooner than this") and the cap is the
    // courtesy. Reading it as the floor keeps the promise.
    config_.cap = config_.floor;
  }
  config_.jitter = std::clamp(config_.jitter, 0.0, 1.0);
}

std::chrono::milliseconds Backoff::Base(int failures) const {
  if (failures <= 0) {
    return std::chrono::milliseconds::zero();
  }
  std::chrono::milliseconds base = config_.floor;
  for (int step = 1; step < failures && base < config_.cap; ++step) {
    base *= 2;
  }
  return base > config_.cap ? config_.cap : base;
}

double Backoff::Roll() {
  if (random_) {
    return std::clamp(random_(), 0.0, 1.0);
  }
  if (state_ == 0) {
    state_ = SeedFromClock();
  }
  // 53 bits, which is every bit a double can hold without rounding, over 2^53.
  // Anything cruder shows up as a spread with visible steps in it.
  constexpr std::uint64_t kMantissa = 1ull << 53;
  return static_cast<double>(NextBits(&state_) >> 11) / static_cast<double>(kMantissa);
}

std::chrono::milliseconds Backoff::Fail() {
  if (failures_ < std::numeric_limits<int>::max()) {
    ++failures_;
  }
  const std::chrono::milliseconds base = Base(failures_);
  // Upward, so the floor is a floor. See the header for why the cap does not
  // clamp this back down.
  const auto extra = static_cast<std::chrono::milliseconds::rep>(
      static_cast<double>(base.count()) * config_.jitter * Roll());
  delay_ = base + std::chrono::milliseconds{extra};
  return delay_;
}

void Backoff::Succeed() {
  failures_ = 0;
  delay_ = std::chrono::milliseconds::zero();
}

}  // namespace rommsync::retry

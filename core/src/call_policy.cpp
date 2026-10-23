// `sync::CallPolicy`'s retry curve: `retry::Backoff`, waited out the way the
// policy says. Its own file so that no one call's source owns the rule.
#include <chrono>
#include <thread>

#include "rommsync/backoff.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::sync {

retry::Backoff BackoffFor(const CallPolicy& policy) {
  retry::BackoffConfig config;
  // The cap wins over the floor here, where `retry::Backoff` reads it the other
  // way round: `max_backoff` is a ceiling a caller of one call sets on purpose,
  // and every one of these loops honoured it before the first wait.
  config.floor = policy.backoff > policy.max_backoff ? policy.max_backoff : policy.backoff;
  config.cap = policy.max_backoff;
  config.jitter = policy.jitter;
  return retry::Backoff(config, policy.random);
}

std::chrono::milliseconds WaitToRetry(const CallPolicy& policy, retry::Backoff* backoff) {
  const std::chrono::milliseconds delay = backoff->Fail();
  if (policy.wait != nullptr) {
    policy.wait(delay);
  } else {
    std::this_thread::sleep_for(delay);
  }
  return delay;
}

}  // namespace rommsync::sync

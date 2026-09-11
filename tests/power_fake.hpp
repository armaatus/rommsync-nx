// A `power::Module` a test drives one request at a time (M9-4, #208).
//
// PSC is a service, so the console's `power::Module` cannot be reached from a
// laptop -- but everything the module is *for* can: the order of the calls, the
// promise that each request is answered exactly once, and, in `test_engine`,
// that the answer waits for the save write in flight. This is the half that
// stands in for `psc:m` in both.
//
// Two binaries use it -- `test_power` drives the state machine and
// `test_engine.sleeps` drives a whole `SdEngine` through it -- which is why it
// is a header beside them rather than a class inside one.
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "power.hpp"

namespace power_fake {

namespace power = rommsync::sysmodule::power;

/// PSC, as a queue the test pushes into.
///
/// `Deliver` is one request; `AwaitAck` is the answer coming back. Both are
/// blocking-with-a-budget rather than sleeps, so a scenario that hangs fails on
/// the assertion that names the promise instead of on a CTest timeout that names
/// nothing.
class Scripted final : public power::Module {
 public:
  bool NextRequest(power::State* state) override {
    std::unique_lock<std::mutex> lock(mutex_);
    delivered_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
    if (pending_.empty()) {
      return false;
    }
    *state = pending_.front();
    pending_.pop_front();
    return true;
  }

  bool Acknowledge(power::State state) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      acknowledged_.push_back(state);
    }
    acked_.notify_all();
    return true;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    delivered_.notify_all();
  }

  /// Hand the watcher one request.
  ///
  /// **A queue and not a slot**, so a scenario can put the whole sequence in
  /// before the watcher has picked any of it up. PSC itself does not: it waits
  /// for the acknowledgement before it sends the next. `test_power` pins the
  /// answers in order, which is what makes the queue safe here -- the watcher
  /// still handles them one at a time.
  void Deliver(power::State state) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(state);
    }
    delivered_.notify_all();
  }

  /// Wait for the acknowledgement count to reach `wanted`. False when it did
  /// not, which is the interesting answer as often as the true one: an
  /// acknowledgement that has *not* gone out while a save write is in flight is
  /// what `engine.sleeps` is about.
  bool AwaitAcks(std::size_t wanted, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return acked_.wait_for(lock, budget, [this, wanted] { return acknowledged_.size() >= wanted; });
  }

  std::vector<power::State> acknowledged() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return acknowledged_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable delivered_;
  std::condition_variable acked_;
  std::deque<power::State> pending_;
  std::vector<power::State> acknowledged_;
  bool stopped_ = false;
};

/// The watcher on a thread of its own, stopped and joined by the destructor.
///
/// What `main.cpp` does with a libnx `Thread` (`power_psc.cpp`) and what a
/// scenario here needs a `std::thread` for -- `power::Watcher` owns no thread,
/// deliberately, because the console's stack size is not `std::thread`'s to
/// choose. Both binaries that drive a watcher want exactly these five lines.
class Running {
 public:
  Running(Scripted& module, power::Sink& sink)
      : module_(module), watcher_(module, sink), thread_([this] { watcher_.Run(); }) {}

  ~Running() {
    module_.Stop();
    thread_.join();
  }

 private:
  Scripted& module_;
  power::Watcher watcher_;
  std::thread thread_;
};

}  // namespace power_fake

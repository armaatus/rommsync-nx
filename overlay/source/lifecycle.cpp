#include "lifecycle.hpp"

#include <cstdint>

#include "ipc_client.hpp"

namespace rommsync::overlay {
namespace {

std::uint64_t& ShowingsMutable() {
  static std::uint64_t showings = 0;
  return showings;
}

int& TeardownsMutable() {
  static int teardowns = 0;
  return teardowns;
}

}  // namespace

std::uint64_t Showings() { return ShowingsMutable(); }

bool Cadence::Due() {
  const std::uint64_t showing = Showings();
  if (fresh_ || showing != showing_ || since_ + 1 >= every_) {
    fresh_ = false;
    showing_ = showing;
    since_ = 0;
    return true;
  }
  ++since_;
  return false;
}

Lifecycle::~Lifecycle() { Stop(); }

void Lifecycle::Start() {
  // First, before anything can go wrong: a previous instance that died holding
  // exclusive input left it held, and this is the only process in a position to
  // give it back (#209).
  if (foreground_ != nullptr) {
    foreground_(false);
  }
}

void Lifecycle::Shown() { ++ShowingsMutable(); }

void Lifecycle::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  client_.Shutdown();
  ++TeardownsMutable();
}

int Lifecycle::Teardowns() { return TeardownsMutable(); }

}  // namespace rommsync::overlay

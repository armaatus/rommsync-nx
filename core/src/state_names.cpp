#include "rommsync/state_names.hpp"

#include <string>
#include <string_view>

#include "rommsync/config_text.hpp"
#include "rommsync/save_source.hpp"

namespace rommsync::scan {
namespace {

using config::text::LowerAscii;

constexpr std::string_view kState = ".state";
constexpr std::string_view kAutoSuffix = ".state.auto";

/// What follows a state's stem.
enum class Kind {
  kNone,      ///< not a state name either frontend writes
  kBare,      ///< `.state`
  kNumbered,  ///< `.state<N>`
  kAuto,      ///< `.state.auto`
};

struct Suffix {
  Kind kind = Kind::kNone;
  std::string_view stem;
  unsigned number = 0;
};

/// `name` split into its stem and the state suffix after it. A number is
/// digits only, with no leading zero but `0` itself, and at most three of
/// them: `.state01` is a different file from `.state1`, and neither frontend
/// writes the first, so it maps to nothing rather than to the second.
Suffix Split(std::string_view name) {
  const std::string lowered = LowerAscii(name);
  Suffix suffix;
  const std::size_t auto_at = lowered.size() - kAutoSuffix.size();
  if (lowered.size() > kAutoSuffix.size() &&
      lowered.compare(auto_at, kAutoSuffix.size(), kAutoSuffix) == 0) {
    suffix.kind = Kind::kAuto;
    suffix.stem = name.substr(0, name.size() - kAutoSuffix.size());
    return suffix;
  }
  const std::size_t at = lowered.rfind(kState);
  if (at == std::string::npos || at == 0) {
    return suffix;
  }
  const std::string_view digits = std::string_view(lowered).substr(at + kState.size());
  if (digits.size() > 3 || (digits.size() > 1 && digits.front() == '0')) {
    return suffix;
  }
  unsigned number = 0;
  for (const char digit : digits) {
    if (digit < '0' || digit > '9') {
      return suffix;
    }
    number = number * 10 + static_cast<unsigned>(digit - '0');
  }
  suffix.kind = digits.empty() ? Kind::kBare : Kind::kNumbered;
  suffix.stem = name.substr(0, at);
  suffix.number = number;
  return suffix;
}

/// Tico's six: menu slots 1-5 as `.state0`-`.state4`, Auto as `.state5`.
constexpr unsigned kTicoAuto = 5;

}  // namespace

std::optional<std::string> RetroArchStateName(std::string_view tico_name) {
  const Suffix suffix = Split(tico_name);
  if (suffix.kind != Kind::kNumbered || suffix.number > kTicoAuto) {
    return std::nullopt;
  }
  if (suffix.number == kTicoAuto) {
    return std::string(suffix.stem) + std::string(kAutoSuffix);
  }
  return std::string(suffix.stem) + std::string(kState) + std::to_string(suffix.number + 1);
}

std::optional<std::string> TicoStateName(std::string_view retroarch_name) {
  const Suffix suffix = Split(retroarch_name);
  if (suffix.kind == Kind::kAuto) {
    return std::string(suffix.stem) + std::string(kState) + std::to_string(kTicoAuto);
  }
  if (suffix.kind != Kind::kNumbered || suffix.number < 1 || suffix.number > kTicoAuto) {
    return std::nullopt;
  }
  return std::string(suffix.stem) + std::string(kState) + std::to_string(suffix.number - 1);
}

bool IsTicoStateName(std::string_view name) {
  const Suffix suffix = Split(name);
  return suffix.kind == Kind::kNumbered && suffix.number <= kTicoAuto;
}

bool IsRetroArchStateName(std::string_view name) {
  const Suffix suffix = Split(name);
  return suffix.kind == Kind::kBare || suffix.kind == Kind::kAuto ||
         (suffix.kind == Kind::kNumbered && suffix.number >= 1);
}

std::string StateMatchName(std::string_view name) {
  // `.auto` off the end, and the rom lookup takes `.state` off what is left.
  constexpr std::size_t kDotAuto = kAutoSuffix.size() - kState.size();
  return Split(name).kind == Kind::kAuto ? std::string(name.substr(0, name.size() - kDotAuto))
                                         : std::string(name);
}

bool MapsStateSlots(const config::Config& config, std::string_view emulator_id) {
  return config.sync.map_state_slots && IsRetroArchCore(emulator_id);
}

}  // namespace rommsync::scan

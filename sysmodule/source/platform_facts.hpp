// Atmosphère's version and the emuMMC flag, as the diagnostics bundle says them
// (M11-23, #322).
//
// Both are `splGetConfig` items only Exosphère answers, so `__appInit` asks
// `spl:` for them -- once, and with `spl:` closed again before anything else is
// opened -- and leaves the answers here. `main` turns them into
// `PlatformFacts`' two sentences. The asking needs a console; the decoding and
// the sentence a failed read leaves do not, which is why they are this header
// and not `main.cpp` (`engine.platform_facts`).
//
// No libnx type, for `engine.hpp`'s reason: `test_engine` includes it on the
// host.
#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <type_traits>

namespace rommsync::sysmodule::platform {

/// Exosphère's own `GetConfig` items. libnx's `SplConfigItem` stops at
/// Nintendo's 17, so `main.cpp` dispatches these itself (`AskExosphere`); the
/// numbers are Atmosphère's `spl::ConfigItem` (`spl_types.hpp`).
inline constexpr std::uint32_t kExosphereApiVersion = 65000;
inline constexpr std::uint32_t kExosphereEmummcType = 65007;

/// One item, as `__appInit` left it.
///
/// **Trivially constructible, and it has to be:** `__appInit` writes it before
/// `__libc_init_array` runs (`g_boot`'s reason in `main.cpp`), so `failed` is
/// a pointer to a literal rather than a string.
///
/// `read` false with `failed` null is a read never attempted -- `spl:` was not
/// registered within the boot wait. `failed` names the call that answered
/// `result` instead.
struct ConfigRead {
  const char* failed;
  std::uint32_t result;
  std::uint64_t value;
  bool read;
};
static_assert(std::is_trivially_default_constructible_v<ConfigRead>,
              "__appInit runs before __libc_init_array; a ConfigRead must be "
              "constant-initialised");

/// Why `read` has no value. Never a guess: a bundle that said "sysMMC" for a
/// read that failed would send a support thread the wrong way.
inline std::string NotRead(const ConfigRead& read) {
  if (read.failed == nullptr) return "not read: spl: was not available at boot";
  char hex[8 + 1] = {};
  const std::to_chars_result end = std::to_chars(hex, hex + sizeof(hex) - 1, read.result, 16);
  return std::string("not read: ") + read.failed + " answered 0x" + std::string(hex, end.ptr);
}

/// Item 65000. Exosphère packs it as major, minor and micro in the top three
/// bytes, then the key generation and the target firmware
/// (`secmon_smc_info.cpp`); the bundle wants the release, `1.8.0`.
inline std::string AtmosphereVersion(const ConfigRead& read) {
  if (!read.read) return NotRead(read);
  const auto byte = [&read](int shift) {
    return std::to_string((read.value >> shift) & 0xFF);
  };
  return byte(56) + "." + byte(48) + "." + byte(40);
}

/// Item 65007, the emuMMC configuration's type: 0 is none, so sysMMC; 1 a
/// partition, 2 files on the SD card. A type Atmosphère adds later is still
/// emuMMC, and still says which.
inline std::string Emummc(const ConfigRead& read) {
  if (!read.read) return NotRead(read);
  switch (read.value) {
    case 0:
      return "no (sysMMC)";
    case 1:
      return "yes (partition)";
    case 2:
      return "yes (file)";
    default:
      return "yes (type " + std::to_string(read.value) + ")";
  }
}

}  // namespace rommsync::sysmodule::platform

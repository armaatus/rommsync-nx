// SHA-256, FIPS 180-4, in the portable engine.
//
// Here rather than from a library because `core/` may include only standard
// headers and `rommsync/` ones (core/AGENTS.md), and because the one caller
// that needs it -- deriving `client_device_identifier` from a value that must
// never leave the console in the clear (device_identity.hpp) -- cannot be
// written honestly without a real hash. A stand-in that "looks hashed" would be
// reversible, which is the whole thing being avoided.
//
// It is not a security boundary on its own: see `DeriveDeviceIdentity` in
// device_identity.hpp for what hashing a low-entropy value does and does not
// buy.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace rommsync::crypto {

/// A digest is 32 bytes.
inline constexpr std::size_t kSha256DigestBytes = 32;

/// The digest of `data`, as 32 raw bytes.
std::string Sha256(std::string_view data);

/// The same digest as lowercase hex. `bytes` truncates the output to that many
/// digest bytes -- twice as many hex characters -- for a caller that wants a
/// shorter identifier than 64 characters. Clamped to a whole digest.
std::string Sha256Hex(std::string_view data, std::size_t bytes = kSha256DigestBytes);

/// Lowercase hex of arbitrary bytes. Here because every caller that wants a
/// digest as text wants this too.
std::string ToHex(std::string_view bytes);

}  // namespace rommsync::crypto

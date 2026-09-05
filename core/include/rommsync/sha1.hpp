// SHA-1, FIPS 180-4, in the portable engine.
//
// Here rather than from a library for the reason `md5.hpp` and `sha256.hpp` are:
// `core/` may include only standard headers and `rommsync/` ones
// (core/AGENTS.md). It is a *third* digest and replaces neither -- SHA-256
// derives the device identifier, MD5 is what RomM compares saves on, and this is
// the one RomM records for a **rom**: `sha1_hash` on the rom schema
// (docs/API_CONTRACT.md#resume--integrity), which is what says a 120 MiB
// transfer over a bad link actually arrived.
//
// **This is not a security primitive.** SHA-1 is collision-broken and is used
// here for exactly one thing: comparing a downloaded rom against the digest the
// server already computed for the same file. The client does not get to pick the
// algorithm -- a SHA-256 matches nothing in that field. Nothing that needs
// collision resistance may use this; `crypto::Sha256` is next door.
//
// `Sha1Hasher` is incremental because a rom is gigabytes and the sysmodule's
// inner heap is 512 KiB: `Sha1FileHex` feeds it a chunk at a time rather than
// reading the file whole.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rommsync::crypto {

/// A digest is 20 bytes.
inline constexpr std::size_t kSha1DigestBytes = 20;

/// ...and 40 hex characters, which is the width of a rom's `sha1_hash`.
inline constexpr std::size_t kSha1HexDigits = kSha1DigestBytes * 2;

/// SHA-1, a chunk at a time.
///
/// `Update` as many times as there are chunks, `Finish` once. The split points
/// do not affect the digest -- which is the property worth testing, since a
/// buffering bug is invisible to any test that hashes one string in one call.
class Sha1Hasher {
 public:
  /// Absorb `data`. Any size, including zero.
  Sha1Hasher& Update(std::string_view data);

  /// The digest as 20 raw bytes. Call once: the padding is appended to the
  /// running state, so a second call would digest the padding too.
  std::string Finish();

  /// The same digest as 40 lowercase hex characters. Here rather than left to
  /// the caller for `Md5Hasher::FinishHex`'s reason -- every streaming caller
  /// wants the hex, and a second spelling of the conversion is a second place
  /// for a digest to come out uppercase.
  std::string FinishHex();

 private:
  /// FIPS 180-4 §5.3.1's initial H(0).
  std::uint32_t state_[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};

  /// Bytes seen but not yet compressed -- always fewer than 64.
  unsigned char pending_[64] = {};
  std::size_t pending_size_ = 0;

  /// Total bytes absorbed. The length appended is in bits and is taken modulo
  /// 2^64, so this need not saturate.
  std::uint64_t absorbed_ = 0;
};

/// The digest of `data`, as 20 raw bytes.
std::string Sha1(std::string_view data);

/// The same digest as 40 lowercase hex characters -- the form a rom's
/// `sha1_hash` takes on the wire.
///
/// The hex conversion is `crypto::ToHex` (sha256.hpp), shared rather than
/// copied: two spellings of "bytes as hex" is two places for a digest to come
/// out uppercase, and an uppercase digest matches nothing on the server.
std::string Sha1Hex(std::string_view data);

/// Stream `path` through SHA-1 and answer 40 lowercase hex characters.
///
/// Chunked rather than read whole (`crypto::StreamFile`, hash_file.hpp): a rom
/// is gigabytes and the sysmodule heap is 512 KiB, so a version of this that
/// buffered the file would be a `bad_alloc` on the console and a green test on a
/// laptop.
///
/// **Empty when the file could not be read to the end**, which no digest can
/// collide with -- a digest is always 40 characters. A partial read is not a
/// short digest: hashing the half of a rom that arrived would produce a
/// perfectly plausible answer that matches nothing, and a caller cannot tell
/// that from a corrupt download. Callers that need to say *why* the file could
/// not be read use `crypto::StreamFile` directly.
std::string Sha1FileHex(const std::string& path);

}  // namespace rommsync::crypto

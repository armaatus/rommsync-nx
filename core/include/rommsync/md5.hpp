// MD5, RFC 1321, in the portable engine.
//
// Here rather than from a library for the same reason `sha256.hpp` is: `core/`
// may include only standard headers and `rommsync/` ones (core/AGENTS.md). It
// is a *separate* digest from that one and neither replaces the other --
// SHA-256 derives the device identifier, and MD5 is the digest RomM compares
// saves on.
//
// **This is not a security primitive.** MD5 is collision-broken and is used
// here for exactly one thing: `ClientSaveState.content_hash`, which RomM
// computes as an MD5 hexdigest of the save's bytes (sync.hpp,
// docs/API_CONTRACT.md). The client does not get to pick the algorithm -- a
// SHA-256 in that field matches nothing, so every unchanged save negotiates as
// changed forever. Nothing that needs collision resistance may use this;
// `crypto::Sha256` is next door.
//
// `Md5Hasher` is incremental because a save state is tens of megabytes and the
// sysmodule heap is small: `state::HashFile` feeds it a chunk at a time rather
// than reading the file whole.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rommsync::crypto {

/// A digest is 16 bytes.
inline constexpr std::size_t kMd5DigestBytes = 16;

/// ...and 32 hex characters, which is `sync::kContentHashDigits`.
inline constexpr std::size_t kMd5HexDigits = kMd5DigestBytes * 2;

/// MD5, a chunk at a time.
///
/// `Update` as many times as there are chunks, `Finish` once. The split points
/// do not affect the digest -- which is the property worth testing, since a
/// buffering bug is invisible to any test that hashes one string in one call.
class Md5Hasher {
 public:
  /// Absorb `data`. Any size, including zero.
  Md5Hasher& Update(std::string_view data);

  /// The digest as 16 raw bytes. Call once: the padding is appended to the
  /// running state, so a second call would digest the padding too.
  std::string Finish();

  /// The same digest as 32 lowercase hex characters. Here rather than left to
  /// the caller because every streaming caller wants the hex -- it is what goes
  /// on the wire -- and a second spelling of the conversion is a second place
  /// for a digest to come out uppercase.
  std::string FinishHex();

 private:
  /// RFC 1321 §3.3's initial A, B, C, D.
  std::uint32_t state_[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

  /// Bytes seen but not yet compressed -- always fewer than 64.
  unsigned char pending_[64] = {};
  std::size_t pending_size_ = 0;

  /// Total bytes absorbed. The length RFC 1321 appends is in bits, and it is
  /// taken modulo 2^64, so this need not saturate.
  std::uint64_t absorbed_ = 0;
};

/// The digest of `data`, as 16 raw bytes.
std::string Md5(std::string_view data);

/// The same digest as 32 lowercase hex characters -- the form `content_hash`
/// takes on the wire, since RomM stores and compares `hexdigest()`.
///
/// The hex conversion is `crypto::ToHex` (sha256.hpp), shared rather than
/// copied: two spellings of "bytes as hex" is two places for a digest to come
/// out uppercase, and an uppercase digest matches nothing on the server.
std::string Md5Hex(std::string_view data);

/// Stream `path` through MD5 and answer 32 lowercase hex characters.
///
/// `crypto::Sha1FileHex`'s twin, and here for the same caller: a rom whose
/// library has no `sha1_hash` is verified against its `md5_hash` instead
/// (download.hpp). Chunked through `crypto::StreamFile` rather than read whole,
/// for the reason that header gives.
///
/// **Empty when the file could not be read to the end**, which no digest can
/// collide with. A caller that needs to say *why* uses `crypto::StreamFile`
/// directly -- `state::HashFile` is the one that does.
std::string Md5FileHex(const std::string& path);

}  // namespace rommsync::crypto

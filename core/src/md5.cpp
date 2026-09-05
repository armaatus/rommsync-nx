#include "rommsync/md5.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "rommsync/sha256.hpp"  // crypto::ToHex

namespace rommsync::crypto {
namespace {

/// `floor(2^32 * abs(sin(i + 1)))` for i in [0, 64). RFC 1321 §3.4's table,
/// transcribed rather than computed -- `std::sin` is not required to give the
/// same last bit everywhere, and a table that is wrong in one entry still
/// produces 32 plausible hex characters. The RFC's own test vectors in
/// `core.md5` are the real check.
constexpr std::uint32_t kSineTable[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u,
};

/// Per-round left-rotate amounts, four rounds of sixteen. RFC 1321 §3.4.
constexpr int kShifts[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

std::uint32_t RotateLeft(std::uint32_t value, int by) {
  return (value << by) | (value >> (32 - by));
}

/// One 64-byte block into the running state. RFC 1321 §3.4.
///
/// The block is read **little-endian**, which is the difference between this
/// and `sha256.cpp`'s otherwise identical-looking compression: MD5 came out of
/// a little-endian world and SHA-2 did not, and getting the two the same way
/// round is the classic way to produce a digest that is self-consistent and
/// matches nobody.
void Compress(std::uint32_t state[4], const unsigned char block[64]) {
  std::uint32_t words[16];
  for (int at = 0; at < 16; ++at) {
    words[at] = static_cast<std::uint32_t>(block[at * 4]) |
                static_cast<std::uint32_t>(block[at * 4 + 1]) << 8 |
                static_cast<std::uint32_t>(block[at * 4 + 2]) << 16 |
                static_cast<std::uint32_t>(block[at * 4 + 3]) << 24;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];

  for (int at = 0; at < 64; ++at) {
    std::uint32_t mixed = 0;
    int word = 0;
    if (at < 16) {
      mixed = (b & c) | (~b & d);
      word = at;
    } else if (at < 32) {
      mixed = (d & b) | (~d & c);
      word = (5 * at + 1) % 16;
    } else if (at < 48) {
      mixed = b ^ c ^ d;
      word = (3 * at + 5) % 16;
    } else {
      mixed = c ^ (b | ~d);
      word = (7 * at) % 16;
    }

    const std::uint32_t rotated =
        RotateLeft(a + mixed + kSineTable[at] + words[word], kShifts[at]);
    a = d;
    d = c;
    c = b;
    b = b + rotated;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

}  // namespace

Md5Hasher& Md5Hasher::Update(std::string_view data) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t at = 0;

  // Top up a partial block first, so the fast path below only ever sees a
  // hasher with nothing buffered.
  if (pending_size_ != 0) {
    const std::size_t room = sizeof(pending_) - pending_size_;
    const std::size_t take = data.size() < room ? data.size() : room;
    for (std::size_t copied = 0; copied < take; ++copied) {
      pending_[pending_size_ + copied] = bytes[copied];
    }
    pending_size_ += take;
    at = take;
    if (pending_size_ == sizeof(pending_)) {
      Compress(state_, pending_);
      pending_size_ = 0;
    }
  }

  for (; at + 64 <= data.size(); at += 64) {
    Compress(state_, bytes + at);
  }

  for (; at < data.size(); ++at) {
    pending_[pending_size_++] = bytes[at];
  }

  absorbed_ += data.size();
  return *this;
}

std::string Md5Hasher::Finish() {
  // The tail, padded: whatever is buffered, a 0x80 byte, zeroes, and the length
  // in bits as a little-endian 64-bit value. That needs one block, or two when
  // the remainder leaves no room for the length.
  unsigned char tail[128] = {};
  for (std::size_t copied = 0; copied < pending_size_; ++copied) {
    tail[copied] = pending_[copied];
  }
  tail[pending_size_] = 0x80;
  const std::size_t blocks = pending_size_ + 9 <= 64 ? 1 : 2;
  const std::uint64_t bits = absorbed_ * 8;
  for (int byte = 0; byte < 8; ++byte) {
    tail[blocks * 64 - 8 + static_cast<std::size_t>(byte)] =
        static_cast<unsigned char>((bits >> (byte * 8)) & 0xffu);
  }
  for (std::size_t block = 0; block < blocks; ++block) {
    Compress(state_, tail + block * 64);
  }

  std::string digest(kMd5DigestBytes, '\0');
  for (std::size_t word = 0; word < 4; ++word) {
    for (int byte = 0; byte < 4; ++byte) {
      digest[word * 4 + static_cast<std::size_t>(byte)] =
          static_cast<char>((state_[word] >> (byte * 8)) & 0xffu);
    }
  }
  return digest;
}

std::string Md5Hasher::FinishHex() { return ToHex(Finish()); }

std::string Md5(std::string_view data) {
  Md5Hasher hasher;
  hasher.Update(data);
  return hasher.Finish();
}

std::string Md5Hex(std::string_view data) { return ToHex(Md5(data)); }

}  // namespace rommsync::crypto

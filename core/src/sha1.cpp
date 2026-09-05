#include "rommsync/sha1.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "rommsync/hash_file.hpp"
#include "rommsync/sha256.hpp"  // crypto::ToHex

namespace rommsync::crypto {
namespace {

std::uint32_t RotateLeft(std::uint32_t value, int by) {
  return (value << by) | (value >> (32 - by));
}

/// One 64-byte block into the running state. FIPS 180-4 §6.1.2.
///
/// The block is read **big-endian**, unlike `md5.cpp`'s otherwise
/// identical-looking compression: MD5 came out of a little-endian world and the
/// SHA family did not, and getting the two the same way round is the classic way
/// to produce a digest that is self-consistent and matches nobody.
void Compress(std::uint32_t state[5], const unsigned char block[64]) {
  std::uint32_t words[80];
  for (int at = 0; at < 16; ++at) {
    words[at] = static_cast<std::uint32_t>(block[at * 4]) << 24 |
                static_cast<std::uint32_t>(block[at * 4 + 1]) << 16 |
                static_cast<std::uint32_t>(block[at * 4 + 2]) << 8 |
                static_cast<std::uint32_t>(block[at * 4 + 3]);
  }
  // The message schedule. The rotate by one is what separates SHA-1 from SHA-0,
  // and leaving it out gives a hash that passes every structural test and none
  // of the vectors.
  for (int at = 16; at < 80; ++at) {
    words[at] = RotateLeft(words[at - 3] ^ words[at - 8] ^ words[at - 14] ^ words[at - 16], 1);
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];

  for (int at = 0; at < 80; ++at) {
    std::uint32_t mixed = 0;
    std::uint32_t constant = 0;
    if (at < 20) {
      mixed = (b & c) | (~b & d);
      constant = 0x5a827999u;
    } else if (at < 40) {
      mixed = b ^ c ^ d;
      constant = 0x6ed9eba1u;
    } else if (at < 60) {
      mixed = (b & c) | (b & d) | (c & d);
      constant = 0x8f1bbcdcu;
    } else {
      mixed = b ^ c ^ d;
      constant = 0xca62c1d6u;
    }

    const std::uint32_t rotated = RotateLeft(a, 5) + mixed + e + constant + words[at];
    e = d;
    d = c;
    c = RotateLeft(b, 30);
    b = a;
    a = rotated;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

}  // namespace

Sha1Hasher& Sha1Hasher::Update(std::string_view data) {
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

std::string Sha1Hasher::Finish() {
  // The tail, padded: whatever is buffered, a 0x80 byte, zeroes, and the length
  // in bits as a big-endian 64-bit value. That needs one block, or two when the
  // remainder leaves no room for the length.
  unsigned char tail[128] = {};
  for (std::size_t copied = 0; copied < pending_size_; ++copied) {
    tail[copied] = pending_[copied];
  }
  tail[pending_size_] = 0x80;
  const std::size_t blocks = pending_size_ + 9 <= 64 ? 1 : 2;
  const std::uint64_t bits = absorbed_ * 8;
  for (int byte = 0; byte < 8; ++byte) {
    tail[blocks * 64 - 1 - static_cast<std::size_t>(byte)] =
        static_cast<unsigned char>((bits >> (byte * 8)) & 0xffu);
  }
  for (std::size_t block = 0; block < blocks; ++block) {
    Compress(state_, tail + block * 64);
  }

  std::string digest(kSha1DigestBytes, '\0');
  for (std::size_t word = 0; word < 5; ++word) {
    for (int byte = 0; byte < 4; ++byte) {
      digest[word * 4 + static_cast<std::size_t>(byte)] =
          static_cast<char>((state_[word] >> (24 - byte * 8)) & 0xffu);
    }
  }
  return digest;
}

std::string Sha1Hasher::FinishHex() { return ToHex(Finish()); }

std::string Sha1(std::string_view data) {
  Sha1Hasher hasher;
  hasher.Update(data);
  return hasher.Finish();
}

std::string Sha1Hex(std::string_view data) { return ToHex(Sha1(data)); }

std::string Sha1FileHex(const std::string& path) {
  Sha1Hasher hasher;
  bool opened = false;
  if (!StreamFile(path, hasher, &opened)) {
    return {};
  }
  return hasher.FinishHex();
}

}  // namespace rommsync::crypto

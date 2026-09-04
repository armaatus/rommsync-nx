#include "rommsync/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rommsync::crypto {
namespace {

/// The first 32 bits of the fractional parts of the cube roots of the first 64
/// primes. FIPS 180-4 §4.2.2, transcribed, not computed -- a table that is
/// wrong in one entry still produces plausible-looking digests, which is why
/// the known-answer vectors in `core.device_identity` are the real check.
constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

std::uint32_t RotateRight(std::uint32_t value, int by) {
  return (value >> by) | (value << (32 - by));
}

/// One 64-byte block into the running state. FIPS 180-4 §6.2.2.
void Compress(std::uint32_t state[8], const unsigned char block[64]) {
  std::uint32_t schedule[64];
  for (int at = 0; at < 16; ++at) {
    schedule[at] = static_cast<std::uint32_t>(block[at * 4]) << 24 |
                   static_cast<std::uint32_t>(block[at * 4 + 1]) << 16 |
                   static_cast<std::uint32_t>(block[at * 4 + 2]) << 8 |
                   static_cast<std::uint32_t>(block[at * 4 + 3]);
  }
  for (int at = 16; at < 64; ++at) {
    const std::uint32_t s0 = RotateRight(schedule[at - 15], 7) ^
                             RotateRight(schedule[at - 15], 18) ^ (schedule[at - 15] >> 3);
    const std::uint32_t s1 = RotateRight(schedule[at - 2], 17) ^
                             RotateRight(schedule[at - 2], 19) ^ (schedule[at - 2] >> 10);
    schedule[at] = schedule[at - 16] + s0 + schedule[at - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (int at = 0; at < 64; ++at) {
    const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t choice = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choice + kRoundConstants[at] + schedule[at];
    const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

std::string Sha256(std::string_view data) {
  // The first 32 bits of the fractional parts of the square roots of the first
  // eight primes. FIPS 180-4 §5.3.3.
  std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  std::size_t at = 0;
  for (; at + 64 <= data.size(); at += 64) {
    Compress(state, bytes + at);
  }

  // The tail, padded: the remainder, a 0x80 byte, zeroes, and the length in
  // bits as a big-endian 64-bit value. That needs one block, or two when the
  // remainder leaves no room for the length.
  unsigned char tail[128] = {};
  const std::size_t remaining = data.size() - at;
  for (std::size_t copied = 0; copied < remaining; ++copied) {
    tail[copied] = bytes[at + copied];
  }
  tail[remaining] = 0x80;
  const std::size_t blocks = remaining + 9 <= 64 ? 1 : 2;
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
  for (int byte = 0; byte < 8; ++byte) {
    tail[blocks * 64 - 1 - static_cast<std::size_t>(byte)] =
        static_cast<unsigned char>((bits >> (byte * 8)) & 0xffu);
  }
  for (std::size_t block = 0; block < blocks; ++block) {
    Compress(state, tail + block * 64);
  }

  std::string digest(kSha256DigestBytes, '\0');
  for (std::size_t word = 0; word < 8; ++word) {
    for (int byte = 0; byte < 4; ++byte) {
      digest[word * 4 + static_cast<std::size_t>(byte)] =
          static_cast<char>((state[word] >> (24 - byte * 8)) & 0xffu);
    }
  }
  return digest;
}

std::string ToHex(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    out.push_back(kDigits[value >> 4]);
    out.push_back(kDigits[value & 0x0fu]);
  }
  return out;
}

std::string Sha256Hex(std::string_view data, std::size_t bytes) {
  const std::string digest = Sha256(data);
  const std::size_t keep = bytes < kSha256DigestBytes ? bytes : kSha256DigestBytes;
  return ToHex(std::string_view(digest).substr(0, keep));
}

}  // namespace rommsync::crypto

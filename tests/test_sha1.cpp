// SHA-1, against FIPS 180-4's and RFC 3174's own test suite.
//
// Pure arithmetic apart from the last scenario, and the thing it guards is
// invisible from a status code: a digest that is wrong is still 40 plausible hex
// characters, and a client that computes one either refuses every rom it
// downloads or -- far worse -- accepts every corrupt one, depending on which way
// the bug falls. The published vectors are the only check that separates "a
// SHA-1" from "a hash function that resembles one".
//
// Beyond the vectors, four properties the vectors alone cannot see:
//
//   the chunking      -- a digest must not depend on where `Update` was split,
//                        which is the one bug `Sha1(std::string_view)` can never
//                        expose and `Sha1FileHex` depends on entirely.
//   the padding edges -- 55/56/63/64/119/120 bytes, the lengths where the
//                        length field does and does not fit in the last block.
//   the spelling      -- lowercase, 40 digits, since RomM's `sha1_hash` is
//                        compared as text and an uppercase digest matches
//                        nothing.
//   the file          -- `Sha1FileHex` over the 120 MiB fixture equals what
//                        `sha1sum` says, computed without holding it in memory.
//
//   vectors   -- everything above except the fixture; needs no server, no files
//   fixture   -- the 120 MiB seeded rom, streamed
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include "checks.hpp"
#include "rommsync/sha1.hpp"

namespace {

namespace crypto = rommsync::crypto;

struct Vector {
  const char* input;
  const char* digest;
};

/// RFC 3174 §7.3's suite, plus the empty string -- which FIPS 180-4 includes and
/// which is the one input where a hasher that never compressed a block still has
/// to emit the padded digest.
constexpr Vector kPublished[] = {
    {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
    {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
     "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
};

void ThePublishedVectorsPass(checks::Checks& c) {
  for (const Vector& vector : kPublished) {
    const std::string what = std::string("SHA-1 vector \"") + vector.input + "\" (" +
                             std::to_string(std::string(vector.input).size()) + " bytes)";
    c.ExpectEq(crypto::Sha1Hex(vector.input), std::string(vector.digest), what);
  }

  // RFC 3174's third and fourth vectors are built rather than written out: a
  // million bytes, and a 640-byte pattern. Both are long enough that a wrong
  // message schedule cannot survive them, which the short ones can.
  c.ExpectEq(crypto::Sha1Hex(std::string(1'000'000, 'a')),
             std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"),
             "RFC 3174 vector 3: a million 'a'");

  std::string repeated;
  for (int at = 0; at < 10; ++at) {
    repeated += "0123456701234567012345670123456701234567012345670123456701234567";
  }
  c.ExpectEq(crypto::Sha1Hex(repeated), std::string("dea356a2cddd90c7a7ecedc5ebb563934f460452"),
             "RFC 3174 vector 4: the 640-byte pattern");
}

/// The same vectors through the incremental interface, one byte at a time.
///
/// `Sha1Hex` hashes in one `Update`, so it exercises exactly none of the
/// buffering. Feeding a vector byte by byte is the same digest through as many
/// buffered partial blocks as it has bytes, and any off-by-one in the top-up
/// path shows up here and nowhere else.
void OneByteAtATimeIsTheSameDigest(checks::Checks& c) {
  for (const Vector& vector : kPublished) {
    const std::string input = vector.input;
    crypto::Sha1Hasher hasher;
    for (const char byte : input) {
      hasher.Update(std::string_view(&byte, 1));
    }
    c.ExpectEq(hasher.FinishHex(), std::string(vector.digest),
               std::string("byte-at-a-time \"") + vector.input + "\"");
  }
}

/// A body long enough to span blocks, split at every point in it.
///
/// This is the property `Sha1FileHex` rests on: it reads a fixed 4 KiB chunk at
/// a time, so a rom's digest depends on the read size unless the split is
/// invisible. Nothing about a rom's *size* is under this client's control, so a
/// hasher that is only correct on 64-byte multiples would be wrong on almost
/// every real file and right on every test that hashed one string.
void TheSplitPointDoesNotMatter(checks::Checks& c) {
  std::string body;
  for (int at = 0; at < 300; ++at) {
    body += static_cast<char>(at % 251);
  }
  const std::string expected = crypto::Sha1Hex(body);

  for (std::size_t split = 0; split <= body.size(); ++split) {
    crypto::Sha1Hasher hasher;
    hasher.Update(std::string_view(body).substr(0, split));
    hasher.Update(std::string_view(body).substr(split));
    if (hasher.FinishHex() != expected) {
      c.Expect(false, "a split at " + std::to_string(split) + " changed the digest");
      return;
    }
  }

  // ...and in three pieces, which is what a chunked read of a file that is not
  // a multiple of the buffer actually looks like.
  crypto::Sha1Hasher thirds;
  thirds.Update(std::string_view(body).substr(0, 7));
  thirds.Update(std::string_view(body).substr(7, 190));
  thirds.Update(std::string_view(body).substr(197));
  c.ExpectEq(thirds.FinishHex(), expected, "three uneven chunks");

  // An empty Update in the middle is a short read, not a boundary.
  crypto::Sha1Hasher interrupted;
  interrupted.Update(std::string_view(body).substr(0, 64));
  interrupted.Update(std::string_view());
  interrupted.Update(std::string_view(body).substr(64));
  c.ExpectEq(interrupted.FinishHex(), expected, "an empty chunk changes nothing");
}

/// The lengths where the padding decides between one block and two.
///
/// 55 bytes leaves exactly room for the 0x80 byte and the 8-byte length; 56 does
/// not, and needs a second block. 64 and 128 are the exact multiples, where the
/// tail is padding and nothing else. These are the lengths a wrong `blocks`
/// calculation gets away with everywhere else.
///
/// The expected digests come from `sha1sum(1)`, not from this implementation: an
/// implementation compared against itself agrees whatever it does.
void ThePaddingEdges(checks::Checks& c) {
  struct Edge {
    std::size_t length;
    const char* digest;
  };
  // `std::string(n, 'a')` at each boundary the padding cares about.
  constexpr Edge kEdges[] = {
      {55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"},   // the last length that fits one block
      {56, "c2db330f6083854c99d4b5bfb6e8f29f201be699"},   // the first that needs two
      {57, "f08f24908d682555111be7ff6f004e78283d989a"},
      {63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5"},
      {64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"},   // an exact block, all padding in the next
      {65, "11655326c708d70319be2610e8a57d9a5b959d3b"},
      {119, "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56"},
      {120, "f34c1488385346a55709ba056ddd08280dd4c6d6"},
      {128, "ad5b3fdbcb526778c2839d2f151ea753995e26a0"},  // two exact blocks
  };
  for (const Edge& edge : kEdges) {
    c.ExpectEq(crypto::Sha1Hex(std::string(edge.length, 'a')), std::string(edge.digest),
               std::to_string(edge.length) + " bytes of 'a'");
  }
}

/// 40 lowercase hex, which is how RomM stores a rom's `sha1_hash` and therefore
/// the only spelling that can be compared against it.
void TheSpellingIsWhatTheServerCompares(checks::Checks& c) {
  c.ExpectEq(crypto::Sha1("abc").size(), crypto::kSha1DigestBytes, "the raw digest is 20 bytes");

  const std::string digest = crypto::Sha1Hex("240pee.nes contents");
  c.ExpectEq(digest.size(), crypto::kSha1HexDigits, "the hexdigest is 40 characters");
  bool lowercase_hex = true;
  for (const char character : digest) {
    const bool decimal = character >= '0' && character <= '9';
    const bool lower = character >= 'a' && character <= 'f';
    lowercase_hex = lowercase_hex && (decimal || lower);
  }
  c.Expect(lowercase_hex, "and every character is a lowercase hex digit");
}

/// A file that is not there is empty, and empty is a value no digest can be.
void AnUnreadableFileIsNotADigest(checks::Checks& c) {
  c.Expect(crypto::Sha1FileHex(std::string(ROMMSYNC_TEST_SCRATCH) + "/no-such-rom.gba").empty(),
           "a file that is not there hashes to nothing at all, which no digest collides with");
}

// --- the fixture --------------------------------------------------------------

/// `Sha1FileHex` over the 120 MiB seeded rom.
///
/// The digest is `sha1sum`'s, and it is also what RomM's own scan recorded for
/// the same file -- which is the comparison that matters, since the whole point
/// of this hash is to agree with the server about a rom. Streamed: the file is
/// 126 MB and the sysmodule's heap is 512 KiB, so a version of this that read it
/// whole would pass here and be a `bad_alloc` on the console.
bool TheFixtureStreams(checks::Checks& c) {
  const std::string path = std::string(ROMMSYNC_LIBRARY_DIR) + "/roms/gba/synthetic-large.gba";
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    // The seeded library is fetched, not committed (`.gitignore`), so this is
    // the same skip every rig scenario takes when the fixture is not there --
    // and the same failure under ROMMSYNC_REQUIRE_RIG, where a skip would let a
    // green CI run mean nothing was checked.
    std::cerr << "the seeded library is not there: " << path
              << "\n  seed it with: ./server/testing/seed.sh\n";
    return false;
  }
  std::fclose(file);

  c.ExpectEq(crypto::Sha1FileHex(path),
             std::string("b66994c177eb3575f877b0a80617f1fe250555f4"),
             "the 120 MiB fixture hashes to what sha1sum and RomM both say it does");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "vectors";
  checks::Checks c;

  // CTest's SKIP_RETURN_CODE, which is `rig::kSkip` everywhere else in this
  // directory. Spelled out rather than included: rig.hpp pulls in the libcurl
  // backend, and this test has no business linking a transport.
  constexpr int kSkip = 77;

  if (scenario == "vectors") {
    ThePublishedVectorsPass(c);
    OneByteAtATimeIsTheSameDigest(c);
    TheSplitPointDoesNotMatter(c);
    ThePaddingEdges(c);
    TheSpellingIsWhatTheServerCompares(c);
    AnUnreadableFileIsNotADigest(c);
  } else if (scenario == "fixture") {
    if (!TheFixtureStreams(c)) {
      return kSkip;
    }
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }

  if (c.failures() == 0) {
    std::cout << "sha1." << scenario << " ok\n";
  }
  return c.failures() == 0 ? 0 : 1;
}

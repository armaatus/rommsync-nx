// MD5, against RFC 1321's own test suite.
//
// Pure arithmetic, so this never skips -- and it must not, because the thing it
// guards is invisible from a status code. A digest that is wrong is still 32
// plausible hex characters, and a client that sends one negotiates every
// unchanged save as changed, forever (sync.hpp). The RFC's vectors are the only
// check that separates "an MD5" from "a hash function that resembles one".
//
// Beyond the vectors, three properties the vectors alone cannot see:
//
//   the chunking      -- a digest must not depend on where `Update` was split,
//                        which is the one bug `Md5(std::string_view)` can never
//                        expose and `state::HashFile` depends on entirely.
//   the padding edges -- 55/56/63/64/119/120 bytes, the lengths where the
//                        length field does and does not fit in the last block.
//   the spelling      -- lowercase, 32 digits, and what `sync::Validate`
//                        accepts, since an uppercase digest matches nothing.
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "checks.hpp"
#include "rommsync/json.hpp"
#include "rommsync/md5.hpp"
#include "rommsync/sync.hpp"

namespace crypto = rommsync::crypto;
namespace sync = rommsync::sync;

namespace {

struct Vector {
  const char* input;
  const char* digest;
};

/// RFC 1321 §A.5's suite, verbatim and in its order. The empty string is the
/// first of them -- the RFC includes it, and it is the one input where a
/// hasher that never compressed a block still has to emit the padded digest.
constexpr Vector kRfc1321[] = {
    {"", "d41d8cd98f00b204e9800998ecf8427e"},
    {"a", "0cc175b9c0f1b6a831c399e269772661"},
    {"abc", "900150983cd24fb0d6963f7d28e17f72"},
    {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
    {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
    {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
     "d174ab98d277d9f5a5611c2c9f419d9f"},
    {"123456789012345678901234567890123456789012345678901234567890123456789012"
     "34567890",
     "57edf4a22be3c955ac49da2e2107b67a"},
};

void TheRfcVectorsPass(checks::Checks& c) {
  for (const Vector& vector : kRfc1321) {
    const std::string what =
        std::string("RFC 1321 vector \"") + vector.input + "\" (" +
        std::to_string(std::string(vector.input).size()) + " bytes)";
    c.ExpectEq(crypto::Md5Hex(vector.input), std::string(vector.digest), what);
  }
}

/// The same vectors through the incremental interface, one byte at a time.
///
/// `Md5Hex` hashes in one `Update`, so it exercises exactly none of the
/// buffering. Feeding a vector byte by byte is the same digest through 62
/// buffered partial blocks, and any off-by-one in the top-up path shows up here
/// and nowhere else.
void OneByteAtATimeIsTheSameDigest(checks::Checks& c) {
  for (const Vector& vector : kRfc1321) {
    const std::string input = vector.input;
    crypto::Md5Hasher hasher;
    for (const char byte : input) {
      hasher.Update(std::string_view(&byte, 1));
    }
    c.ExpectEq(hasher.FinishHex(), std::string(vector.digest),
               std::string("byte-at-a-time \"") + vector.input + "\"");
  }
}

/// A body long enough to span blocks, split at every point in it.
///
/// This is the property `state::HashFile` rests on: it reads 32 KiB at a time,
/// so a save's digest depends on the read size unless the split is invisible.
/// Nothing about a save's *size* is under this client's control, so a hasher
/// that is only correct on 64-byte multiples would be wrong on almost every
/// real file and right on every test that hashed one string.
void TheSplitPointDoesNotMatter(checks::Checks& c) {
  std::string body;
  for (int at = 0; at < 300; ++at) {
    body += static_cast<char>(at % 251);
  }
  const std::string expected = crypto::Md5Hex(body);

  for (std::size_t split = 0; split <= body.size(); ++split) {
    crypto::Md5Hasher hasher;
    hasher.Update(std::string_view(body).substr(0, split));
    hasher.Update(std::string_view(body).substr(split));
    if (hasher.FinishHex() != expected) {
      c.Expect(false, "a split at " + std::to_string(split) + " changed the digest");
      return;
    }
  }

  // ...and in three pieces, which is what a chunked read of a file that is not
  // a multiple of the buffer actually looks like.
  crypto::Md5Hasher thirds;
  thirds.Update(std::string_view(body).substr(0, 7));
  thirds.Update(std::string_view(body).substr(7, 190));
  thirds.Update(std::string_view(body).substr(197));
  c.ExpectEq(thirds.FinishHex(), expected, "three uneven chunks");

  // An empty Update in the middle is a short read, not a boundary.
  crypto::Md5Hasher interrupted;
  interrupted.Update(std::string_view(body).substr(0, 64));
  interrupted.Update(std::string_view());
  interrupted.Update(std::string_view(body).substr(64));
  c.ExpectEq(interrupted.FinishHex(), expected, "an empty chunk changes nothing");
}

/// The lengths where RFC 1321's padding decides between one block and two.
///
/// 55 bytes leaves exactly room for the 0x80 byte and the 8-byte length; 56
/// does not, and needs a second block. 64 and 128 are the exact multiples,
/// where the tail is padding and nothing else. These are the lengths a wrong
/// `blocks` calculation gets away with everywhere else.
///
/// The expected digests come from `md5(1)`, not from this implementation: an
/// implementation compared against itself agrees whatever it does.
void ThePaddingEdges(checks::Checks& c) {
  struct Edge {
    std::size_t length;
    const char* digest;
  };
  // `std::string(n, 'a')` at each boundary the padding cares about.
  constexpr Edge kEdges[] = {
      {55, "ef1772b6dff9a122358552954ad0df65"},   // the last length that fits one block
      {56, "3b0c8ac703f828b04c6c197006d17218"},   // the first that needs two
      {57, "652b906d60af96844ebd21b674f35e93"},
      {63, "b06521f39153d618550606be297466d5"},
      {64, "014842d480b571495a4a0363793f7367"},   // an exact block, all padding in the next
      {65, "c743a45e0d2e6a95cb859adae0248435"},
      {119, "8a7bd0732ed6a28ce75f6dabc90e1613"},
      {120, "5f61c0ccad4cac44c75ff505e1f1e537"},
      {128, "e510683b3f5ffe4093d021808bc6ff70"},  // two exact blocks
  };
  for (const Edge& edge : kEdges) {
    c.ExpectEq(crypto::Md5Hex(std::string(edge.length, 'a')), std::string(edge.digest),
               std::to_string(edge.length) + " bytes of 'a'");
  }
}

/// 32 lowercase hex, which is what `sync::Validate` will take and what RomM
/// stores. A digest that comes out uppercase matches nothing on the server, and
/// it is the same silent failure as a SHA1 in the field.
void TheSpellingIsWhatTheServerCompares(checks::Checks& c) {
  c.ExpectEq(crypto::kMd5HexDigits, sync::kContentHashDigits,
             "an MD5 hexdigest is exactly ClientSaveState::content_hash's width");
  c.ExpectEq(crypto::Md5("abc").size(), crypto::kMd5DigestBytes, "the raw digest is 16 bytes");

  const std::string digest = crypto::Md5Hex("Game (USA).srm contents");
  c.ExpectEq(digest.size(), crypto::kMd5HexDigits, "the hexdigest is 32 characters");
  bool lowercase_hex = true;
  for (const char digit : digest) {
    const bool decimal = digit >= '0' && digit <= '9';
    const bool lower = digit >= 'a' && digit <= 'f';
    lowercase_hex = lowercase_hex && (decimal || lower);
  }
  c.Expect(lowercase_hex, "and every character is a lowercase hex digit");

  sync::ClientSaveState save;
  save.rom_id = 12;
  save.file_name = "Game (USA).srm";
  save.slot = "autosave";
  save.content_hash = digest;
  save.updated_at = sync::Timestamp{} + std::chrono::seconds{1757000000};
  save.file_size_bytes = 23;
  const rommsync::json::Error refused = sync::Validate(save);
  c.Expect(refused.ok(), "sync::Validate accepts a digest this module produced -- " +
                             refused.Describe());
}

}  // namespace

int main() {
  checks::Checks c;
  TheRfcVectorsPass(c);
  OneByteAtATimeIsTheSameDigest(c);
  TheSplitPointDoesNotMatter(c);
  ThePaddingEdges(c);
  TheSpellingIsWhatTheServerCompares(c);
  return c.failures() == 0 ? 0 : 1;
}

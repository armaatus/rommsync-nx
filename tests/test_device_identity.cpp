// `client_device_identifier`: one console, one value, and never the serial.
//
// The failure this file exists to catch is the quiet one. An identifier that
// changes -- because a re-pair discarded it, because the commit was
// interrupted, because a later boot preferred the seed over the file -- does
// not throw anything. RomM simply registers a second device, and the user finds
// out weeks later when a save comes back with no history. So most of what is
// asserted here is that the answer is the *same* answer, across every event
// that could plausibly change it.
//
// The hash underneath gets known-answer vectors, because an implementation that
// only agrees with itself proves nothing: a wrong round constant still produces
// 64 plausible hex characters, and every identifier derived from it would be
// wrong in the same consistent way.
//
// No network and no rig: a hash, a parser and the filesystem, so it never
// skips.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "checks.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/device_identity.hpp"
#include "rommsync/sha256.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;
namespace crypto = rommsync::crypto;
namespace io = rommsync::io;

namespace {

std::filesystem::path ScratchDir() {
  const std::filesystem::path dir =
      std::filesystem::path(ROMMSYNC_TEST_SCRATCH) / "device_identity";
  std::filesystem::create_directories(dir);
  return dir;
}

/// A fresh path, with nothing of a previous run beside it.
std::filesystem::path FreshPath(const std::string& name) {
  const std::filesystem::path path = ScratchDir() / name;
  std::filesystem::remove_all(path);
  std::filesystem::remove_all(io::TempPathFor(path.string()));
  std::filesystem::remove_all(io::PreviousPathFor(path.string()));
  return path;
}

/// A synthetic value shaped like what `setsysGetSerialNumber` returns. Not a
/// real serial, and it never leaves this file -- which is the property being
/// asserted about the identifier derived from it.
const std::string kSerial = "XAW10000000000";

auth::IdentitySeed SerialSeed() {
  auth::IdentitySeed seed;
  seed.stable = kSerial;
  return seed;
}

auth::IdentitySeed EntropySeed(char fill) {
  auth::IdentitySeed seed;
  seed.entropy = std::string(auth::kMinimumEntropyBytes, fill);
  return seed;
}

std::string ReadWhole(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteWhole(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// SHA-256 against answers computed by an implementation that is not this one.
///
/// The first four are FIPS 180-4's published vectors. The rest are lengths, not
/// contents: the padding is the part of SHA-256 that is easy to get wrong, and
/// it goes wrong only at the block boundary -- 55 bytes still fits the length
/// field, 56 does not and needs a second block, 64 is exactly one block. A test
/// that hashed only "abc" would pass with all of that broken.
void Sha256MatchesTheKnownVectors(checks::Checks& c) {
  struct Case {
    std::string input;
    const char* digest;
    const char* what;
  };
  const Case kCases[] = {
      {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "the empty string"},
      {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "\"abc\""},
      {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "the 448-bit vector"},
      {"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqr"
       "lmnopqrsmnopqrstnopqrstu",
       "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1", "the 896-bit vector"},
      {std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318",
       "55 bytes -- the last length that pads into one block"},
      {std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a",
       "56 bytes -- the first that needs two"},
      {std::string(63, 'a'), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34",
       "63 bytes"},
      {std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb",
       "64 bytes -- exactly one block, and still a pad block after it"},
      {std::string(65, 'a'), "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0",
       "65 bytes"},
      {std::string(119, 'a'), "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb",
       "119 bytes"},
      {std::string(120, 'a'), "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c",
       "120 bytes"},
      {std::string(127, 'a'), "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a",
       "127 bytes"},
      {std::string(128, 'a'), "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e",
       "128 bytes -- two whole blocks"},
      {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
       "a million bytes"},
  };
  for (const Case& one : kCases) {
    c.ExpectEq(crypto::Sha256Hex(one.input), std::string(one.digest),
               std::string("SHA-256 of ") + one.what);
  }

  // A NUL in the middle is hashed, not treated as the end of the input -- which
  // matters because the derivation deliberately puts one between the salt and
  // the material.
  c.Expect(crypto::Sha256Hex(std::string("a\0b", 3)) != crypto::Sha256Hex("a"),
           "an embedded NUL is part of the input");

  // A truncated digest is a prefix of the whole one, not a different hash.
  const std::string whole = crypto::Sha256Hex("abc");
  c.ExpectEq(crypto::Sha256Hex("abc", 16), whole.substr(0, 32), "a truncated digest is a prefix");
  c.ExpectEq(crypto::Sha256Hex("abc", 999), whole, "asking for more than a digest gives a digest");
}

/// The identifier is derived from the serial and is not the serial: the value
/// that identifies the hardware must not leave the console.
void HidesTheSerialItCameFrom(checks::Checks& c) {
  const auth::DerivedIdentity derived = auth::DeriveDeviceIdentity(SerialSeed());
  c.Expect(derived.ok(), "a serial derives an identifier: " + derived.message);

  const std::string id = derived.value.client_device_identifier;
  c.Expect(auth::IsDeviceIdentifier(id), "and it has the documented shape: " + id);
  c.ExpectEq(id.size(), auth::kDeviceIdentifierLength, "the documented length");
  c.Expect(id.find(kSerial) == std::string::npos, "the serial is not in it");
  c.Expect(id != kSerial, "and it is not the serial");
  c.Expect(derived.value.source == auth::IdentitySource::kDerived, "recorded as derived");

  // Stable: the same console derives the same value, on this build and on any
  // other. Pinned to a literal rather than to a second call, because a
  // derivation that changed between releases would re-register every console in
  // the world and a self-comparison would not notice.
  c.ExpectEq(id,
             std::string(auth::kDeviceIdentifierPrefix) +
                 crypto::Sha256Hex(std::string(auth::kIdentitySalt) + '\0' + kSerial, 16),
             "salted, and derived the documented way");

  // Different consoles, different identifiers.
  auth::IdentitySeed other = SerialSeed();
  other.stable += "1";
  c.Expect(auth::DeriveDeviceIdentity(other).value.client_device_identifier != id,
           "another console derives another identifier");
}

/// The fallback, for a platform that offers nothing stable. Unlinkable to the
/// hardware, and refused outright when what it is handed is not entropy.
void MintsFromEntropyWhenThereIsNoSerial(checks::Checks& c) {
  const auth::DerivedIdentity minted = auth::DeriveDeviceIdentity(EntropySeed('x'));
  c.Expect(minted.ok(), "entropy mints an identifier: " + minted.message);
  c.Expect(auth::IsDeviceIdentifier(minted.value.client_device_identifier), "of the same shape");
  c.Expect(minted.value.source == auth::IdentitySource::kRandom, "recorded as random");
  c.Expect(auth::DeriveDeviceIdentity(EntropySeed('y')).value.client_device_identifier !=
               minted.value.client_device_identifier,
           "different entropy, different identifier");

  auth::IdentitySeed thin;
  thin.entropy = std::string(auth::kMinimumEntropyBytes - 1, 'x');
  const auth::DerivedIdentity refused = auth::DeriveDeviceIdentity(thin);
  c.Expect(!refused.ok(), "too little entropy is refused rather than stretched");
  c.ExpectEq(std::string(auth::ToString(refused.error)), std::string("no_seed"), "as no_seed");

  const auth::DerivedIdentity nothing = auth::DeriveDeviceIdentity({});
  c.Expect(!nothing.ok(), "and an empty seed is refused");
  c.ExpectEq(std::string(auth::ToString(nothing.error)), std::string("no_seed"), "as no_seed");

  // The failure that would be worse than a duplicate device: a platform layer
  // substituting a short placeholder for a serial it could not read. Every
  // console doing that derives the *same* identifier, RomM treats them as one
  // device, and one console's saves overwrite another's. A real serial is
  // fourteen characters, so nothing legitimate is caught here.
  auth::IdentitySeed placeholder;
  placeholder.stable = std::string(auth::kMinimumStableChars - 1, 'X');
  const auth::DerivedIdentity refused_stable = auth::DeriveDeviceIdentity(placeholder);
  c.Expect(!refused_stable.ok(), "a stable value too short to be a serial is refused");
  c.ExpectEq(std::string(auth::ToString(refused_stable.error)), std::string("no_seed"),
             "as no_seed");
  c.Expect(refused_stable.message.find(placeholder.stable) == std::string::npos,
           "and the message does not quote it, because it is the serial");

  // ...and a short stable value does not silently poison an otherwise fine
  // entropy seed either: the entropy is used, and the result is a random one.
  auth::IdentitySeed fallback = EntropySeed('e');
  fallback.stable = std::string(auth::kMinimumStableChars - 1, 'X');
  const auth::DerivedIdentity fell_back = auth::DeriveDeviceIdentity(fallback);
  c.Expect(fell_back.ok(), "a short stable value falls back to entropy: " + fell_back.message);
  c.Expect(fell_back.value.source == auth::IdentitySource::kRandom, "recorded as random");

  // A stable value wins when there is one, because a derived identifier
  // survives losing the file and a random one does not.
  auth::IdentitySeed both = EntropySeed('x');
  both.stable = kSerial;
  c.ExpectEq(auth::DeriveDeviceIdentity(both).value.client_device_identifier,
             auth::DeriveDeviceIdentity(SerialSeed()).value.client_device_identifier,
             "a stable value is preferred to entropy");
}

/// Written once, read every time after. The acceptance criterion: stable across
/// restarts.
void PersistsAndIsReadBack(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("stable.dat");

  const auth::IdentityResult first = auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(first.ok(), "the first call mints one: " + first.message);
  c.Expect(first.created, "and says it created it");
  c.Expect(std::filesystem::exists(path), "and there is a file");
  c.Expect(!std::filesystem::exists(io::TempPathFor(path.string())),
           "with no temp file left beside it");

  const auth::IdentityResult again = auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(again.ok(), "the next boot reads it back: " + again.message);
  c.Expect(!again.created, "and says it did not create it");
  c.ExpectEq(again.value.client_device_identifier, first.value.client_device_identifier,
             "the same identifier across restarts");

  // The file wins over the seed. A console that gains a serial after having
  // been given a random identifier keeps the random one -- preferring the seed
  // here is exactly how one console becomes two in RomM's device list.
  const auth::IdentityResult reseeded =
      auth::LoadOrCreateDeviceIdentity(path.string(), EntropySeed('z'));
  c.ExpectEq(reseeded.value.client_device_identifier, first.value.client_device_identifier,
             "a different seed does not change a persisted identifier");

  // The serial is not on the SD card either, only its hash.
  c.Expect(ReadWhole(path).find(kSerial) == std::string::npos,
           "and the serial is not in the file");
}

/// The one event this file exists for. "Re-pair" discards the token; it must
/// not discard the identifier, or RomM registers the console a second time.
void SurvivesARePair(checks::Checks& c) {
  const std::filesystem::path directory = ScratchDir() / "repair";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path identity = directory / auth::kDeviceIdentityFileName;
  const std::filesystem::path token = directory / "token.dat";

  const auth::IdentityResult before =
      auth::LoadOrCreateDeviceIdentity(identity.string(), SerialSeed());
  c.Expect(before.ok(), "paired once: " + before.message);
  WriteWhole(token, "a token record");

  c.Expect(auth::DiscardToken(token.string()), "re-pair discards the token");
  c.Expect(!std::filesystem::exists(token), "the token is gone");
  c.Expect(std::filesystem::exists(identity), "and the identifier is not");

  const auth::IdentityResult after =
      auth::LoadOrCreateDeviceIdentity(identity.string(), SerialSeed());
  c.Expect(!after.created, "the re-pair reads the identifier rather than minting one");
  c.ExpectEq(after.value.client_device_identifier, before.value.client_device_identifier,
             "so RomM sees the same console");
}

/// The commit window: `WriteAtomically` leaves `path` missing and the record
/// under `.old` for one moment. Minting there would be a second device.
void RecoversFromTheCommitWindow(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("window.dat");
  const std::filesystem::path previous = io::PreviousPathFor(path.string());

  const auth::IdentityResult first = auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(first.ok(), "the identifier is written: " + first.message);

  // Exactly the state an interruption between the two renames leaves behind.
  std::filesystem::rename(path, previous);
  const auth::IdentityResult recovered =
      auth::LoadOrCreateDeviceIdentity(path.string(), EntropySeed('q'));
  c.Expect(recovered.ok(), "the previous record is read rather than replaced: " + recovered.message);
  c.Expect(!recovered.created, "nothing was minted");
  c.ExpectEq(recovered.value.client_device_identifier, first.value.client_device_identifier,
             "and it is the same identifier");
  c.Expect(std::filesystem::exists(path),
           "and it is put back under its real name rather than left in the window");
}

/// A record that is not a record. There is nothing left in it to preserve, so
/// it is replaced -- but a file that could not be *read* is a different thing,
/// and minting over one would be unrecoverable.
void ReplacesRubbishButNotAnUnreadableFile(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("rubbish.dat");
  WriteWhole(path, "not json at all");
  const auth::IdentityResult replaced =
      auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(replaced.ok(), "a corrupt record is replaced: " + replaced.message);
  c.Expect(replaced.created, "by a freshly minted one");
  c.Expect(auth::IsDeviceIdentifier(replaced.value.client_device_identifier), "of the right shape");

  // Right JSON, wrong value. RomM would accept whatever this is, and the
  // console would then be whatever the corruption made it.
  WriteWhole(path, R"({"client_device_identifier":"nx-not-hex","source":"derived"})");
  const auth::DerivedIdentity parsed =
      auth::ParseDeviceIdentity(R"({"client_device_identifier":"nx-not-hex","source":"derived"})");
  c.Expect(!parsed.ok(), "an identifier of the wrong shape is refused");
  c.ExpectEq(std::string(auth::ToString(parsed.error)), std::string("malformed"), "as malformed");
  c.Expect(auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed()).created,
           "and the file holding it is replaced");

  // A directory where the record should be: `fopen` succeeds and the read
  // fails, which is the shape of a card having a bad moment. Minting here would
  // overwrite an identifier that may be perfectly fine.
  const std::filesystem::path unreadable = FreshPath("unreadable.dat");
  std::filesystem::create_directories(unreadable);
  const auth::IdentityResult refused =
      auth::LoadOrCreateDeviceIdentity(unreadable.string(), SerialSeed());
  c.Expect(!refused.ok(), "a file that cannot be read is not answered by minting");
  c.ExpectEq(std::string(auth::ToString(refused.error)), std::string("unreadable"), "as unreadable");
  c.Expect(refused.value.client_device_identifier.empty(), "and hands back no identifier");
  std::filesystem::remove_all(unreadable);

  // The other half, and the one that actually bites: a path that *exists* and
  // whose `fopen` fails. On Horizon that is a full handle table or `sdmc:` not
  // mounted yet at boot; here it is a symlink loop, which is the one way to get
  // a non-ENOENT open failure without depending on who is running the test.
  // Reporting this as "there is no file" would mint over a perfectly good
  // identifier and register the console in RomM a second time.
  const std::filesystem::path loop = FreshPath("loop.dat");
  std::error_code ignored;
  std::filesystem::create_symlink(loop.filename(), loop, ignored);
  if (ignored) {
    c.Expect(false, "the test can create a symlink loop: " + ignored.message());
  } else {
    const io::ReadResult read = io::ReadFile(loop.string());
    c.Expect(!read.ok(), "a path that will not open is an error");
    c.ExpectEq(std::string(io::ToString(read.error)), std::string("unreadable"),
               "reported as unreadable, not as missing");

    const auth::IdentityResult unopenable =
        auth::LoadOrCreateDeviceIdentity(loop.string(), SerialSeed());
    c.Expect(!unopenable.ok(), "and it is not answered by minting either");
    c.ExpectEq(std::string(auth::ToString(unopenable.error)), std::string("unreadable"),
               "as unreadable");
  }
  std::filesystem::remove(loop, ignored);

  // ...while a genuinely absent file is still `kMissing`, which is what keeps
  // first boot working at all.
  const io::ReadResult absent = io::ReadFile((ScratchDir() / "nothing-here.dat").string());
  c.ExpectEq(std::string(io::ToString(absent.error)), std::string("missing"),
             "a file that is not there is missing");
  const io::ReadResult nowhere =
      io::ReadFile((ScratchDir() / "no-such-directory" / "device.dat").string());
  c.ExpectEq(std::string(io::ToString(nowhere.error)), std::string("missing"),
             "and so is one under a directory that is not there");
}

/// An identifier that was not persisted is a different identifier next boot, so
/// it is an error rather than a value.
void RefusesAnIdentifierItCouldNotPersist(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("unpersistable.dat");
  // A directory where the temp file wants to be, so the write fails at its
  // first step -- the same lever `core.token_store` uses.
  std::filesystem::create_directories(io::TempPathFor(path.string()));

  const auth::IdentityResult failed =
      auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(!failed.ok(), "an identifier that could not be written is an error");
  c.ExpectEq(std::string(auth::ToString(failed.error)), std::string("persist_failed"),
             "as persist_failed");
  c.Expect(failed.value.client_device_identifier.empty(),
           "and no identifier is handed out to pair with");
  c.Expect(failed.message.find(path.string()) != std::string::npos, "the message names the path");
  std::filesystem::remove_all(io::TempPathFor(path.string()));

  const std::filesystem::path missing = ScratchDir() / "no-such-directory" / "device.dat";
  const auth::IdentityResult nowhere =
      auth::LoadOrCreateDeviceIdentity(missing.string(), SerialSeed());
  c.Expect(!nowhere.ok(), "and a missing directory is named rather than silently ignored");
  c.ExpectEq(std::string(auth::ToString(nowhere.error)), std::string("persist_failed"),
             "as persist_failed");
}

/// The record itself, both directions.
void RoundTripsTheRecord(checks::Checks& c) {
  const auth::DerivedIdentity derived = auth::DeriveDeviceIdentity(SerialSeed());
  const std::string text = auth::SerializeDeviceIdentity(derived.value);
  c.Expect(text.find('\n') == std::string::npos, "the record is one line");

  const auth::DerivedIdentity back = auth::ParseDeviceIdentity(text);
  c.Expect(back.ok(), "it parses: " + back.message);
  c.ExpectEq(back.value.client_device_identifier, derived.value.client_device_identifier,
             "the identifier survives");
  c.Expect(back.value.source == auth::IdentitySource::kDerived, "and so does the source");

  // A later build naming a source this one does not know still yields its
  // identifier: the identifier is the thing that has to survive, and refusing
  // the record over a decorative field would mint a second device.
  const auth::DerivedIdentity future = auth::ParseDeviceIdentity(
      std::string(R"({"client_device_identifier":")") + derived.value.client_device_identifier +
      R"(","source":"attested"})");
  c.Expect(future.ok(), "an unknown source is not fatal: " + future.message);
  c.ExpectEq(future.value.client_device_identifier, derived.value.client_device_identifier,
             "and the identifier is still read");
  c.Expect(future.value.source == auth::IdentitySource::kUnknown, "reported as unknown");

  const auth::DerivedIdentity headless =
      auth::ParseDeviceIdentity(R"({"source":"derived"})");
  c.Expect(!headless.ok(), "a record with no identifier is refused");
  c.ExpectEq(std::string(auth::ToString(headless.error)), std::string("malformed"), "as malformed");

  const std::string id = derived.value.client_device_identifier;
  c.Expect(!auth::IsDeviceIdentifier(id.substr(0, id.size() - 1)), "one character short is refused");
  c.Expect(!auth::IsDeviceIdentifier(id + "0"), "one character long is refused");
  c.Expect(!auth::IsDeviceIdentifier("sw-" + id.substr(3)), "the wrong prefix is refused");
  c.Expect(!auth::IsDeviceIdentifier(std::string(auth::kDeviceIdentifierPrefix) +
                                     std::string(auth::kDeviceIdentifierHexChars, 'A')),
           "uppercase hex is refused, so the value is one shape and not two");
  c.Expect(!auth::IsDeviceIdentifier(""), "and so is nothing at all");
}

/// A factory reset, which is the one thing that may change the identifier.
void ForgettingIsDeliberate(checks::Checks& c) {
  const std::filesystem::path path = FreshPath("forget.dat");
  const auth::IdentityResult first = auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(first.ok(), "there is an identifier: " + first.message);

  c.Expect(auth::ForgetDeviceIdentity(path.string()), "forgetting removes it");
  c.Expect(!std::filesystem::exists(path), "the file is gone");
  c.Expect(auth::ForgetDeviceIdentity(path.string()), "and forgetting nothing still succeeds");

  // A *derived* identifier comes back the same after a wipe, which is the
  // reason a stable seed is preferred to entropy in the first place.
  const auth::IdentityResult after = auth::LoadOrCreateDeviceIdentity(path.string(), SerialSeed());
  c.Expect(after.created, "the next boot mints one again");
  c.ExpectEq(after.value.client_device_identifier, first.value.client_device_identifier,
             "and a serial derives its way back to the same one");
}

}  // namespace

int main() {
  checks::Checks c;
  Sha256MatchesTheKnownVectors(c);
  HidesTheSerialItCameFrom(c);
  MintsFromEntropyWhenThereIsNoSerial(c);
  PersistsAndIsReadBack(c);
  SurvivesARePair(c);
  RecoversFromTheCommitWindow(c);
  ReplacesRubbishButNotAnUnreadableFile(c);
  RefusesAnIdentifierItCouldNotPersist(c);
  RoundTripsTheRecord(c);
  ForgettingIsDeliberate(c);
  return c.failures() == 0 ? 0 : 1;
}

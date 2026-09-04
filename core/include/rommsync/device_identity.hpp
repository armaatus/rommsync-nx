// `client_device_identifier`: one console, one value, for the life of the SD.
//
// RomM keys a device on what the client sends at `POST /api/auth/device/init`
// (docs/AUTH.md). Send a different value on a re-pair and RomM does not
// recognise the console -- it registers a *second* device, and every save on it
// starts with an empty sync history. So the identifier has to survive reboots,
// app updates, and above all a re-pair, which is the one moment the client
// deliberately throws credentials away.
//
// That is why it does not live in `token.dat`. "Re-pair" discards the token
// (`DiscardToken`), and an identifier discarded with it would be re-derived as
// a new one on the next pairing -- exactly the duplicate this exists to
// prevent. It gets its own file, written once, next to the token but not inside
// it.
//
// What it must not be is the console serial. The serial identifies the hardware
// and, through a warranty record, a person; RomM needs neither. What RomM needs
// is a value that is the same every time and different on another console, and
// a hash of the serial is that (docs/SECURITY.md).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace rommsync::auth {

/// The file the identifier lives in, relative to `sdmc:/config/rommsync/`.
inline constexpr const char* kDeviceIdentityFileName = "device.dat";

/// Domain separation for the derivation, versioned so a future change of shape
/// is a different value rather than a silent collision with this one.
///
/// It is *not* a secret and cannot be: this repository is public, so anyone
/// holding a list of serials can hash them against this salt and recognise a
/// console. What it buys is that the same serial hashed for some other purpose,
/// by us or by anyone else, does not produce the same string -- so this
/// identifier is not a join key across systems. The property that actually
/// protects the user is that the serial never leaves the console at all.
inline constexpr const char* kIdentitySalt = "rommsync-nx/client-device-identifier/v1";

/// `nx-` and 32 lowercase hex characters: 128 bits, which is far past any
/// collision worry for one person's device list, and short enough to read back
/// off RomM's UI. The prefix is there so a human looking at that list can tell
/// what wrote the row.
inline constexpr const char* kDeviceIdentifierPrefix = "nx-";
inline constexpr std::size_t kDeviceIdentifierHexChars = 32;
/// Derived, not written out: a prefix of a different length with a hardcoded 3
/// here would make `IsDeviceIdentifier` reject every value this module
/// produces, which reads back as "every `device.dat` in the field is corrupt"
/// and mints a new identifier on every console.
inline constexpr std::size_t kDeviceIdentifierLength =
    std::string_view(kDeviceIdentifierPrefix).size() + kDeviceIdentifierHexChars;

/// Where an identifier came from. Recorded because it is the first thing worth
/// knowing when a console shows up twice in RomM, and it is the one field of
/// this record a human ever reads.
enum class IdentitySource {
  kDerived,  ///< hashed from a value the platform says is stable for this console
  kRandom,   ///< minted from entropy, because the platform offered nothing stable
  /// Read back from a record that named a source this build does not know.
  /// Never produced by `DeriveDeviceIdentity`. It exists so that a file written
  /// by a later version still yields its identifier: the identifier is what has
  /// to survive, and refusing the record over a decorative field would mint a
  /// new one and duplicate the console in RomM.
  kUnknown,
};

/// Stable, log-friendly name. Never null.
const char* ToString(IdentitySource source);

/// The record `device.dat` holds, one JSON object.
struct DeviceIdentity {
  std::string client_device_identifier;
  IdentitySource source = IdentitySource::kRandom;
};

/// What the platform can offer to derive from. Filled in by the layer that is
/// allowed to include a libnx header; `core/` never is.
///
/// On Horizon `stable` is the console serial (`setsysGetSerialNumber`) and
/// `entropy` comes from the CSRNG. On the host both come from the caller, which
/// is what makes every branch below testable without a console.
struct IdentitySeed {
  /// The same on every boot of this console, different on another one. Never
  /// sent anywhere and never stored: only its hash leaves this function.
  ///
  /// At least `kMinimumStableChars`, or the derivation is refused. That floor
  /// is not a quality check and cannot be one -- this module cannot tell a
  /// serial from a placeholder -- but it does catch the shape the mistake
  /// usually takes: a platform layer that hands over `""`, a truncated value,
  /// or a short constant when `setsysGetSerialNumber` failed. Every console
  /// affected would derive the *same* identifier, and RomM would treat them as
  /// one device and let one console's saves overwrite another's. **The platform
  /// layer must fail rather than substitute.**
  std::string stable;

  /// Unpredictable bytes, used only when there is no stable value. At least
  /// `kMinimumEntropyBytes` of them, or the derivation is refused -- an
  /// identifier minted from four guessable bytes is one two consoles can share.
  std::string entropy;
};

/// Below this, `entropy` is not entropy.
inline constexpr std::size_t kMinimumEntropyBytes = 16;

/// Below this, `stable` is a placeholder rather than a console value. A Switch
/// serial is fourteen characters; this is well under it, so a real one is never
/// refused.
inline constexpr std::size_t kMinimumStableChars = 8;

/// Why deriving or persisting the identifier did not work.
enum class IdentityError {
  kNone,
  kNoSeed,         ///< no usable stable value and not enough entropy: nothing to derive from
  kMalformed,      ///< the text read back is not an identity record
  kPersistFailed,  ///< it was derived and could not be written; see `message`
  kUnreadable,     ///< the file exists and could not be read -- do *not* mint over it
};

/// Stable, log-friendly name. Never null.
const char* ToString(IdentityError error);

/// Derive an identifier without touching the disk.
///
/// Prefers `stable` when there is one long enough to be a console value,
/// because a derived identifier survives
/// losing `device.dat` -- a wiped `config/` folder re-derives the same value and
/// RomM still recognises the console, where a random one would not. Falls back
/// to `entropy`, which is unlinkable to the hardware but only as durable as the
/// file it is stored in.
///
/// Deterministic for a given `stable`: same console, same answer, on any build.
struct DerivedIdentity {
  DeviceIdentity value{};
  IdentityError error = IdentityError::kNone;
  std::string message;
  bool ok() const { return error == IdentityError::kNone; }
};
DerivedIdentity DeriveDeviceIdentity(const IdentitySeed& seed);

/// Whether `text` has the exact shape this module produces. A value that does
/// not is refused rather than sent: RomM would accept it, and the console would
/// then be whatever the corruption made it.
bool IsDeviceIdentifier(std::string_view text);

/// The record as one line of JSON, and back.
std::string SerializeDeviceIdentity(const DeviceIdentity& identity);
DerivedIdentity ParseDeviceIdentity(std::string_view text);

struct IdentityResult {
  DeviceIdentity value{};
  IdentityError error = IdentityError::kNone;
  std::string message;

  /// True when this call minted the identifier rather than reading it back.
  /// Worth logging exactly once: on any boot after the first it is a signal
  /// that something removed the file.
  bool created = false;

  bool ok() const { return error == IdentityError::kNone; }
};

/// The identifier at `path`, minting and persisting one if there is none.
///
/// **A file that reads back wins, always.** Not the seed, even when the seed
/// would derive something different: a console that gains a stable serial after
/// having been given a random identifier must keep the random one, or it turns
/// into two devices in RomM. The seed is consulted only when there is nothing on
/// disk.
///
/// A file that cannot be *read* is `kUnreadable` and mints nothing -- a
/// transient read error is not evidence that no identifier exists, and minting
/// over one is unrecoverable. A file that reads back and is not an identity
/// record is replaced, because there is nothing in it left to preserve.
///
/// A mint that cannot be persisted is `kPersistFailed` with no value: an
/// identifier that is not on disk is a different one next boot, so pairing with
/// it would create the duplicate this module exists to avoid.
IdentityResult LoadOrCreateDeviceIdentity(const std::string& path, const IdentitySeed& seed);

/// Remove the identifier. Not part of "Re-pair" -- re-pairing keeps it, which is
/// the point of the file. This is for a factory reset, and for tests.
bool ForgetDeviceIdentity(const std::string& path);

}  // namespace rommsync::auth

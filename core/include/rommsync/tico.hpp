// Where Tico keeps saves and states on this card: its layout before 0.8.0, or
// 0.8.0's per-account one -- and, for the second, whose (#231).
//
// Tico 0.8.0 moved every module's saves from `<root>/<slug>/` to
// `<root>/users/<uid>/<slug>/`, where `<uid>` is the Switch `AccountUid` as 32
// lowercase hex digits; states moved the same way under their own root (#226
// R5). A custom root gets `users/<uid>/` appended too, so the rule is about the
// folder's parent, not about `/tico/saves`: a Tico folder `<root>/<slug>` in the
// folder map is read at `<root>/users/<uid>/<slug>` once `<root>/users/` exists.
//
// That is decided when the card is read, every tick, and never written into
// `config.ini`. The folders in the map keep meaning "Tico's folder for this
// system", in whichever layout the installed Tico uses -- so a file v1.0.0-rc1
// wrote with `/tico/saves/snes` in it still means what it meant, and needs no
// new config version (#205).
//
// **Several accounts are never guessed between.** Each is one person's
// progress, and syncing the wrong one is someone else's save uploaded over
// yours on the next device. `[tico] account` names one; until it does, nothing
// under that `users/` is synced, and `Notes` says which accounts there are.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/config.hpp"
#include "rommsync/file_system.hpp"

namespace rommsync::tico {

/// The folder per-account saves live under, beside the per-system folders.
inline constexpr const char* kUsersFolder = "users";

/// How many accounts one `users/` listing is read for. A Switch holds at most
/// eight; more is a card something else wrote, and the list is only ever
/// shown, so the rest are counted rather than kept.
inline constexpr std::size_t kMaxAccounts = 8;

/// Which layout one Tico root is read in.
enum class Layout {
  kLegacy,     ///< `<root>/<slug>/`: no `users/`, an empty one, or `account = legacy`
  kAccount,    ///< `<root>/users/<uid>/<slug>/`
  kUndecided,  ///< several accounts and none named, or a `users/` that would not list
};

/// One root, resolved.
struct Resolution {
  Layout layout = Layout::kLegacy;

  /// `<root>/users`.
  std::string users_dir;

  /// The account read, for `kAccount`: lowercase, as Tico names its folder.
  std::string uid;

  /// The accounts `users/` holds, sorted, at most `kMaxAccounts` -- only
  /// listed under `[tico] account = auto` or a named one. Folders whose name is
  /// not an account id are not in it.
  std::vector<std::string> accounts;

  /// Accounts past `kMaxAccounts`, counted and not kept.
  std::size_t accounts_dropped = 0;

  /// For `kUndecided`, why, in a sentence for the log and the overlay.
  std::string why;
};

/// The root a Tico folder from the map sits in -- `/tico/saves` for
/// `/tico/saves/snes` -- or "" when `folder` is not a Tico folder (`scan::SourceOf`)
/// or has no parent to hold a `users/`.
std::string RootOf(std::string_view folder);

/// Where Tico keeps `folder` (a map folder `RootOf` answers for) in `resolved`'s
/// layout: `<root>/users/<uid>/<slug>` for `kAccount`, `folder` itself for
/// `kLegacy`, "" for `kUndecided`.
std::string FolderIn(const Resolution& resolved, std::string_view folder);

/// How `root` -- the parent of a Tico folder the map lists, `/tico/saves` --
/// is laid out under `tico`. Lists `<root>/users` at most once; never for
/// `account = legacy`.
Resolution Resolve(const config::TicoConfig& tico, fs::FileSystem& files, std::string_view root);

/// The Tico roots `config` reads: the parent of every Tico folder in its save
/// map, and in its state map when `sync.states` is on. Deduplicated ignoring
/// case, as FAT32 does, in map order.
std::vector<std::string> Roots(const config::Config& config);

/// What the user should know about whose saves are read: which account `auto`
/// settled on (a notice), that several accounts wait for `[tico] account` to
/// name one, or that the named one has no folder (warnings, listing the
/// accounts there are). Shown by the overlay beside `config.ini`'s own
/// diagnostics, which is where a user picks one. Empty while Tico's layout is
/// the one before 0.8.0.
std::vector<config::Diagnostic> Notes(const config::Config& config, fs::FileSystem& files);

}  // namespace rommsync::tico

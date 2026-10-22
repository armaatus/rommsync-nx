// Which RomM this console is talking to, and what that RomM can do (M10-2, #228).
//
// RomM publishes no capability endpoint. What it does publish, unauthenticated
// and on every release this client has met, is `GET /api/heartbeat` with
// `SYSTEM.VERSION` in it -- 5.2.0 as well as 5.4.0 (#227's captures) -- so every
// feature that differs between releases is inferred from that one string, the
// way Grout infers them (#226 R1, "Version detection"). This file is the one
// place that inference is written down: a behaviour that depends on the server
// asks `ServerCapabilities`, never the version text.
//
// Three things here are decisions rather than facts:
//
//   - **5.4.0 is the floor** (D1 in #226). Below it sync is refused outright,
//     because what a 5.2 or 5.3 server lacks -- `delete`, the `emulators`
//     filter, hash baselines -- is what keeps cross-device sync from silently
//     superseding another device's save. Every flag is therefore true on any
//     server that passes the floor; the struct exists so the next RomM release
//     has somewhere to land, not because a false flag has a code path.
//   - **A pre-release counts as its release.** `5.4.0-beta.1` reads as 5.4.0:
//     a beta carries the features its release notes are written about, and
//     refusing the people who test RomM's betas would be refusing exactly the
//     servers most likely to be newer than this build.
//   - **A version that is missing or will not parse reads as the newest**,
//     which is the rule `server/testing/provision.py` follows for the same
//     server. A RomM built from source reports a placeholder rather than a
//     number, and every RomM that ever shipped a heartbeat has carried the
//     field -- so an absent one is far likelier to be a newer server than a
//     5.2. It is said in the log (`server.version`), never silently assumed.
//
// **The parse is shallow, and bounded by the member it reads, not the body.**
// The heartbeat is the server's whole configuration -- metadata sources, every
// platform folder, scheduled tasks, OIDC -- and grows with the library, and a
// parsed `json::Value` weighs about eleven times its text (#217). So the body
// is walked, not parsed: only the `SYSTEM` member's text is handed to
// `json::Parse`, and that slice is refused past `kMaxSystemBytes`. What this
// costs on the heap is the body the transport already holds plus a document of
// three fields, whatever else the server grows.
#pragma once

#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "rommsync/auth.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"

namespace rommsync::server {

/// A RomM release number. Ordered the way releases are.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;

  auto operator<=>(const Version&) const = default;
};

/// The oldest RomM this build syncs with (D1, #226).
inline constexpr Version kMinimumVersion{5, 4, 0};

/// The releases that changed what a client can ask for (#226 R1). Named for
/// the release rather than for one feature, because each brought several:
/// 5.3 `rom_ids` and the slot cap, 5.4 everything else `ServerCapabilities`
/// lists.
inline constexpr Version kRelease53{5, 3, 0};
inline constexpr Version kRelease54{5, 4, 0};

/// The longest version string this client keeps, and so carries to the overlay.
/// A real one is `5.4.0-beta.1`; anything longer is a server answering
/// something that is not a version, and is cut rather than carried whole.
inline constexpr std::size_t kMaxVersionBytes = 32;

/// The largest `SYSTEM` member this client will parse. RomM's is three fields
/// and about eighty bytes; the bound is what keeps "a member of three fields"
/// true of a server that sends something else under the name.
inline constexpr std::size_t kMaxSystemBytes = 4 * 1024;

/// `5.4.0`, `5.4.0-beta.1`, `5.4`, `v5.4.0` and `5.4.0+abc` all read as 5.4.0.
/// Anything else -- empty, `<version>`, `development` -- is nullopt.
std::optional<Version> ParseVersion(std::string_view text);

/// `5.4.0`. What a log line and the overlay's hint say about the floor.
std::string ToString(const Version& version);

/// What one server can do, and the version it was inferred from.
///
/// Every behaviour that depends on the server's release asks one of these
/// fields -- #232's `overwrite=false`, #233's `delete`, #234's `content_hash`,
/// #245's device `capabilities` -- so that the day a release changes one,
/// `CapabilitiesFor` is the single edit.
struct ServerCapabilities {
  /// The version as the server spelled it, cut to `kMaxVersionBytes`. Empty
  /// when the heartbeat carried none.
  std::string version;

  /// `version` parsed. False is the "reads as the newest" case in the header.
  bool known = false;

  /// At or above `kMinimumVersion` -- or not known, which reads as newest.
  /// False is the refusal: no negotiate, no transfer (`sync::TickOutcome::
  /// kUnsupported`).
  bool supported = false;

  bool rom_ids = false;              ///< 5.3: negotiate and the save list take `rom_ids`
  bool slot_cap = false;             ///< 5.3: `MAX_SAVES_PER_SLOT` prunes each slot
  bool delete_action = false;        ///< 5.4: negotiate answers `delete`
  bool emulators_filter = false;     ///< 5.4: negotiate takes `emulators`
  bool restore_unlisted = false;     ///< 5.4: negotiate takes `restore_unlisted`
  bool hash_baselines = false;       ///< 5.4: `content_hash` records per-device baselines
  bool device_capabilities = false;  ///< 5.4: `POST /api/devices` takes `capabilities`

  /// `DEVICE_INSTALL.ENABLED` in the heartbeat: the server's web UI offers
  /// "Install on device", and `/api/devices/{id}/installs` answers (#245).
  ///
  /// **Configuration, not a release feature**, so it is the one field
  /// `CapabilitiesFor` never sets: only `ReadServer`, from the body it read. And
  /// it never fails a read -- a member that is missing or will not walk is
  /// "off", because an optional feature must not cost a console its sync.
  bool install_enabled = false;
};

/// The capabilities of a server reporting `version_text`. Never fails: see the
/// header for what an unparseable version means.
///
/// `floor` decides `supported` and nothing else. It is `kMinimumVersion` for
/// every caller but a test that raises it past the fixture to see a refusal
/// (`SdEngine::UseMinimumServerVersion`).
ServerCapabilities CapabilitiesFor(std::string_view version_text,
                                   const Version& floor = kMinimumVersion);

/// The same, for a release this client names itself -- `kMinimumVersion`, as
/// the capabilities a caller with no server to ask is assumed to have.
ServerCapabilities CapabilitiesFor(const Version& version);

/// `SYSTEM.VERSION` out of a heartbeat body, without parsing the rest of it.
///
/// `value` is empty, and `ok()`, when the body is a JSON object that carries no
/// `SYSTEM.VERSION` -- the missing-field case, which is not an error. It is an
/// error when the body is not a JSON object at all, when it ends mid-value, or
/// when `SYSTEM` is not an object of at most `kMaxSystemBytes` whose `VERSION`,
/// if present, is a string or null.
auth::Parsed<std::string> ParseHeartbeatVersion(std::string_view body);

/// `DEVICE_INSTALL.ENABLED` out of a heartbeat body (#245).
///
/// **Walked, not parsed, and so bounded by nothing.** #228 asked for a bound on
/// this member like `kMaxSystemBytes`, because `EXCLUDED_PLATFORM_SLUGS` grows
/// with the server's configuration. A bound is what keeps a *parse* from
/// costing heap; this one parses nothing -- the walk crosses the member to find
/// `ENABLED` and holds no part of it -- so the list costs no heap at any length,
/// and a bound could only turn a long list into a remote install that silently
/// stopped working. The platforms it excludes are the server's to enforce: it
/// refuses to queue one (`POST .../installs` answers 400).
///
/// `value` is false, and `ok()`, when there is no `DEVICE_INSTALL` or no
/// `ENABLED` in it, or `ENABLED` is null. It is an error when the body is not a
/// heartbeat object, when `DEVICE_INSTALL` is not an object, appears twice, or
/// holds an `ENABLED` that is not `true`, `false` or `null`.
auth::Parsed<bool> ParseHeartbeatInstall(std::string_view body);

/// What asking the server came to.
struct Probe {
  ServerCapabilities capabilities;

  /// Set when the request never produced a response.
  http::Error transport = http::Error::kNone;

  /// The status of a response that was not a 2xx. Zero otherwise.
  int status = 0;

  /// The complaint about a 2xx body that was not a heartbeat.
  json::Error shape;

  /// One sentence for the log. Never carries a URL credential.
  std::string message;

  /// Requests spent, including the first.
  int attempts = 0;

  bool ok() const { return transport == http::Error::kNone && status == 0 && shape.ok(); }

  /// Worth asking again on the next tick rather than reporting as a server
  /// this client does not recognise: no response, or one a proxy or a RomM
  /// having a bad minute gives (a 5xx, 408 or 429).
  bool transient() const;
};

/// `GET <base_url>/api/heartbeat`, unauthenticated, under `policy`.
///
/// Retried with backoff on a transient failure, as every network call here is
/// (CLAUDE.md); a 4xx or a body that is not a heartbeat is not retried, because
/// it does not change on a second attempt. `policy.cancel` ends it at the next
/// boundary.
Probe ReadServer(http::HttpClient& client, std::string_view base_url,
                 const sync::CallPolicy& policy = {},
                 const Version& floor = kMinimumVersion);

}  // namespace rommsync::server

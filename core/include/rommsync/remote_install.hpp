// "Install on device" from RomM's web UI, polled rather than pushed (M10-19, #245).
//
// RomM 5.4 lets a user pick a console in the web UI and queue a rom for it. The
// server holds the request until the device takes it, and offers two ways to
// hear about one: a Socket.IO event, and `POST /api/devices/{id}/installs/claim`.
// This client uses only the second, once a tick. A socket client in a sysmodule
// is a second connection held open for the whole boot, out of a heap of 1.4 MiB
// and a session count Horizon rations (docs/DEVELOPMENT.md); a claim is one
// request on a thread that is making requests anyway. A rom queued in the
// browser therefore starts at the next tick rather than at once -- the price,
// and the only one.
//
// Three exchanges, all on the device's own paired token:
//
//   - **Register.** `PUT /api/devices/{id}` with `{"capabilities":
//     {"remote_install": true}}`. Without it the web UI does not offer the
//     console, and `POST .../installs` answers 400 "does not accept remote
//     installs". A `PUT`, not `POST /api/devices` with `capabilities`, because
//     that call creates a second device however it is asked
//     (device_registration.hpp) -- and pairing already made this one.
//   - **Claim.** `POST /api/devices/{id}/installs/claim` takes every pending
//     request and answers **every request the device holds taken**, oldest
//     first -- the ones taken before as well as now. So a claim is idempotent:
//     a console that loses power between claiming and reporting is handed the
//     same requests again on its next claim, and nothing needs to be on the card
//     to remember them by. That is what lets the engine keep its claims in
//     memory only, and bound them: a request past `kMaxClaims` stays taken on
//     the server and comes back with the next claim.
//   - **Report.** `PUT /api/devices/{id}/installs/{request_id}` with `done`,
//     `already_installed` or `failed`, which ends the request. Only a taken
//     request can be reported; one the user cancelled in the web UI meanwhile,
//     or one already ended, answers 404, and so does one that expired
//     (`DEVICE_INSTALL_REQUEST_TTL_DAYS`). All three mean "nothing left to
//     report".
//
// What happens between the claim and the report is the download queue's, and
// nothing new: the rom id goes in exactly as a press in the overlay puts it
// there (`download::Queue`), and the queue's own terminal state is the verdict
// (`VerdictFor`). So a rom installed from the browser is fetched, verified,
// placed and -- through #241 -- has its save restored exactly as one queued on
// the console would be.
//
// Verified against the 5.4.0 fixture: the device-only binding (a token bound to
// another device is a 404, not a 403, because the device is "not theirs"), the
// idempotent claim, the 422 for any report but the three, and the 404 after one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/download.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"
#include "rommsync/sync.hpp"
#include "rommsync/token_store.hpp"

namespace rommsync::install {

/// The capability RomM checks before it queues anything for a device.
inline constexpr const char* kCapability = "remote_install";

/// The requests one console tracks at once. A claim answering more keeps the
/// rest taken on the server, where the next claim returns them (see the
/// header): the bound costs a user who queued seventeen roms in one go a tick's
/// wait for the seventeenth, and nothing else. It is a heap row
/// (`kHeapInstallClaims`, sysmodule/source/main.cpp).
inline constexpr std::size_t kMaxClaims = 16;

/// The longest request id kept. RomM's are UUIDs, 36 bytes; anything longer is
/// not one, and is refused rather than pasted into a URL path.
inline constexpr std::size_t kMaxRequestIdBytes = 64;

/// `InstallReason`'s `max_length` in RomM 5.4.0. A longer reason is a 422 for
/// the whole report, so it is cut rather than sent.
inline constexpr std::size_t kMaxReasonBytes = 500;

/// One request the device holds taken: what to fetch, and what to report on.
struct Request {
  std::string id;
  std::int64_t rom_id = 0;
};

/// A claim's answer, read.
struct ClaimPage {
  /// At most the `max` asked for, oldest first as the server sends them.
  std::vector<Request> requests;

  /// Requests past `max`. Still taken on the server, and in the next claim.
  std::size_t dropped = 0;
};

/// Parse a 200 body from `POST /api/devices/{id}/installs/claim`: a bare array
/// of `InstallRequestSchema`, read one element at a time (`json::ParseEach`), so
/// a long answer costs its text and one row (#217).
///
/// Held to the house rule (core/AGENTS.md): an element without a usable `id`
/// or a positive `rom_id` fails the whole answer rather than being skipped, and
/// an id is usable only when it is at most `kMaxRequestIdBytes` of URL-path-safe
/// characters.
auth::Parsed<ClaimPage> ParseClaim(std::string_view body, std::size_t max = kMaxClaims);

/// What a device may tell RomM about a request it took.
enum class Outcome {
  kDone,
  kAlreadyInstalled,
  kFailed,
};

/// The wire spelling: `done`, `already_installed`, `failed`. Never null.
const char* ToString(Outcome outcome);

struct Verdict {
  Outcome outcome = Outcome::kFailed;

  /// Why, for `kFailed`: the queue row's own message, which never quotes a
  /// file name (`download::QueueEntry::message`). Empty otherwise.
  std::string reason;
};

/// What to report for a request whose rom's queue row is `entry`, or nullopt
/// while the row is still work to do.
///
/// `entry.rom_id == 0` is no row at all -- the user took it out of the queue on
/// the console -- and is a failure that says so: the browser is still waiting,
/// and silence would leave the request taken until it expired.
std::optional<Verdict> VerdictFor(const download::QueueEntry& entry);

/// `{"capabilities":{"remote_install":true}}`.
std::string CapabilitiesBody(bool remote_install);

/// `{"status":"failed","reason":"..."}`, the reason cut to `kMaxReasonBytes`
/// on a UTF-8 boundary. No `reason` member when it is empty.
std::string ReportBody(const Verdict& verdict);

/// What one of the three exchanges came to.
struct Exchange {
  /// Set when no response arrived.
  http::Error transport = http::Error::kNone;

  /// A response that was not a 2xx. Zero otherwise.
  int status = 0;

  /// A 2xx whose body was not what the endpoint answers.
  json::Error shape;

  /// One sentence for the log. Never carries the token.
  std::string message;

  /// Requests spent, the first included.
  int attempts = 0;

  bool ok() const { return transport == http::Error::kNone && status == 0 && shape.ok(); }

  /// Worth trying again: no response, a 5xx, 408 or 429 -- or a 409, which is
  /// how RomM 5.4.0 answers a claim or a report whose Redis transaction kept
  /// colliding with another writer (`InstallTransitionError`). A request a
  /// device has claimed is never in the wrong status for its own report, so a
  /// 409 here is that race and nothing else.
  bool transient() const;

  /// The request is not taken any more -- cancelled in the web UI, ended, or
  /// expired -- so there is nothing left to report on it. RomM answers 404 for
  /// all three.
  bool ended() const { return status == 404; }
};

struct Claimed {
  Exchange exchange;
  ClaimPage page;
};

/// `PUT <base>/api/devices/<token.device_id>` with `CapabilitiesBody`.
///
/// Retried with backoff on a transient failure, as every network call here is
/// (CLAUDE.md); anything else is answered as it came.
Exchange Register(http::HttpClient& client, std::string_view base_url,
                  const auth::StoredToken& token, bool remote_install,
                  const sync::CallPolicy& policy = {});

/// `POST <base>/api/devices/<token.device_id>/installs/claim`.
Claimed Claim(http::HttpClient& client, std::string_view base_url, const auth::StoredToken& token,
              const sync::CallPolicy& policy = {}, std::size_t max = kMaxClaims);

/// `PUT <base>/api/devices/<token.device_id>/installs/<request_id>` with
/// `ReportBody(verdict)`.
Exchange Report(http::HttpClient& client, std::string_view base_url,
                const auth::StoredToken& token, std::string_view request_id,
                const Verdict& verdict, const sync::CallPolicy& policy = {});

}  // namespace rommsync::install

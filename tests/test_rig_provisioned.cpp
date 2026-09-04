// Proves the fixture is PROVISIONED, not merely running.
//
// rig.smoke answers "is RomM up?". This answers "is RomM usable?", which is a
// different question and the one that was quietly false: seed.sh stages roms on
// disk, but nothing imported them, so RomM reported an empty library while every
// health check stayed green. A test that needs a rom then fails on an empty
// library and reads exactly like a bug in the code under test.
//
// It asserts what server/testing/provision.py is for:
//   1. the library has been scanned          -- roms are queryable, not just on disk
//   2. the curated `Handheld` collection exists                            (M0-6)
//   3. the client token it minted authenticates -- the device-code flow ran
//      to completion with no human approving anything                     (M0-6)
//
// Point 3 is the one that unblocks M0-4: before it, capturing real auth shapes
// meant a person clicking "approve" in RomM's web UI, which an agent cannot do.

#include <iostream>
#include <memory>
#include <string>

#include "rig.hpp"

namespace {

namespace http = rommsync::http;

http::Result GetAuthed(http::HttpClient& client, const std::string& url,
                       const std::string& token) {
  http::Request request;
  request.url = url;
  if (!token.empty()) {
    request.headers.push_back({"Authorization", "Bearer " + token});
  }
  return client.Send(request);
}

}  // namespace

int main() {
  const std::unique_ptr<http::HttpClient> owned = rommsync::host::MakeCurlHttpClient();
  http::HttpClient& client = *owned;
  const std::string base = rig::BaseUrl();

  if (!rig::Reachable(client, base)) {
    std::cerr << "rig unreachable at " << base
              << "\n  start it with: ./scripts/orca/compose.sh up -d\n";
    return rig::kSkip;
  }

  rig::Checks checks;

  // 1. the credentials provisioning wrote -------------------------------------
  // First, because /api/roms is an authenticated endpoint: without the token
  // every later check would read a 401 body and report an empty library.
  const std::string fixture = rig::ReadFile(ROMMSYNC_FIXTURE_AUTH);
  checks.Expect(!fixture.empty(),
                "server/testing/fixture-auth.env exists -- the fixture was provisioned "
                "(run ./.venv/bin/python server/testing/provision.py)");

  const std::string token = rig::FixtureValue(fixture, "ROMM_FIXTURE_TOKEN");
  const std::string collection_id = rig::FixtureValue(fixture, "ROMM_FIXTURE_COLLECTION_ID");
  const std::string device_id = rig::FixtureValue(fixture, "ROMM_FIXTURE_DEVICE_ID");
  checks.Expect(!token.empty(), "a client token was minted");
  checks.Expect(!collection_id.empty(), "a collection id was recorded");
  // Every sync call is scoped by device_id. An empty one here surfaces much
  // later, inside whichever sync test negotiates with it, looking like that
  // test's own bug.
  checks.Expect(!device_id.empty(), "a device_id was registered");

  if (token.empty()) {
    std::cerr << "\nno token, so nothing below can be checked; the device-code flow "
                 "never completed.\n";
    return 1;
  }

  // 2. that token really authenticates ----------------------------------------
  // The flow can produce a token-shaped string and still not have been approved,
  // so this spends it rather than trusting it.
  const http::Result me = GetAuthed(client, base + "/api/users/me", token);
  checks.ExpectOk(me, "GET /api/users/me with the fixture token");
  checks.ExpectEq(me.response.status, 200,
                  "the minted token authenticates -- device-code approval completed headlessly");

  // 3. the library was scanned ------------------------------------------------
  // Asked of RomM rather than read back from the credentials file: that file
  // records what provisioning believed, and this has to be about what the
  // server actually holds.
  const http::Result roms = GetAuthed(client, base + "/api/roms?limit=1", token);
  checks.ExpectOk(roms, "GET /api/roms");
  const std::string total = rig::JsonNumber(roms.response.body, "total");
  checks.Expect(!total.empty() && total != "0",
                "the library has been scanned -- roms are queryable, not just staged on disk");

  // 4. the curated collection -------------------------------------------------
  const http::Result collections = GetAuthed(client, base + "/api/collections", token);
  checks.ExpectOk(collections, "GET /api/collections");
  checks.Expect(collections.response.body.find("\"Handheld\"") != std::string::npos,
                "the curated `Handheld` collection exists");

  // Existing-and-empty is the failure mode a name check cannot see: the point
  // of the collection is what is in it.
  if (!collection_id.empty()) {
    const http::Result curated =
        GetAuthed(client, base + "/api/roms?collection_id=" + collection_id + "&limit=1", token);
    checks.ExpectOk(curated, "GET /api/roms?collection_id");
    const std::string curated_total = rig::JsonNumber(curated.response.body, "total");
    checks.Expect(!curated_total.empty() && curated_total != "0",
                  "the `Handheld` collection actually holds roms");
  }

  if (checks.failures() == 0) {
    std::cout << "fixture provisioned: " << total << " rom(s), Handheld collection, "
              << "working client token\n";
  }
  return checks.failures() == 0 ? 0 : 1;
}

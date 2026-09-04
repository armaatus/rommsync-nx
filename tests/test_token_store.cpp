// token.dat: written atomically, or not written at all.
//
// The pairing flow runs once, with a human at a browser. Everything after it
// depends on this file, and the failure that matters is not "the write failed"
// -- it is a write that fails *half way* and leaves a file that looks like a
// token, parses like a token, and is not one. So the assertions here are mostly
// about what survives a failure rather than about what a success produces.
//
// No network and no rig: this is the filesystem and a parser, so it never
// skips.
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;

namespace {

std::filesystem::path ScratchDir() {
  const std::filesystem::path dir = std::filesystem::path(ROMMSYNC_TEST_SCRATCH) / "token_store";
  std::filesystem::create_directories(dir);
  return dir;
}

auth::StoredToken Fixture() {
  auth::StoredToken token;
  token.server_url = "http://romm.lan:8080";
  // Synthetic, and shaped like the real thing: `rmm_` + 64 hex characters.
  token.access_token = "rmm_" + std::string(64, 'a');
  token.device_id = "9fdce844-779b-4216-a6e2-597a2f3e7027";
  token.scopes = {"me.read", "roms.read", "assets.write"};
  return token;
}

std::string ReadWhole(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void SameToken(checks::Checks& c, const auth::StoredToken& got, const auth::StoredToken& want,
               const std::string& what) {
  c.ExpectEq(got.server_url, want.server_url, what + ": server_url");
  c.ExpectEq(got.access_token, want.access_token, what + ": access_token");
  c.ExpectEq(got.device_id, want.device_id, what + ": device_id");
  c.ExpectEq(got.scopes.size(), want.scopes.size(), what + ": scope count");
  for (std::size_t at = 0; at < want.scopes.size() && at < got.scopes.size(); ++at) {
    c.ExpectEq(got.scopes[at], want.scopes[at], what + ": scope " + std::to_string(at));
  }
  c.ExpectEq(got.expires_at.has_value(), want.expires_at.has_value(), what + ": has expires_at");
  if (got.expires_at.has_value() && want.expires_at.has_value()) {
    c.ExpectEq(*got.expires_at, *want.expires_at, what + ": expires_at");
  }
}

/// A record has to survive being written and read by this same code, including
/// the values that would break a body built by concatenation.
void RoundTrips(checks::Checks& c) {
  const auth::StoredToken token = Fixture();
  const auth::Parsed<auth::StoredToken> back = auth::ParseStoredToken(auth::SerializeStoredToken(token));
  c.Expect(back.ok(), "a serialized token parses: " + back.error.Describe());
  SameToken(c, back.value, token, "round trip");

  // `null` is the answer 5.2.0 gives on every response, and it has to survive
  // as "no expiry" rather than as an empty string a timestamp parser would try.
  c.Expect(!back.value.expires_at.has_value(), "a null expires_at stays absent");

  auth::StoredToken dated = token;
  dated.expires_at = "2026-09-04T13:04:00.528870+00:00";
  const auth::Parsed<auth::StoredToken> dated_back =
      auth::ParseStoredToken(auth::SerializeStoredToken(dated));
  c.Expect(dated_back.ok(), "a dated token parses: " + dated_back.error.Describe());
  SameToken(c, dated_back.value, dated, "dated round trip");

  // The one way to get a hand-built JSON body wrong. A server URL is user
  // input, so it really can hold a quote or a backslash.
  auth::StoredToken awkward = token;
  awkward.server_url = "http://romm.lan/\"odd\\path\n";
  const auth::Parsed<auth::StoredToken> awkward_back =
      auth::ParseStoredToken(auth::SerializeStoredToken(awkward));
  c.Expect(awkward_back.ok(), "an awkward server_url parses: " + awkward_back.error.Describe());
  SameToken(c, awkward_back.value, awkward, "awkward round trip");

  const std::string text = auth::SerializeStoredToken(token);
  c.Expect(text.find('\n') == std::string::npos, "the record is one line");
}

/// Written, then read back off the disk. The `.tmp` file is an implementation
/// detail that must not outlive the write.
void WritesAndReads(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "roundtrip.dat";
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".tmp");

  const auth::StoredToken token = Fixture();
  const auth::StoreResult saved = auth::SaveToken(path.string(), token);
  c.Expect(saved.ok(), "SaveToken succeeds: " + saved.message);
  c.Expect(!std::filesystem::exists(path.string() + ".tmp"), "no temp file is left behind");

  const auth::LoadedToken loaded = auth::LoadToken(path.string());
  c.Expect(loaded.ok(), "LoadToken succeeds: " + loaded.message);
  SameToken(c, loaded.value, token, "on disk");

  // Overwriting is the re-pair path, and it has to replace rather than append.
  auth::StoredToken second = token;
  second.access_token = "rmm_" + std::string(64, 'b');
  second.scopes = {"me.read"};
  c.Expect(auth::SaveToken(path.string(), second).ok(), "overwriting succeeds");
  const auth::LoadedToken again = auth::LoadToken(path.string());
  c.Expect(again.ok(), "the overwritten token loads: " + again.message);
  SameToken(c, again.value, second, "after overwrite");
  c.Expect(!std::filesystem::exists(path.string() + ".tmp"), "still no temp file");
}

/// The guarantee the whole file exists for: a write that cannot complete costs
/// the *new* token, never the one already on disk.
void AFailedWriteLeavesTheOldToken(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "survivor.dat";
  const std::filesystem::path temp = path.string() + ".tmp";
  std::filesystem::remove(path);
  std::filesystem::remove_all(temp);

  const auth::StoredToken original = Fixture();
  c.Expect(auth::SaveToken(path.string(), original).ok(), "the first token is written");
  const std::string before = ReadWhole(path);

  // A directory where the temp file wants to be: fopen cannot open it, so the
  // write fails at the first step, which is exactly the step everything else
  // is arranged to fail at.
  std::filesystem::create_directories(temp);
  auth::StoredToken replacement = original;
  replacement.access_token = "rmm_" + std::string(64, 'c');
  const auth::StoreResult failed = auth::SaveToken(path.string(), replacement);
  c.Expect(!failed.ok(), "a write that cannot stage its temp file fails");
  c.ExpectEq(std::string(auth::ToString(failed.error)), std::string("open_failed"),
             "and says why");
  c.Expect(failed.message.find(replacement.access_token) == std::string::npos,
           "the failure message does not quote the token");

  c.ExpectEq(ReadWhole(path), before, "the destination still holds the previous token");
  const auth::LoadedToken loaded = auth::LoadToken(path.string());
  c.Expect(loaded.ok(), "and it still loads");
  SameToken(c, loaded.value, original, "the survivor");
  std::filesystem::remove_all(temp);
}

/// A missing directory is the case a console hits on first run, and it has to
/// be a named error rather than a silent no-op that leaves the sysmodule
/// believing it persisted a token.
void NamesAMissingDirectory(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "no-such-directory" / "token.dat";
  const auth::StoreResult saved = auth::SaveToken(path.string(), Fixture());
  c.Expect(!saved.ok(), "writing into a missing directory fails");
  c.ExpectEq(std::string(auth::ToString(saved.error)), std::string("open_failed"), "as open_failed");
  c.Expect(saved.message.find(path.string()) != std::string::npos, "and names the path");
}

/// Refused on the way *out*, not just on the way in. A file that exists and
/// holds an unusable token is worse than no file: the sysmodule would believe
/// it is paired and 401 on every tick.
void RefusesAnUnusableToken(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "unusable.dat";
  std::filesystem::remove(path);

  struct Case {
    const char* what;
    auth::StoredToken token;
  };
  auth::StoredToken blank = Fixture();
  blank.access_token.clear();
  auth::StoredToken nul = Fixture();
  // Legal JSON, and every C API downstream stops at the NUL -- so the token
  // that would reach an Authorization header is not the one that was checked.
  nul.device_id = std::string("d\0evil", 6);
  auth::StoredToken no_server = Fixture();
  no_server.server_url.clear();

  const Case kCases[] = {
      {"a blank access_token", blank},
      {"a device_id with an embedded NUL", nul},
      {"no server_url", no_server},
  };
  for (const Case& one : kCases) {
    const auth::StoreResult saved = auth::SaveToken(path.string(), one.token);
    c.Expect(!saved.ok(), std::string("refuses ") + one.what);
    c.ExpectEq(std::string(auth::ToString(saved.error)), std::string("unusable_token"),
               std::string("as unusable_token: ") + one.what);
    c.Expect(!std::filesystem::exists(path), std::string("and writes nothing: ") + one.what);
  }
}

/// The reader half of "never a partial write". Whatever leaves half a record on
/// disk -- an older build without the rename, a corrupted card -- must not read
/// back as a token.
void RefusesWhatIsNotAToken(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "damaged.dat";

  const auth::LoadedToken missing = auth::LoadToken((ScratchDir() / "absent.dat").string());
  c.Expect(!missing.ok(), "a missing file is an error, not an empty token");
  c.ExpectEq(std::string(auth::ToString(missing.error)), std::string("read_failed"), "read_failed");

  const std::string whole = auth::SerializeStoredToken(Fixture());
  struct Case {
    const char* what;
    std::string text;
  };
  const Case kCases[] = {
      {"an empty file", ""},
      {"half a record", whole.substr(0, whole.size() / 2)},
      {"a record missing device_id",
       R"({"server_url":"http://x","access_token":"rmm_a","scopes":[],"expires_at":null})"},
      {"a record with a blank token",
       R"({"server_url":"http://x","access_token":"","device_id":"d","scopes":[],)"
       R"("expires_at":null})"},
      {"two records", whole + whole},
  };
  for (const Case& one : kCases) {
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(one.text.data(), static_cast<std::streamsize>(one.text.size()));
    }
    const auth::LoadedToken loaded = auth::LoadToken(path.string());
    c.Expect(!loaded.ok(), std::string("refuses ") + one.what);
    c.ExpectEq(std::string(auth::ToString(loaded.error)), std::string("malformed"),
               std::string("as malformed: ") + one.what);
  }
}

/// The record pairing actually persists comes off a `DeviceTokenResponse`, and
/// carries the server it was issued by: a token is only meaningful against that
/// one, so re-pointing the sysmodule elsewhere must not send a stranger a
/// bearer token.
void CarriesTheServerItWasIssuedBy(checks::Checks& c) {
  auth::DeviceTokenResponse granted;
  granted.access_token = "rmm_" + std::string(64, 'd');
  granted.device_id = "9fdce844-779b-4216-a6e2-597a2f3e7027";
  granted.scopes = {"me.read", "roms.read"};

  const auth::StoredToken token = auth::StoredTokenFrom("http://romm.lan:8080/", granted);
  c.ExpectEq(token.server_url, std::string("http://romm.lan:8080/"), "keeps the server url");
  c.ExpectEq(token.access_token, granted.access_token, "keeps the token");
  c.ExpectEq(token.device_id, granted.device_id, "keeps the device id");
  c.ExpectEq(token.scopes.size(), std::size_t{2}, "keeps the approved scopes");
  c.Expect(!token.expires_at.has_value(), "a null expiry stays null");
}

}  // namespace

int main() {
  checks::Checks c;
  RoundTrips(c);
  WritesAndReads(c);
  AFailedWriteLeavesTheOldToken(c);
  NamesAMissingDirectory(c);
  RefusesAnUnusableToken(c);
  RefusesWhatIsNotAToken(c);
  CarriesTheServerItWasIssuedBy(c);
  return c.failures() == 0 ? 0 : 1;
}

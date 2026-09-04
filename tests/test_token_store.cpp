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
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/atomic_file.hpp"
#include "rommsync/pairing.hpp"
#include "rommsync/token_store.hpp"

namespace auth = rommsync::auth;
namespace io = rommsync::io;

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

void WriteWhole(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
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
  c.Expect(!std::filesystem::exists(path.string() + ".old"),
           "and the record it replaced is not left lying around");
}

/// The commit is two renames, not one, because Horizon's is not a replace.
///
/// `fsFsRenameFile` refuses a destination that already exists -- which is every
/// re-pair -- so `SaveToken` moves the record already in place aside first. That
/// happens on both platforms deliberately: a fallback taken only on the console
/// is a path no test here would ever run. What it costs is one moment where
/// `token.dat` does not exist and `token.dat.old` holds the previous record, so
/// `LoadToken` reads that one rather than reporting nothing was ever paired.
void RecoversFromTheCommitWindow(checks::Checks& c) {
  const std::filesystem::path path = ScratchDir() / "window.dat";
  const std::filesystem::path previous = path.string() + ".old";
  std::filesystem::remove(path);
  std::filesystem::remove(previous);

  const auth::StoredToken token = Fixture();
  c.Expect(auth::SaveToken(path.string(), token).ok(), "the token is written");

  // Exactly the state a power cut between the two renames leaves behind.
  std::filesystem::rename(path, previous);
  c.Expect(!std::filesystem::exists(path), "the destination is gone, as it would be mid-commit");

  const auth::LoadedToken loaded = auth::LoadToken(path.string());
  c.Expect(loaded.ok(), "the previous record is read rather than lost: " + loaded.message);
  SameToken(c, loaded.value, token, "recovered");

  // With nothing to fall back to, a missing file still reports itself as one
  // rather than as whatever the fallback failed with.
  std::filesystem::remove(previous);
  const auth::LoadedToken nothing = auth::LoadToken(path.string());
  c.Expect(!nothing.ok(), "and with no fallback there is still nothing");
  c.ExpectEq(std::string(auth::ToString(nothing.error)), std::string("read_failed"),
             "reported as read_failed");
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
  // `null` means "no expiry" and is fine; a *present* empty string is refused
  // on the way back in, so writing one would produce a file that exists, looks
  // paired, and cannot be read.
  auth::StoredToken blank_expiry = Fixture();
  blank_expiry.expires_at = "";

  const Case kCases[] = {
      {"a blank access_token", blank},
      {"a device_id with an embedded NUL", nul},
      {"no server_url", no_server},
      {"a present but empty expires_at", blank_expiry},
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


/// The acceptance criterion, taken literally: kill the process mid-write.
///
/// A directory in the way of the temp file (above) proves the *first* step can
/// fail safely. It does not prove anything about a write that has already put
/// bytes on the card and then stops -- which is the case a power cut produces,
/// and the only one where a truncated record could plausibly reach `token.dat`.
///
/// The kill is `RLIMIT_FSIZE` rather than a timer. A child that may write only
/// a few hundred bytes, asked to write a record far larger than that, is killed
/// by `SIGXFSZ` *inside* the write, at a byte offset the kernel picks -- no
/// sleeping, no racing, and no cleanup on the way out: no destructor runs, no
/// `remove` of the temp file, exactly what a console losing power does. A timing
/// based kill would pass on a fast machine by never landing in the window.
void SurvivesTheProcessBeingKilledMidWrite(checks::Checks& c) {
#ifdef ROMMSYNC_CAN_FORK
  const std::filesystem::path path = ScratchDir() / "killed.dat";
  std::filesystem::remove(path);
  std::filesystem::remove(io::TempPathFor(path.string()));
  std::filesystem::remove(io::PreviousPathFor(path.string()));

  const auth::StoredToken original = Fixture();
  c.Expect(auth::SaveToken(path.string(), original).ok(), "the working token is written");
  const std::string before = ReadWhole(path);

  // Big enough that the write cannot fit under the limit, and cannot be
  // buffered whole either: the bytes have to reach the card before the record
  // is complete, which is the state being forced.
  auth::StoredToken huge = original;
  huge.access_token = "rmm_" + std::string(64, 'e');
  huge.scopes.assign(20000, "roms.read");

  std::fflush(nullptr);  // nothing of ours may be flushed twice by the child
  const pid_t child = fork();
  c.Expect(child >= 0, "the test can fork");
  if (child == 0) {
    const rlimit limit{4096, 4096};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
      _exit(2);
    }
    auth::SaveToken(path.string(), huge);
    _exit(0);  // reached only if the kill did not happen; the parent checks
  }

  int status = 0;
  c.Expect(waitpid(child, &status, 0) == child, "the child is reaped");
  c.Expect(WIFSIGNALED(status) != 0,
           "the child was killed by a signal rather than returning -- otherwise this test is "
           "checking a completed write");

  // The guarantee. Whatever is on the card, it is the token that was working
  // before the child died, complete and readable.
  c.Expect(std::filesystem::exists(path), "the destination still exists");
  c.ExpectEq(ReadWhole(path), before, "and still holds the previous token, byte for byte");
  const auth::LoadedToken loaded = auth::LoadToken(path.string());
  c.Expect(loaded.ok(), "which still loads: " + loaded.message);
  SameToken(c, loaded.value, original, "the survivor of a killed write");
  c.Expect(loaded.value.access_token != huge.access_token,
           "and it is not the token the killed write was carrying");

  // The debris a killed write leaves is a truncated temp file. It must not stop
  // the next write from succeeding, and it must not be read as a token.
  const auth::LoadedToken debris = auth::LoadToken(io::TempPathFor(path.string()));
  c.Expect(!debris.ok(), "the truncated temp file is not a token record");

  auth::StoredToken next = original;
  next.access_token = "rmm_" + std::string(64, 'f');
  const auth::StoreResult recovered = auth::SaveToken(path.string(), next);
  c.Expect(recovered.ok(), "and the next write goes through anyway: " + recovered.message);
  const auth::LoadedToken after = auth::LoadToken(path.string());
  c.Expect(after.ok(), "leaving the new token in place: " + after.message);
  SameToken(c, after.value, next, "after recovery");
  c.Expect(!std::filesystem::exists(io::TempPathFor(path.string())),
           "and the debris is gone");
#else
  c.Expect(false, "this test needs fork(); it is not built on a platform without one");
#endif
}

/// "Re-pair" has to genuinely discard the old credentials.
///
/// The trap is `token.dat.old`. It is what the commit leaves behind for a
/// moment, and a re-pair that unlinked only `token.dat` would leave the same
/// bearer token sitting next to it under a name nobody looks at.
void DiscardingLeavesNothingBehind(checks::Checks& c) {
  const std::filesystem::path directory = ScratchDir() / "discard";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "token.dat";

  const auth::StoredToken token = Fixture();
  c.Expect(auth::SaveToken(path.string(), token).ok(), "a token is written");
  // Every piece of debris a commit can leave, all holding the same secret.
  WriteWhole(io::TempPathFor(path.string()), auth::SerializeStoredToken(token));
  WriteWhole(io::PreviousPathFor(path.string()), auth::SerializeStoredToken(token));

  c.Expect(auth::DiscardToken(path.string()), "discarding reports that nothing is left");
  c.Expect(!std::filesystem::exists(path), "the record is gone");
  c.Expect(!std::filesystem::exists(io::TempPathFor(path.string())), "the temp file is gone");
  c.Expect(!std::filesystem::exists(io::PreviousPathFor(path.string())),
           "and so is the one the commit parks the previous record in");

  // The check that would have caught unlinking only the obvious file: nothing
  // anywhere in the directory still carries the token.
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory)) {
    c.Expect(ReadWhole(entry.path()).find(token.access_token) == std::string::npos,
             "no file beside it carries the token: " + entry.path().filename().string());
  }

  const auth::LoadedToken loaded = auth::LoadToken(path.string());
  c.Expect(!loaded.ok(), "and the client reads itself as unpaired");
  c.Expect(auth::DiscardToken(path.string()), "discarding nothing succeeds too");
}

/// The acceptance criterion, asserted rather than eyeballed: no secret reaches
/// a log or an error message.
///
/// Every string this module can hand to a caller for printing is produced with
/// a fixture whose secrets are distinctive needles, and searched for them. That
/// shape is the point -- a test that checked one known-bad message would go
/// green the day someone adds a message that quotes the body.
void NoSecretReachesALogLine(checks::Checks& c) {
  const std::string kToken = "rmm_needle0000000000000000000000000000000000000000000000000000tok";
  const std::string kDeviceCode = "needle1111111111111111111111111111111111111111111111111111111code";

  auth::StoredToken token = Fixture();
  token.access_token = kToken;

  std::vector<std::string> printed;
  printed.push_back(auth::DescribeStoredToken(token));

  // Every failure path that produces a message, with the needle in the record.
  const std::filesystem::path directory = ScratchDir() / "quiet";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const std::filesystem::path path = directory / "token.dat";

  auth::StoredToken unusable = token;
  unusable.device_id.clear();
  printed.push_back(auth::SaveToken(path.string(), unusable).message);
  printed.push_back(auth::SaveToken((directory / "gone" / "token.dat").string(), token).message);
  std::filesystem::create_directories(io::TempPathFor(path.string()));
  printed.push_back(auth::SaveToken(path.string(), token).message);
  std::filesystem::remove_all(io::TempPathFor(path.string()));
  printed.push_back(auth::LoadToken(path.string()).message);

  // A file that holds the token and is not a record: the parser has to name
  // what is wrong with it without quoting any of it.
  WriteWhole(path, auth::SerializeStoredToken(token).substr(0, 40));
  printed.push_back(auth::LoadToken(path.string()).message);
  WriteWhole(path, R"({"server_url":"http://x","access_token":")" + kToken +
                       R"(","device_id":"d","scopes":[],"expires_at":7})");
  printed.push_back(auth::LoadToken(path.string()).message);

  // And the two response bodies that carry a secret, rejected. A device init
  // body holds the `device_code`; a token body holds the token.
  printed.push_back(
      auth::ParseDeviceTokenResponse(R"({"access_token":")" + kToken + R"(","device_id":7})")
          .error.Describe());
  printed.push_back(auth::ParseDeviceInitResponse(R"({"device_code":")" + kDeviceCode +
                                                  R"(","user_code":"ABCD1234"})")
                        .error.Describe());
  printed.push_back(auth::ParseDeviceInitResponse(R"({"device_code":")" + kDeviceCode + R"("})" +
                                                  "trailing rubbish")
                        .error.Describe());

  // The pairing payload the overlay decodes and anything may render.
  auth::PairingStatus status;
  status.state = auth::PairingState::kFailed;
  status.user_code = "ABCD1234";
  status.message = "device token response: field device_id: expected a string, got a number";
  printed.push_back(auth::SerializePairingStatus(status));

  for (const std::string& line : printed) {
    c.Expect(!line.empty(), "every message says something");
    c.Expect(line.find(kToken) == std::string::npos, "no access token in: " + line);
    c.Expect(line.find(kDeviceCode) == std::string::npos, "no device code in: " + line);
  }

  // ...and the summary still says the things a support thread actually needs.
  const std::string described = auth::DescribeStoredToken(token);
  c.Expect(described.find(token.server_url) != std::string::npos, "the summary names the server");
  c.Expect(described.find(token.device_id) != std::string::npos, "and the device");
  c.Expect(described.find("me.read") != std::string::npos, "and the scopes");
  c.Expect(described.find(std::to_string(kToken.size())) != std::string::npos,
           "and that there is a token, by length only");
}

}  // namespace

int main() {
  checks::Checks c;
  RoundTrips(c);
  WritesAndReads(c);
  AFailedWriteLeavesTheOldToken(c);
  NamesAMissingDirectory(c);
  RecoversFromTheCommitWindow(c);
  RefusesAnUnusableToken(c);
  RefusesWhatIsNotAToken(c);
  CarriesTheServerItWasIssuedBy(c);
  SurvivesTheProcessBeingKilledMidWrite(c);
  DiscardingLeavesNothingBehind(c);
  NoSecretReachesALogLine(c);
  return c.failures() == 0 ? 0 : 1;
}

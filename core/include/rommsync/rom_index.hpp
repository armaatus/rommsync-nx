// The rom index: `GET /api/roms`, paged, cached for a tick, and the name
// matching that hangs off it.
//
// Step 0 of docs/SYNC_PROTOCOL.md is "turn a file on the card into a rom id",
// and this is the half that knows what roms exist. It is fetched once per tick
// and reused -- by the save scanner here, and by M3's downloads, which need the
// same `id` and the same `has_multiple_files`.
//
// Two things about the endpoint are not what a guess produces:
//
//   - It answers an **envelope**, `{items, total, limit, offset}`, not a bare
//     array. A client that reads the body as an array finds nothing; one that
//     reads `items` and stops has whatever page size RomM felt like giving it.
//   - So it has to be **paged**. `total` is the library, `items` is the page,
//     and a rom on the last page is exactly the one a save is going to need.
//
// Matching is deliberately narrow. `fs_name_no_ext` exact, `fs_name_no_tags` as
// a fallback, scoped to a platform when the path implies one -- and **ambiguity
// is a skip, never a guess**. A wrong match does not fail; it syncs one game's
// save onto another game, which is the same class of loss as an overwrite
// without a backup.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/auth_gate.hpp"
#include "rommsync/http.hpp"
#include "rommsync/json.hpp"

namespace rommsync::roms {

/// One rom, reduced to the fields step 0 and M3's downloads actually use.
///
/// The list schema carries around eighty; keeping five is not tidiness but
/// arithmetic -- a library of a few thousand roms is held in a sysmodule's heap
/// for the length of a tick.
struct Rom {
  std::int64_t id = 0;

  /// `240pee.nes` -> `240pee`. The primary match key.
  std::string fs_name_no_ext;

  /// The same with `(USA)`, `[!]` and friends removed. The fallback key: an
  /// emulator names a save after the rom file it loaded, so this only matters
  /// when the file on the card came from elsewhere.
  std::string fs_name_no_tags;

  /// RomM's on-disk folder name for the platform (`gba`), which is what
  /// `config::Config::platforms` is keyed by. Case-sensitive on both sides.
  std::string platform_fs_slug;

  /// RomM's own multi-file signal, on the *list* schema as well as the detail
  /// one -- so M3 can skip one without a second call per rom.
  ///
  /// The schema carries `has_simple_single_file` and `has_nested_single_file`
  /// beside it and this index reads neither, on purpose. A rom is exactly one
  /// of the three, and only the first is out of scope: a *nested* single-file
  /// rom is a directory on the server holding one file, and it downloads like
  /// any other rom -- same whole-rom endpoint, a real `Content-Length`, and a
  /// `sha1_hash` that is the digest of the bytes that arrive. Reading "is a
  /// directory" rather than this field is the mistake that would refuse it, and
  /// `download.nested` is there to fail if anything starts to.
  bool has_multiple_files = false;
};

/// One page of `GET /api/roms?limit=&offset=`.
struct Page {
  std::vector<Rom> roms;

  /// The whole library, as the server counted it when it answered.
  std::int64_t total = 0;

  /// Echoed back, and not necessarily what was asked for: RomM caps `limit`.
  /// The paging loop advances by what it actually received rather than by this.
  std::int64_t limit = 0;
  std::int64_t offset = 0;
};

/// Read one page body, or say which field was wrong.
///
/// Strict, like every other shape in the engine: a body that is not the
/// envelope, or a rom missing `platform_fs_slug`, is a named error rather than
/// an index that silently holds fewer roms than the library does -- which would
/// present as saves that stop matching, weeks later, on one console.
json::Error ParsePage(std::string_view body, Page* out);

/// Why a base name did not become a rom id.
enum class MatchOutcome {
  kMatched,
  kUnmatched,   ///< no rom carries this name -- a file that is not a save of anything
  kAmbiguous,   ///< several do, and nothing in the path says which. Never guessed.
};

/// The result of one lookup. `rom` is set only when `outcome` is `kMatched`.
struct Match {
  MatchOutcome outcome = MatchOutcome::kUnmatched;
  const Rom* rom = nullptr;

  /// A sentence for the log, naming the base name and -- for an ambiguity --
  /// the platforms it landed on, since that is what a user has to look at to
  /// resolve it by mapping a folder.
  std::string reason;

  bool matched() const { return outcome == MatchOutcome::kMatched; }
};

/// The library, as one tick sees it.
class RomIndex {
 public:
  void Add(Rom rom) {
    roms_.push_back(std::move(rom));
    by_no_ext_.clear();  // the lookup below is rebuilt on the next Find
    by_no_tags_.clear();
  }

  const std::vector<Rom>& roms() const { return roms_; }
  std::size_t size() const { return roms_.size(); }
  bool empty() const { return roms_.empty(); }

  /// The fetch stopped at `FetchOptions::max_roms` before the library ran out.
  /// Matching still works; it just cannot see the roms that were not read, so a
  /// caller logs this rather than treating the misses as unmatched files.
  bool truncated() const { return truncated_; }
  void set_truncated(bool truncated) { truncated_ = truncated; }

  const Rom* ById(std::int64_t id) const;

  /// Match one base name -- a save's file name minus its extension.
  ///
  /// `platform_fs_slug` is the hint the *path* gave, empty when it gave none.
  /// A hint scopes the search and does not merely rank it: a save under
  /// `/tico/saves/gba` is a GBA save, so a same-named NES rom is not a worse
  /// candidate, it is the wrong one. A hintless directory -- RetroArch's one
  /// flat `saves/` -- searches the whole library, and two hits there is the
  /// ambiguity that gets skipped.
  Match Find(std::string_view base_name, std::string_view platform_fs_slug) const;

 private:
  /// The two match keys, as offsets into `roms_` sorted by that key.
  ///
  /// Built once, on the first `Find` after the last `Add`, because the
  /// alternative is a linear scan of the whole library per file per pass: at
  /// this module's own bounds that is over a hundred million string
  /// comparisons in one tick, on a console. Offsets rather than a map of
  /// strings to pointers for the reason `kMaxIndexRoms` exists at all -- four
  /// bytes a rom against the sysmodule's heap, not a node and a copied key.
  void BuildLookup() const;

  std::vector<Rom> roms_;
  mutable std::vector<std::uint32_t> by_no_ext_;
  mutable std::vector<std::uint32_t> by_no_tags_;
  bool truncated_ = false;
};

/// How many roms one tick will hold.
///
/// A bound for the same reason `config::kMaxConfigBytes` is one: this runs on a
/// sysmodule heap, and "however many the server says" is a number a client does
/// not get to choose. Five figures of roms is an order of magnitude past a
/// curated Switch library, and the index says when it stopped rather than
/// pretending the rest are not there.
inline constexpr std::size_t kMaxIndexRoms = 20000;

/// The page size asked for. RomM may answer with fewer.
inline constexpr int kDefaultPageSize = 200;

struct FetchOptions {
  /// The origin, normalised -- `config::ServerConfig::url`.
  std::string base_url;

  /// The client token. Sent as `Authorization: Bearer`.
  std::string bearer_token;

  int page_size = kDefaultPageSize;
  std::size_t max_roms = kMaxIndexRoms;
};

/// A fetched index, or the reason there isn't one.
///
/// The three failures are kept apart because they are three different things to
/// do about it: a transport error is "retry next tick", a 401 is "the token is
/// gone, re-pair", and a shape error is "this server is not the RomM this build
/// was written against" -- which is the one that must never be papered over
/// with an empty index, because an empty index makes every save unmatched.
struct FetchResult {
  RomIndex index;

  http::Error transport = http::Error::kNone;

  /// The status of the request that failed. Zero when none did.
  int status = 0;

  /// The shape complaint, when the body was not the envelope.
  json::Error shape;

  /// One sentence for the log. Never carries the token.
  std::string message;

  bool ok() const { return transport == http::Error::kNone && status == 0 && shape.ok(); }
};

/// Fetch the whole library, one page at a time.
///
/// Pages until the server runs out, `total` is reached, or `max_roms` is --
/// whichever comes first. A short page is not an ending: the loop advances by
/// the number of roms it actually received, so a server that caps `limit` at
/// something smaller than was asked for is paged through rather than truncated
/// at its first answer.
FetchResult FetchRomIndex(http::HttpClient& client, const FetchOptions& options);

/// What a library fetch said about this console's credentials, for
/// `auth::Gate::Observe`.
///
/// The `AnswerOf` overload beside this file's error shape, the way there is one
/// beside every other (auth_gate.hpp states the rule they all follow). Only a
/// library that actually parsed is `kAccepted`: a transport failure, a 5xx and a
/// 200 that was not a rom page are all `kSilent`, because anything in front of
/// RomM answers those and clearing a rejection count on one would let a proxy
/// keep a console asking forever.
auth::Answer AnswerOf(const FetchResult& fetched);

}  // namespace rommsync::roms

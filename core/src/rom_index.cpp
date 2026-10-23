#include "rommsync/rom_index.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rommsync::roms {
namespace {

json::Error Fail(std::string field, std::string message) {
  json::Error error;
  error.field = std::move(field);
  error.message = std::move(message);
  return error;
}

/// The platforms a set of candidates sits on, deduplicated, at most four.
///
/// Four because this ends up in a log line and in the overlay, and the number
/// that helps is "which folders do I have to map", not the whole library. A
/// name on five platforms is already a name the user has to look at.
std::string PlatformsOf(const std::vector<const Rom*>& candidates) {
  std::vector<std::string_view> slugs;
  for (const Rom* rom : candidates) {
    if (std::find(slugs.begin(), slugs.end(), std::string_view(rom->platform_fs_slug)) ==
        slugs.end()) {
      slugs.push_back(rom->platform_fs_slug);
    }
  }
  std::string out;
  for (std::size_t index = 0; index < slugs.size() && index < 4; ++index) {
    if (index != 0) {
      out += ", ";
    }
    out += std::string(slugs[index]);
  }
  if (slugs.size() > 4) {
    out += ", and " + std::to_string(slugs.size() - 4) + " more";
  }
  return out;
}

/// A name field that must be present and a string, and **may be empty**.
///
/// Everything else about it is held to `json::Reader`'s bar: an embedded NUL is
/// refused, because every C API downstream stops at one and the value that got
/// used would not be the value that was checked.
json::Error ReadName(const json::Value& item, const char* key, std::string* out) {
  const json::Value* value = item.Find(key);
  if (value == nullptr) {
    return Fail(key, "missing");
  }
  if (!value->is_string()) {
    return Fail(key, "expected a string");
  }
  if (value->string().find('\0') != std::string::npos) {
    return Fail(key, "contains a NUL");
  }
  *out = value->string();
  return {};
}

/// `items[index]`, onto the end of `roms`, or the field that was wrong with it.
json::Error ReadRom(const json::Value& item, std::size_t index, std::vector<Rom>* roms) {
  const std::string where = "items[" + std::to_string(index) + "].";
  json::Reader reader(item, "rom");
  Rom rom;
  reader.Required("id", &rom.id);
  reader.Required("platform_fs_slug", &rom.platform_fs_slug);
  reader.Required("has_multiple_files", &rom.has_multiple_files);
  // The two match keys are read leniently, because `json::Reader::Required`
  // refuses an empty string and these two can legitimately be one: RomM
  // derives `fs_name_no_tags` by stripping `(...)` and `[...]`, so a rom
  // whose whole filename is a tag -- `(USA).nes` -- reduces to nothing. Read
  // strictly, that single oddly named file fails the page, aborts the fetch,
  // and leaves an empty index in which *every* save on the card is
  // unmatched, on every tick. An empty key simply matches no base name, which
  // costs that one rom and nothing else.
  if (!reader.ok()) {
    json::Error error = reader.error();
    error.field = where + error.field;
    return error;
  }
  for (const auto& [key, out] : {std::pair{"fs_name_no_ext", &rom.fs_name_no_ext},
                                 std::pair{"fs_name_no_tags", &rom.fs_name_no_tags}}) {
    if (json::Error error = ReadName(item, key, out); !error.ok()) {
      error.field = where + error.field;
      return error;
    }
  }
  if (rom.id <= 0) {
    return Fail(where + "id", "must be a positive rom id");
  }
  roms->push_back(std::move(rom));
  return {};
}

}  // namespace

json::Error ParsePage(std::string_view body, Page* out) {
  // A row at a time, never the page (`json::ParseEach`): a parsed row weighs
  // ~11x its text, and a page held as one document is a multiple of the
  // sysmodule's whole heap (M9-19, #217).
  //
  // And of a row only what is read (#248): a rom RomM identified carries ~12 KiB
  // of metadata nothing here looks at, ~190 KiB as a document.
  static constexpr std::string_view kRead[] = {
      "id",    "platform_fs_slug", "has_multiple_files", "fs_name_no_ext", "fs_name_no_tags",
      "total", "limit",            "offset"};
  Page page;
  const json::ParseResult document = json::ParseEach(
      body, "items", kRead,
      [&page](const json::Value& item, std::size_t index) {
        return ReadRom(item, index, &page.roms);
      },
      "rom list");
  if (!document.ok()) {
    return document.error;
  }

  // The envelope. A body that is a bare array -- which is what the endpoint
  // *looks* like it should answer -- fails here by name rather than by
  // producing an empty page, because an empty page reads as "the library ended"
  // and every save after it is unmatched.
  json::Reader envelope(document.value, "rom list");
  envelope.Required("total", &page.total);
  envelope.Required("limit", &page.limit);
  envelope.Required("offset", &page.offset);
  if (!envelope.ok()) {
    return envelope.error();
  }

  const json::Value* items = document.value.Find("items");
  if (items == nullptr || !items->is_array()) {
    return Fail("items", "expected an array of roms");
  }

  *out = std::move(page);
  return {};
}

const Rom* RomIndex::ById(std::int64_t id) const {
  for (const Rom& rom : roms_) {
    if (rom.id == id) {
      return &rom;
    }
  }
  return nullptr;
}

void RomIndex::BuildLookup() const {
  by_no_ext_.resize(roms_.size());
  by_no_tags_.resize(roms_.size());
  for (std::size_t index = 0; index < roms_.size(); ++index) {
    by_no_ext_[index] = static_cast<std::uint32_t>(index);
    by_no_tags_[index] = static_cast<std::uint32_t>(index);
  }
  const auto by = [this](const std::string Rom::*key) {
    return [this, key](std::uint32_t left, std::uint32_t right) {
      return roms_[left].*key < roms_[right].*key;
    };
  };
  std::sort(by_no_ext_.begin(), by_no_ext_.end(), by(&Rom::fs_name_no_ext));
  std::sort(by_no_tags_.begin(), by_no_tags_.end(), by(&Rom::fs_name_no_tags));
}

Match RomIndex::Find(std::string_view base_name, std::string_view platform_fs_slug) const {
  Match match;
  if (base_name.empty()) {
    match.reason = "the file has no name to match on";
    return match;
  }

  const std::string scope =
      platform_fs_slug.empty() ? std::string() : " on platform " + std::string(platform_fs_slug);

  // `fs_name_no_ext` first, `fs_name_no_tags` only if it found nothing at all.
  // The order matters more than it looks: the tag-stripped name of one rom is
  // routinely the exact name of another -- `Game (USA)` and `Game (Europe)`
  // both reduce to `Game` -- so letting the fallback run alongside the exact
  // match would turn a clean hit into an ambiguity.
  if (!roms_.empty() && by_no_ext_.size() != roms_.size()) {
    BuildLookup();
  }

  for (int pass = 0; pass < 2; ++pass) {
    const std::vector<std::uint32_t>& order = pass == 0 ? by_no_ext_ : by_no_tags_;
    const std::string Rom::*key = pass == 0 ? &Rom::fs_name_no_ext : &Rom::fs_name_no_tags;
    const auto before = [this, key](std::uint32_t left, std::string_view right) {
      return std::string_view(roms_[left].*key) < right;
    };
    const auto after = [this, key](std::string_view left, std::uint32_t right) {
      return left < std::string_view(roms_[right].*key);
    };
    const auto first = std::lower_bound(order.begin(), order.end(), base_name, before);
    const auto last = std::upper_bound(first, order.end(), base_name, after);

    // Counted rather than collected. The overwhelmingly common answers are none
    // and one, and neither is worth a heap allocation per file per pass on a
    // sysmodule. The candidates are gathered only on the branch that has to
    // name them.
    const Rom* found = nullptr;
    std::size_t hits = 0;
    for (auto it = first; it != last; ++it) {
      const Rom& rom = roms_[*it];
      if (!platform_fs_slug.empty() && rom.platform_fs_slug != platform_fs_slug) {
        continue;
      }
      if (found == nullptr) {
        found = &rom;
      }
      ++hits;
    }
    if (hits == 0) {
      continue;
    }
    if (hits == 1) {
      match.outcome = MatchOutcome::kMatched;
      match.rom = found;
      match.reason = std::string("matched \"") + std::string(base_name) + "\" to rom " +
                     std::to_string(match.rom->id) + " (" + match.rom->platform_fs_slug + ")" +
                     (pass == 0 ? "" : " by its tag-stripped name");
      return match;
    }

    std::vector<const Rom*> candidates;
    candidates.reserve(hits);
    for (auto it = first; it != last; ++it) {
      const Rom& rom = roms_[*it];
      if (platform_fs_slug.empty() || rom.platform_fs_slug == platform_fs_slug) {
        candidates.push_back(&rom);
      }
    }
    match.outcome = MatchOutcome::kAmbiguous;
    match.reason = "\"" + std::string(base_name) + "\" matches " + std::to_string(hits) +
                   " roms" + scope + " (" + PlatformsOf(candidates) +
                   "); map the folder to one platform to resolve it";
    return match;
  }

  match.outcome = MatchOutcome::kUnmatched;
  match.reason = "no rom named \"" + std::string(base_name) + "\"" + scope;
  return match;
}

FetchResult FetchRomIndex(http::HttpClient& client, const FetchOptions& options) {
  FetchResult result;
  if (options.base_url.empty()) {
    result.shape = Fail("base_url", "there is no server configured to ask");
    result.message = "no server configured";
    return result;
  }

  // A server that answers a page of one would otherwise take a round trip per
  // rom, up to the whole of `kMaxIndexRoms`, every tick. The cap is on
  // *requests*, so a pathological page size ends the fetch with a truncated
  // index rather than with a tick that never finishes: this many pages that came
  // back, and apart from them this many refused as too large -- at most twice
  // it in all. Apart, because a refusal halving its way down to a wide rom is
  // not a page of the library, and counted together they cut it short.
  constexpr int kMaxPages = 256;
  // No larger than the default, because the default is a heap term rather than
  // a preference (`kDefaultPageSize`): a caller asking for more would be asking
  // for a page the heap was not sized for.
  //
  // And smaller again when a page comes back past the transport's ceiling
  // (`http::kMaxResponseBytes`, #248): a rom RomM identified is ~12 KiB of row,
  // so 64 of them are ~750 KiB. The page is asked for again at half the rows.
  //
  // **And larger again after**, because what was too wide may be one rom: kept
  // at the size that fit around it, a library of 1,024 behind a rom that halved
  // the page to one row is a request a rom, and the cap above ends it a quarter
  // of the way through, every tick. Past a rom left out the page is back at
  // `largest`; otherwise it doubles after `wait` pages that came back. `wait`
  // doubles each time a doubled page is refused, so a library that is wide
  // everywhere -- every rom on IGDB -- pays a probe now and then rather than one
  // a page. Nor is there a backoff between them: the server answered, and the
  // same request is the same size however long it waits.
  const int largest = std::clamp(options.page_size, 1, kDefaultPageSize);
  int page_size = largest;
  int wait = 1;
  int streak = 0;
  bool probing = false;

  std::int64_t offset = 0;
  std::size_t room = 0;
  bool reserved = false;
  // The roms left out, counted, and the first named by where it was: a count
  // and not a list, so a library of them costs nothing that grows with it.
  std::int64_t first_left_out = -1;
  std::int64_t left_out = 0;
  const auto finished = [&result, &first_left_out, &left_out]() -> FetchResult {
    if (left_out > 0) {
      const std::string where = "at offset " + std::to_string(first_left_out);
      result.message += std::string(result.message.empty() ? "" : "; ") +
                        (left_out == 1 ? "the rom " + where + " is"
                                       : std::to_string(left_out) + " roms, the first " + where +
                                             ", are") +
                        " larger than the " + std::to_string(http::kMaxResponseBytes / 1024) +
                        " KiB this client reads in one answer, and not matched";
    }
    return std::move(result);
  };
  // A page lost in transit is a *failure* retry, on the curve every retry loop
  // shares, and its budget is the page's: `backoff` is cleared by every answer.
  const sync::CallPolicy& policy = options.policy;
  const int attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
  retry::Backoff backoff = sync::BackoffFor(policy);
  const auto canceled = [&policy] {
    return policy.cancel != nullptr && policy.cancel->canceled();
  };
  int pages = 0;
  int refused = 0;
  while (pages < kMaxPages && refused < kMaxPages) {
    if (canceled()) {
      // Between requests, so a shutdown is not owed the rest of the library. A
      // cancel, not a failure: the caller reads it as a cancelled tick, never
      // as a server that is down (#248, #251).
      result.transport = http::Error::kCanceled;
      result.message = "the rom index fetch was stopped: the caller cancelled";
      return finished();
    }
    http::Request request;
    // Three things about this URL are not decoration.
    //
    // `order_by=id&order_dir=asc` is a **total** order. The endpoint's default
    // orders by name, and a name is not unique in a RomM library -- `Sonic` on
    // `gg` and on `md` is the normal case -- so a tie straddling a page
    // boundary returns one rom twice and another never. The one that vanished
    // is a save that reports as unmatched and never syncs again; the one that
    // doubled makes its own name ambiguous. An id is unique, so neither
    // happens.
    //
    // The three `with_*` flags default to **true**, and each page otherwise
    // carries the whole library's `rom_id_index` and a freshly aggregated
    // `filter_values` -- a hundred pages each hauling a twenty-thousand-element
    // array through a JSON parser on a sysmodule heap, all of it discarded
    // here. Turning them off is most of what this request costs.
    request.url = options.base_url + "/api/roms?limit=" + std::to_string(page_size) +
                  "&offset=" + std::to_string(offset) +
                  "&order_by=id&order_dir=asc"
                  "&with_char_index=false&with_filter_values=false&with_rom_id_index=false";
    if (!options.bearer_token.empty()) {
      request.headers.push_back({"Authorization", "Bearer " + options.bearer_token});
    }

    request.timeout = policy.timeout;
    request.cancel = policy.cancel;

    const http::Result sent = client.Send(request);
    const bool lost = sent.ok() ? http::TransientStatus(sent.response.status)
                                : sent.error != http::Error::kCanceled &&
                                      sent.error != http::Error::kInvalidRequest &&
                                      !http::AnsweredTooLarge(sent);
    if (!lost) {
      backoff.Succeed();
    } else if (backoff.failures() + 1 < attempts) {
      // The same page again, after the wait -- unless the caller gave up, which
      // the top of the loop says without spending the wait first.
      if (!canceled()) {
        sync::WaitToRetry(policy, &backoff);
      }
      continue;
    }
    if (http::AnsweredTooLarge(sent)) {
      ++refused;
      streak = 0;
      if (probing) {
        // Past a fetch's pages the next probe never comes, which is the end of
        // doubling and of `int` overflow both.
        wait = std::min(wait * 2, kMaxPages);
      }
      probing = false;
      if (page_size > 1) {
        page_size /= 2;
        continue;
      }
      // One rom whose row alone is past the ceiling: left out, and the index
      // says it is not whole -- the same as a library cut at its bound, which is
      // what it is to that rom's saves.
      result.index.set_truncated(true);
      if (left_out++ == 0) {
        first_left_out = offset;
      }
      ++offset;
      page_size = largest;
      wait = 1;
      continue;
    }
    ++pages;
    if (!sent.ok()) {
      result.transport = sent.error;
      result.message = std::string("the rom index could not be fetched: ") +
                       http::ToString(sent.error) + " (" + sent.message + ")";
      return finished();
    }
    if (!sent.successful()) {
      result.status = sent.response.status;
      result.message =
          "the rom index was refused with status " + std::to_string(sent.response.status);
      return finished();
    }

    Page page;
    if (const json::Error error = ParsePage(sent.response.body, &page); !error.ok()) {
      result.shape = error;
      result.message = "the rom index is not the shape this client reads: " + error.Describe();
      return finished();
    }

    if (page.roms.empty()) {
      // An empty page short of `total` is not the library ending -- it is a rom
      // deleted between two of these requests, leaving `offset` past the new
      // end. Saying so matters because the alternative reads as a complete
      // index, and every save for a rom in the missing tail then reports as
      // unmatched with nothing anywhere saying why.
      if (page.total > 0 && offset < page.total) {
        result.index.set_truncated(true);
        result.message = "the rom index ran out at " + std::to_string(offset) + " of " +
                         std::to_string(page.total) +
                         " roms; the library changed mid-fetch and the rest are not matched";
      }
      return finished();
    }
    if (probing) {
      wait = 1;
      probing = false;
    }
    if (page_size < largest && ++streak >= wait) {
      page_size = std::min(page_size * 2, largest);
      streak = 0;
      probing = true;
    }
    // Reserved once, from the first page's `total`, because the index is held
    // for the whole tick and is a heap term at its bound (sysmodule/source/
    // main.cpp): grown by doubling, a library of 1,025 roms would hold room for
    // 2,048 -- and both buffers while it moved. A server that does not say how
    // many there are gets the bound.
    if (!reserved) {
      reserved = true;
      room = page.total > 0
                 ? std::min(static_cast<std::size_t>(page.total), options.max_roms)
                 : options.max_roms;
      result.index.Reserve(room);
    }
    for (Rom& rom : page.roms) {
      if (result.index.size() >= options.max_roms) {
        result.index.set_truncated(true);
        result.message = "the library is larger than the " + std::to_string(options.max_roms) +
                         " roms this client indexes; the rest are not matched";
        return finished();
      }
      if (result.index.size() >= room) {
        // More roms than `total` said when the fetch began: some were added
        // while it ran. Growing past the reservation is the doubling it exists
        // to avoid, so the index stops at what the library was, says so the way
        // a library that shrank mid-fetch does, and the next tick reads the rest.
        result.index.set_truncated(true);
        result.message = "the library grew past " + std::to_string(room) +
                         " roms while it was being read; the roms added since are matched "
                         "from the next tick";
        return finished();
      }
      result.index.Add(std::move(rom));
    }

    offset += static_cast<std::int64_t>(page.roms.size());
    if (page.total > 0 && offset >= page.total) {
      return finished();
    }
  }

  result.index.set_truncated(true);
  result.message = "the rom index stopped after " + std::to_string(pages + refused) +
                   " requests; the rest of the library is not matched";
  return finished();
}

auth::Answer AnswerOf(const FetchResult& fetched) {
  if (fetched.transport != http::Error::kNone) {
    // The exchange never completed, so it says nothing about the token.
    return auth::Answer::kSilent;
  }
  if (fetched.status == 401) {
    return auth::Answer::kRejected;
  }
  if (fetched.status == 403) {
    return auth::Answer::kForbidden;
  }
  if (fetched.status != 0) {
    // Another 4xx, a 5xx, a 429 -- every one of which something in front of RomM
    // answers without ever reading the token.
    return auth::Answer::kSilent;
  }
  if (!fetched.shape.ok()) {
    // A 200 whose body is not a rom page. The token was very likely read, but
    // "very likely" is not the standard this enum holds `kAccepted` to, and a
    // gateway's own 200 page would otherwise clear a rejection count.
    return auth::Answer::kSilent;
  }
  return auth::Answer::kAccepted;
}

}  // namespace rommsync::roms

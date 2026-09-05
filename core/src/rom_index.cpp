#include "rommsync/rom_index.hpp"

#include <algorithm>
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

}  // namespace

json::Error ParsePage(std::string_view body, Page* out) {
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    return document.error;
  }

  // The envelope first. A body that is a bare array -- which is what the
  // endpoint *looks* like it should answer -- fails here by name rather than
  // by producing an empty page, because an empty page reads as "the library
  // ended" and every save after it is unmatched.
  json::Reader envelope(document.value, "rom list");
  Page page;
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

  page.roms.reserve(items->size());
  std::size_t index = 0;
  for (const json::Value& item : items->elements()) {
    const std::string where = "items[" + std::to_string(index++) + "].";
    json::Reader reader(item, "rom");
    Rom rom;
    reader.Required("id", &rom.id);
    reader.Required("fs_name_no_ext", &rom.fs_name_no_ext);
    reader.Required("fs_name_no_tags", &rom.fs_name_no_tags);
    reader.Required("platform_fs_slug", &rom.platform_fs_slug);
    reader.Required("has_multiple_files", &rom.has_multiple_files);
    if (!reader.ok()) {
      json::Error error = reader.error();
      error.field = where + error.field;
      return error;
    }
    if (rom.id <= 0) {
      return Fail(where + "id", "must be a positive rom id");
    }
    page.roms.push_back(std::move(rom));
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
  for (int pass = 0; pass < 2; ++pass) {
    // Counted rather than collected. This runs once per file in every save
    // folder on the card, and the overwhelmingly common answers are none and
    // one -- neither of which is worth a heap allocation per file per pass on a
    // sysmodule. The candidates are gathered only on the branch that has to
    // name them.
    const Rom* first = nullptr;
    std::size_t hits = 0;
    for (const Rom& rom : roms_) {
      if (!platform_fs_slug.empty() && rom.platform_fs_slug != platform_fs_slug) {
        continue;
      }
      const std::string& key = pass == 0 ? rom.fs_name_no_ext : rom.fs_name_no_tags;
      if (key != base_name) {
        continue;
      }
      if (first == nullptr) {
        first = &rom;
      }
      ++hits;
    }
    if (hits == 0) {
      continue;
    }
    if (hits == 1) {
      match.outcome = MatchOutcome::kMatched;
      match.rom = first;
      match.reason = std::string("matched \"") + std::string(base_name) + "\" to rom " +
                     std::to_string(match.rom->id) + " (" + match.rom->platform_fs_slug + ")" +
                     (pass == 0 ? "" : " by its tag-stripped name");
      return match;
    }

    std::vector<const Rom*> candidates;
    candidates.reserve(hits);
    for (const Rom& rom : roms_) {
      if (!platform_fs_slug.empty() && rom.platform_fs_slug != platform_fs_slug) {
        continue;
      }
      if ((pass == 0 ? rom.fs_name_no_ext : rom.fs_name_no_tags) == base_name) {
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

  // A server that answers a page of one would otherwise take twenty thousand
  // round trips to read a library this client already refuses to hold. The cap
  // is on *requests*, so a pathological page size ends the fetch with a
  // truncated index rather than with a tick that never finishes.
  constexpr int kMaxPages = 256;
  const int page_size = std::clamp(options.page_size, 1, 500);

  std::int64_t offset = 0;
  for (int page_number = 0; page_number < kMaxPages; ++page_number) {
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

    const http::Result sent = client.Send(request);
    if (!sent.ok()) {
      result.transport = sent.error;
      result.message = std::string("the rom index could not be fetched: ") +
                       http::ToString(sent.error) + " (" + sent.message + ")";
      return result;
    }
    if (!sent.successful()) {
      result.status = sent.response.status;
      result.message =
          "the rom index was refused with status " + std::to_string(sent.response.status);
      return result;
    }

    Page page;
    if (const json::Error error = ParsePage(sent.response.body, &page); !error.ok()) {
      result.shape = error;
      result.message = "the rom index is not the shape this client reads: " + error.Describe();
      return result;
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
      return result;
    }
    for (Rom& rom : page.roms) {
      if (result.index.size() >= options.max_roms) {
        result.index.set_truncated(true);
        result.message = "the library is larger than the " + std::to_string(options.max_roms) +
                         " roms this client indexes; the rest are not matched";
        return result;
      }
      result.index.Add(std::move(rom));
    }

    offset += static_cast<std::int64_t>(page.roms.size());
    if (page.total > 0 && offset >= page.total) {
      return result;
    }
  }

  result.index.set_truncated(true);
  result.message = "the rom index stopped after " + std::to_string(kMaxPages) +
                   " pages; the rest of the library is not matched";
  return result;
}

}  // namespace rommsync::roms

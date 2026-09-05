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
    std::vector<const Rom*> candidates;
    for (const Rom& rom : roms_) {
      if (!platform_fs_slug.empty() && rom.platform_fs_slug != platform_fs_slug) {
        continue;
      }
      const std::string& key = pass == 0 ? rom.fs_name_no_ext : rom.fs_name_no_tags;
      if (key == base_name) {
        candidates.push_back(&rom);
      }
    }
    if (candidates.empty()) {
      continue;
    }
    if (candidates.size() == 1) {
      match.outcome = MatchOutcome::kMatched;
      match.rom = candidates.front();
      match.reason = std::string("matched \"") + std::string(base_name) + "\" to rom " +
                     std::to_string(match.rom->id) + " (" + match.rom->platform_fs_slug + ")" +
                     (pass == 0 ? "" : " by its tag-stripped name");
      return match;
    }
    match.outcome = MatchOutcome::kAmbiguous;
    match.reason = "\"" + std::string(base_name) + "\" matches " +
                   std::to_string(candidates.size()) + " roms" + scope + " (" +
                   PlatformsOf(candidates) + "); map the folder to one platform to resolve it";
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
    request.url = options.base_url + "/api/roms?limit=" + std::to_string(page_size) +
                  "&offset=" + std::to_string(offset);
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
      return result;  // the library ran out
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

// Read `rom_scope.hpp` first -- the two rules that are not obvious are there.
#include "rommsync/rom_scope.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <utility>

namespace rommsync::sync {
namespace {

/// Sorted, unique, positive.
std::vector<std::int64_t> Normalised(std::vector<std::int64_t> ids) {
  ids.erase(std::remove_if(ids.begin(), ids.end(), [](std::int64_t id) { return id <= 0; }),
            ids.end());
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

/// Cut `ids` into chunks of at most `kMaxNegotiateRomIds`, appended to `out`.
void AppendChunks(const std::vector<std::int64_t>& ids, bool restore,
                  std::vector<ScopeChunk>* out) {
  for (std::size_t begin = 0; begin < ids.size(); begin += kMaxNegotiateRomIds) {
    const std::size_t end = std::min(ids.size(), begin + kMaxNegotiateRomIds);
    ScopeChunk chunk;
    chunk.rom_ids.assign(ids.begin() + static_cast<std::ptrdiff_t>(begin),
                         ids.begin() + static_cast<std::ptrdiff_t>(end));
    chunk.restore = restore;
    out->push_back(std::move(chunk));
  }
}

}  // namespace

std::vector<ScopeChunk> ChunkScope(const std::vector<ClientSaveState>& saves,
                                   const std::vector<std::int64_t>& on_card,
                                   const std::vector<std::int64_t>& restore) {
  const std::vector<std::int64_t> restoring = Normalised(restore);

  std::vector<std::int64_t> ordinary = on_card;
  for (const ClientSaveState& save : saves) {
    ordinary.push_back(save.rom_id);
  }
  ordinary = Normalised(std::move(ordinary));
  ordinary.erase(std::remove_if(ordinary.begin(), ordinary.end(),
                                [&restoring](std::int64_t id) {
                                  return std::binary_search(restoring.begin(), restoring.end(),
                                                            id);
                                }),
                 ordinary.end());

  std::vector<ScopeChunk> chunks;
  AppendChunks(ordinary, /*restore=*/false, &chunks);
  AppendChunks(restoring, /*restore=*/true, &chunks);
  if (chunks.empty()) {
    chunks.emplace_back();
  }

  for (std::size_t index = 0; index < saves.size(); ++index) {
    ScopeChunk* home = &chunks.front();
    for (ScopeChunk& chunk : chunks) {
      if (std::binary_search(chunk.rom_ids.begin(), chunk.rom_ids.end(), saves[index].rom_id)) {
        home = &chunk;
        break;
      }
    }
    home->saves.push_back(index);
  }
  return chunks;
}

RomsOnCard FindRomsOnCard(const config::Config& config, const roms::RomIndex& index,
                          fs::FileSystem& files) {
  // Folder -> the platforms that map it. A folder two platforms share is listed
  // once and matched under each.
  std::map<std::string, std::vector<std::string>, std::less<>> folders;
  for (const auto& [slug, mapped] : config.platforms) {
    for (const std::string& folder : mapped.roms) {
      folders[folder].push_back(slug);
    }
  }

  RomsOnCard found;
  for (const auto& [folder, slugs] : folders) {
    const fs::Listing listing = files.List(folder);
    if (listing.error == fs::ListError::kTooManyEntries) {
      found.truncated.push_back(folder);
    } else if (!listing.ok()) {
      // Most mapped folders do not exist: the map covers every system the
      // console can run, and a card holds a few.
      continue;
    }
    for (const fs::Entry& entry : listing.entries) {
      // A multi-file rom is a folder named as the rom; anything else is a file
      // named as the rom plus its extension.
      const std::string_view name = entry.name;
      const std::string_view base =
          entry.is_directory ? name : name.substr(0, name.size() - ExtensionOf(name).size());
      for (const std::string& slug : slugs) {
        const roms::Match match = index.Find(base, slug);
        if (match.matched()) {
          found.rom_ids.push_back(match.rom->id);
        }
      }
    }
  }
  std::sort(found.rom_ids.begin(), found.rom_ids.end());
  found.rom_ids.erase(std::unique(found.rom_ids.begin(), found.rom_ids.end()),
                      found.rom_ids.end());
  return found;
}

}  // namespace rommsync::sync

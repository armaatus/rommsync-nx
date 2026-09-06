#include "rommsync/list_service.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace rommsync::lists {
namespace {

namespace keys = ipc::list_keys;

json::Error Fail(std::string field, std::string message) {
  json::Error error;
  error.field = std::move(field);
  error.message = std::move(message);
  return error;
}

/// A string field a **row draws**, which may be absent, `null` or empty.
///
/// The one reader here that is not `json::Reader::Required`, and for its
/// documented reason inverted: `Required` refuses an empty string because every
/// string it was written for is an identifier. These are not. RomM types a
/// rom's `name` as `string | null` and leaves it null on an unidentified rom, so
/// a strict read would fail the whole page over one file the server has no
/// metadata for -- which is a library that stops listing rather than a row that
/// draws its file name instead. `rom_index.cpp` makes the same exception for the
/// same shape.
///
/// An embedded NUL is still refused: every C API downstream stops at one, so the
/// value that got used would not be the value that was checked.
json::Error ReadDrawnText(const json::Value& object, const char* key, std::string* out,
                          bool required = true) {
  const json::Value* value = object.Find(key);
  if (value == nullptr || value->is_null()) {
    if (required && value == nullptr) {
      return Fail(key, "missing");
    }
    out->clear();
    return {};
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

/// Prefix `where` onto a field name, so a complaint names the row it came from.
json::Error At(const std::string& where, json::Error error) {
  error.field = where + error.field;
  return error;
}

void PutText(ipc::ListItem* item, std::string_view key, std::string_view value) {
  item->fields.push_back({std::string(key), ipc::ListValue::Text(Shorten(value))});
}

void PutInteger(ipc::ListItem* item, std::string_view key, std::int64_t value) {
  item->fields.push_back({std::string(key), ipc::ListValue::Integer(value)});
}

void PutFlag(ipc::ListItem* item, std::string_view key, bool value) {
  item->fields.push_back({std::string(key), ipc::ListValue::Flag(value)});
}

/// Fill `page` with rows `[from, count)`, stopping on whichever bound comes first.
///
/// The one loop the three kinds share, because the only thing that differs
/// between them is what a row *is*: `make_row(index)` builds it. Returns the
/// index one past the last row that fitted, so the caller advances its offset by
/// what actually went out rather than by what it asked for.
///
/// `page->items.empty()` on return with rows left is the one thing that would be
/// a page that never advances -- see `PageOf` -- and it is the caller's to
/// report.
template <typename MakeRow>
std::int64_t FillPage(ipc::ListPage* page, std::int64_t from, std::int64_t count,
                      std::int32_t page_size, MakeRow make_row) {
  std::int64_t index = from;
  while (index < count && static_cast<std::int32_t>(page->items.size()) < page_size) {
    if (!ipc::AppendIfItFits(page, make_row(index))) {
      break;
    }
    ++index;
  }
  return index;
}

/// The size of the file at `path`, or zero when there is none.
///
/// `download.cpp` has the same three lines and the same reason for them:
/// `core/` may not include `<filesystem>` (core/AGENTS.md) and devkitA64's
/// newlib has no `stat` worth relying on, so a `fopen` and a seek is what both
/// halves of this codebase can do.
std::int64_t FileSizeBytes(const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return 0;
  }
  std::int64_t size = 0;
  if (std::fseek(file, 0, SEEK_END) == 0) {
    const long told = std::ftell(file);
    size = told > 0 ? static_cast<std::int64_t>(told) : 0;
  }
  std::fclose(file);
  return size;
}

}  // namespace

std::string Shorten(std::string_view text) {
  if (text.size() <= kMaxRowTextBytes) {
    return std::string(text);
  }
  // Back off to a UTF-8 boundary: cutting mid-sequence leaves a byte no decoder
  // on the console can draw, and the overlay is handed this to render rather
  // than to interpret.
  std::size_t cut = kMaxRowTextBytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return std::string(text.substr(0, cut)) + "...";
}

json::Error ParseRomPage(std::string_view body, RomPage* out) {
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    return document.error;
  }

  // The envelope first, and by name. A body that is a bare array -- which is
  // what the endpoint looks like it should answer, and what `/api/platforms`
  // really does answer -- fails here rather than producing an empty page, and
  // an empty page is what a truncated response must never be able to look like.
  RomPage page;
  json::Reader envelope(document.value, "rom list");
  envelope.Required("total", &page.total);
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
    RomRow rom;
    reader.Required("id", &rom.id);
    reader.Required("fs_name", &rom.fs_name);
    reader.Required("platform_fs_slug", &rom.platform_fs_slug);
    reader.Required("fs_size_bytes", &rom.size_bytes);
    reader.Required("has_multiple_files", &rom.has_multiple_files);
    if (!reader.ok()) {
      return At(where, reader.error());
    }
    // `name` alone is read leniently: see `ReadDrawnText`.
    if (json::Error error = ReadDrawnText(item, "name", &rom.name); !error.ok()) {
      return At(where, error);
    }
    if (rom.id <= 0) {
      return Fail(where + "id", "must be a positive rom id");
    }
    page.roms.push_back(std::move(rom));
  }

  *out = std::move(page);
  return {};
}

json::Error ParsePlatforms(std::string_view body, std::vector<PlatformRow>* out,
                           bool* truncated) {
  const json::ParseResult document = json::Parse(body);
  if (!document.ok()) {
    return document.error;
  }
  if (!document.value.is_array()) {
    return Fail("", "expected an array of platforms");
  }

  std::vector<PlatformRow> platforms;
  if (truncated != nullptr) {
    *truncated = document.value.size() > kMaxPlatforms;
  }
  std::size_t index = 0;
  for (const json::Value& item : document.value.elements()) {
    if (platforms.size() >= kMaxPlatforms) {
      break;
    }
    const std::string where = "[" + std::to_string(index++) + "].";
    json::Reader reader(item, "platform");
    PlatformRow platform;
    reader.Required("id", &platform.id);
    reader.Required("fs_slug", &platform.fs_slug);
    reader.Required("name", &platform.name);
    reader.Required("rom_count", &platform.rom_count);
    if (!reader.ok()) {
      return At(where, reader.error());
    }
    // What RomM's own UI draws: `custom_name` when the user renamed the
    // platform and `name` otherwise. Optional, because a platform is still a
    // platform without it -- unlike the four above, none of which the browser
    // can draw a row or descend into a list without.
    std::string display_name;
    if (ReadDrawnText(item, "display_name", &display_name, false).ok() &&
        !display_name.empty()) {
      platform.name = std::move(display_name);
    }
    if (platform.id <= 0) {
      return Fail(where + "id", "must be a positive platform id");
    }
    platforms.push_back(std::move(platform));
  }

  *out = std::move(platforms);
  return {};
}

// --- the service --------------------------------------------------------------

Service::Service(const config::Config& config, download::Queue& queue, Clock clock)
    : config_(config), queue_(queue), clock_(std::move(clock)) {}

Service::TimePoint Service::Now() const {
  return clock_ ? clock_() : std::chrono::steady_clock::now();
}

void Service::UseServer(http::HttpClient* client, std::string bearer_token) {
  const std::lock_guard<std::mutex> held(mutex_);
  client_ = client;
  bearer_token_ = std::move(bearer_token);
}

void Service::UseCard(fs::FileSystem* filesystem) {
  const std::lock_guard<std::mutex> held(mutex_);
  filesystem_ = filesystem;
}

std::size_t Service::open_cursors() const {
  const std::lock_guard<std::mutex> held(mutex_);
  return cursors_.size();
}

std::vector<Service::Entry>::iterator Service::Find(ipc::Cursor cursor) {
  return std::find_if(cursors_.begin(), cursors_.end(),
                      [cursor](const Entry& entry) { return entry.id == cursor; });
}

void Service::ReapLocked(TimePoint now) {
  // There is no timer here, so this runs on whatever call happens next. An
  // abandoned cursor therefore costs its own memory until then, which is the
  // trade the header states: a reclaimed cursor is a re-opened list on the
  // overlay's side, and a thread woken every second to notice one is not.
  cursors_.erase(std::remove_if(cursors_.begin(), cursors_.end(),
                                [&](const Entry& entry) {
                                  return now - entry.touched >= kCursorTtl;
                                }),
                 cursors_.end());
}

void Service::EvictOldestLocked() {
  if (cursors_.empty()) {
    return;
  }
  auto oldest = cursors_.begin();
  for (auto it = cursors_.begin(); it != cursors_.end(); ++it) {
    if (it->touched < oldest->touched) {
      oldest = it;
    }
  }
  cursors_.erase(oldest);
}

ipc::Error Service::ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) {
  const TimePoint now = Now();
  const std::lock_guard<std::mutex> held(mutex_);
  ReapLocked(now);
  if (cursors_.size() >= kMaxCursors) {
    // The cap plus one still answers. See the header: a refusal would leave a
    // screen with nowhere to go, while an evicted cursor is one `kBadCursor`
    // and a reload on a side that already handles it (#25).
    EvictOldestLocked();
  }

  Entry entry;
  entry.id = next_id_++;
  entry.request = request;
  entry.request.page_size = std::clamp(request.page_size, 1, ipc::kMaxPageSize);
  if (entry.request.kind != ipc::ListKind::kRoms) {
    // A filter that means nothing for the kind is dropped rather than carried,
    // exactly as `ipc::ServiceCore::ListBegin` drops it -- this class is also
    // called directly by the suite, so it holds itself to the same rule.
    entry.request.platform_id = 0;
    entry.request.search.clear();
  }
  entry.touched = now;
  *cursor = entry.id;
  cursors_.push_back(std::move(entry));
  return ipc::Error::kOk;
}

ipc::Error Service::ListEnd(ipc::Cursor cursor) {
  const TimePoint now = Now();
  const std::lock_guard<std::mutex> held(mutex_);
  ReapLocked(now);
  const auto found = Find(cursor);
  if (found == cursors_.end()) {
    return ipc::Error::kBadCursor;
  }
  cursors_.erase(found);
  return ipc::Error::kOk;
}

ipc::Error Service::FillFromQueueLocked(Entry& entry, ipc::ListPage* page) {
  const std::vector<download::QueueEntry> rows = queue_.Snapshot();
  const download::DownloadStatus status = queue_.Status();

  // The complaint about `queue.json` itself, as a row.
  //
  // #22 built `DownloadStatus::queue_message` as the home for it and kept a
  // `[downloads]` section on `config_diagnostics()` standing anyway, because
  // that was the only channel that reached a user until this list existed. This
  // is the channel. It has to be a row because the projection is pinned whole
  // (`ipc::list_keys`) and because a queue that was discarded has no rows of its
  // own to hang the sentence on -- the screen would otherwise say "Nothing in
  // the download queue" over a queue the card lost. `LibraryBrowserModel` draws
  // it as an inert, unpressable row with the reason underneath, which is exactly
  // what it is.
  const bool complaint = !status.queue_message.empty();
  const std::int64_t count = static_cast<std::int64_t>(rows.size()) + (complaint ? 1 : 0);

  const std::int64_t served =
      FillPage(page, entry.offset, count, entry.request.page_size, [&](std::int64_t index) {
        ipc::ListItem item;
        if (complaint && index == 0) {
          PutInteger(&item, keys::kQueueRomId, 0);
          // Named after the file rather than left blank: the model falls back to
          // "Rom 0" for a row with no `fs_name`, and this row is about the file.
          PutText(&item, keys::kQueueFsName, download::kQueueFileName);
          PutText(&item, keys::kQueuePlatformFsSlug, "");
          PutText(&item, keys::kQueueState, download::ToString(download::QueueState::kFailed));
          PutInteger(&item, keys::kQueueBytesDone, 0);
          PutInteger(&item, keys::kQueueSizeBytes, 0);
          PutInteger(&item, keys::kQueueBytesPerSecond, 0);
          PutInteger(&item, keys::kQueueAttempts, 0);
          PutText(&item, keys::kQueueMessage, status.queue_message);
          return item;
        }
        const download::QueueEntry& row =
            rows[static_cast<std::size_t>(index - (complaint ? 1 : 0))];
        PutInteger(&item, keys::kQueueRomId, row.rom_id);
        PutText(&item, keys::kQueueFsName, row.fs_name);
        PutText(&item, keys::kQueuePlatformFsSlug, row.platform_fs_slug);
        PutText(&item, keys::kQueueState, download::ToString(row.state));
        PutInteger(&item, keys::kQueueBytesDone, row.bytes_done);
        PutInteger(&item, keys::kQueueSizeBytes, row.size_bytes);
        // A rate is not a `QueueEntry` field and never reaches `queue.json`
        // (`download.hpp`), so it comes off the live status and only for the row
        // it was measured on -- a figure drawn against another rom's bar is a
        // number that is simply wrong.
        PutInteger(&item, keys::kQueueBytesPerSecond,
                   status.rom_id == row.rom_id ? status.bytes_per_second : 0);
        PutInteger(&item, keys::kQueueAttempts, row.attempts);
        PutText(&item, keys::kQueueMessage, row.message);
        return item;
      });

  return PageOf(entry, page, served, count);
}

ipc::Error Service::FillFromPlatformsLocked(Entry& entry, ipc::ListPage* page) {
  const std::int64_t count = static_cast<std::int64_t>(entry.platforms.size());
  const std::int64_t served =
      FillPage(page, entry.offset, count, entry.request.page_size, [&](std::int64_t index) {
        const PlatformRow& platform = entry.platforms[static_cast<std::size_t>(index)];
        ipc::ListItem item;
        PutInteger(&item, keys::kPlatformId, platform.id);
        PutText(&item, keys::kPlatformFsSlug, platform.fs_slug);
        PutText(&item, keys::kPlatformName, platform.name);
        PutInteger(&item, keys::kPlatformRomCount, platform.rom_count);
        // Read off the configuration in force *here*, which is the point of the
        // field: the overlay never opens `config.ini` to decide whether a
        // platform has a folder (#25).
        PutFlag(&item, keys::kPlatformMapped, config_.Platform(platform.fs_slug) != nullptr);
        return item;
      });

  // A snapshot cut at `kMaxPlatforms` *ends* at that row: `PageOf` sets
  // `has_more` from the rows left and nothing else, so the browser is never told
  // there is a page after the last one. See `kMaxPlatforms`.
  return PageOf(entry, page, served, count);
}

/// Finish a page filled out of memory: advance the offset, set `has_more`.
///
/// `kTooLarge` for the one thing that would otherwise be a page that never
/// advances: a single row that does not fit a payload on its own. Unreachable
/// while every string is cut at `kMaxRowTextBytes` -- a row is a few hundred
/// bytes against an 8 KiB cap -- and named rather than left silent, because the
/// shape it would otherwise take is a browser asking for the same page once a
/// frame forever. `kTooLarge` is what a bug on *this* side is called
/// (`ipc.hpp`).
ipc::Error Service::PageOf(Entry& entry, ipc::ListPage* page, std::int64_t served,
                           std::int64_t count) {
  if (page->items.empty() && served < count) {
    return ipc::Error::kTooLarge;
  }
  entry.offset = served;
  page->has_more = served < count;
  return ipc::Error::kOk;
}

bool Service::OnDisk(const RomRow& rom, fs::FileSystem* card) const {
  if (card == nullptr || rom.fs_name.empty() || rom.platform_fs_slug.empty()) {
    return false;
  }
  // Every folder the platform maps, not just the one a download would be
  // written to: the later entries are exactly where someone already keeps that
  // platform's roms (config.hpp).
  for (const std::string& candidate :
       config_.ExistingRomPaths({rom.platform_fs_slug, rom.fs_name})) {
    const std::string real = card->Resolve(candidate);
    if (real.empty()) {
      continue;
    }
    const std::int64_t size = FileSizeBytes(real);
    if (size == 0) {
      continue;
    }
    // A declared size that does not match is a different file, or the leftovers
    // of a transfer that never finished. Zero from the server is "it did not
    // say", and then the file being there is all this can honestly report.
    if (rom.size_bytes == 0 || size == rom.size_bytes) {
      return true;
    }
  }
  return false;
}

ipc::ListPage Service::BuildRomPage(const RomPage& fetched, std::int64_t offset,
                                    std::int32_t page_size, fs::FileSystem* card) const {
  ipc::ListPage page;
  std::size_t appended = 0;
  for (const RomRow& rom : fetched.roms) {
    if (static_cast<std::int32_t>(page.items.size()) >= page_size) {
      break;
    }
    const download::QueueEntry queued = queue_.Find(rom.id);
    ipc::ListItem item;
    PutInteger(&item, keys::kRomId, rom.id);
    PutText(&item, keys::kRomName, rom.name);
    PutText(&item, keys::kRomFsName, rom.fs_name);
    PutText(&item, keys::kRomPlatformFsSlug, rom.platform_fs_slug);
    PutInteger(&item, keys::kRomSizeBytes, rom.size_bytes);
    PutFlag(&item, keys::kRomHasMultipleFiles, rom.has_multiple_files);
    PutFlag(&item, keys::kRomOnDisk, OnDisk(rom, card));
    // A terminal row is not "queued": a rom that failed or was skipped is one
    // the user is entitled to ask for again, and a greyed row would leave them
    // no way to (`download::Queue::Enqueue` re-queues a terminal entry on
    // purpose).
    PutFlag(&item, keys::kRomQueued, queued.rom_id != 0 && !download::Terminal(queued.state));
    if (!ipc::AppendIfItFits(&page, std::move(item))) {
      break;
    }
    ++appended;
  }

  // Two ways there is more: the byte cap stopped this page short of what was
  // fetched, or the server has rows past what was fetched. The offset advances
  // by what actually fitted, so the next page starts on the first rom this one
  // could not carry rather than on the first rom of the next request.
  //
  // `appended == 0` over a non-empty fetch would therefore be a page that never
  // advances. It cannot happen while every string is cut at `kMaxRowTextBytes`
  // -- a rom row is a few hundred bytes against an 8 KiB cap -- and
  // `FillFromQueueLocked` says what is done about it if it ever does.
  page.has_more = appended < fetched.roms.size() ||
                  offset + static_cast<std::int64_t>(appended) < fetched.total;
  if (fetched.roms.empty()) {
    // The server counted more than it served: a rom deleted between two of these
    // requests leaves `offset` past the new end (`rom_index.cpp` meets the same
    // shape while paging a whole library). For a *list* the answer is that it
    // ended here -- `has_more` over an empty page is a browser asking for the
    // same offset again, forever, because nothing advanced.
    page.has_more = false;
  }
  return page;
}

ipc::Error Service::ListNext(ipc::Cursor cursor, ipc::ListPage* page) {
  const TimePoint now = Now();
  const std::lock_guard<std::mutex> held(mutex_);
  ReapLocked(now);
  const auto found = Find(cursor);
  if (found == cursors_.end()) {
    return ipc::Error::kBadCursor;
  }
  Entry* entry = &*found;
  entry->touched = now;
  // Cleared here rather than in three places, so every kind answers the same
  // way: a `ListNext` *replaces* the page it is handed. `Dispatch` passes a
  // fresh one, but a caller reusing a `ListPage` across calls -- which the suite
  // does -- would otherwise accumulate rows on the two kinds served out of
  // memory and carry a stale `pending` from an earlier fetch into them.
  *page = ipc::ListPage{};

  if (entry->request.kind == ipc::ListKind::kQueue) {
    return FillFromQueueLocked(*entry, page);
  }
  if (entry->request.kind == ipc::ListKind::kPlatforms && entry->platforms_loaded) {
    return FillFromPlatformsLocked(*entry, page);
  }

  switch (entry->fetch) {
    case Fetch::kReady: {
      entry->fetch = Fetch::kIdle;
      if (entry->request.kind == ipc::ListKind::kPlatforms) {
        // What landed was the snapshot, not a page: every `platforms` page --
        // the first one included -- is served out of it by one function, so the
        // two paths cannot disagree about what page one holds.
        entry->platforms_loaded = true;
        return FillFromPlatformsLocked(*entry, page);
      }
      *page = std::move(entry->ready);
      entry->ready = ipc::ListPage{};
      entry->offset += static_cast<std::int64_t>(page->items.size());
      entry->exhausted = !page->has_more;
      return ipc::Error::kOk;
    }
    case Fetch::kFailed: {
      const ipc::Error failure = entry->failure;
      entry->fetch = Fetch::kIdle;
      return failure;
    }
    case Fetch::kRunning:
      page->pending = true;
      return ipc::Error::kOk;
    case Fetch::kIdle:
      break;
  }

  if (entry->exhausted) {
    // Asked past the end. An empty page rather than another request: the last
    // one already said there was no more.
    page->has_more = false;
    return ipc::Error::kOk;
  }
  if (config_.server.url.empty()) {
    return ipc::Error::kNotConfigured;
  }
  if (client_ == nullptr) {
    // No HTTP backend in this build. See `UseServer`.
    return ipc::Error::kOffline;
  }
  if (entry->consecutive_failures > 0 && now < entry->not_before) {
    // Inside the backoff window: the same failure again, and no request. See
    // `kRetryBackoff`.
    return entry->failure;
  }

  entry->fetch = Fetch::kRunning;
  page->pending = true;
  return ipc::Error::kOk;
}

bool Service::Pump() {
  // What the request needs, copied out under the lock so the call itself is
  // made without holding it. Nothing in this file may block the IPC thread, and
  // a mutex held across a fifteen-second timeout would do exactly that.
  ipc::Cursor id = 0;
  ipc::ListRequest asked;
  std::int64_t offset = 0;
  std::string url;
  std::string token;
  // The two backends are swapped under the lock (`UseServer`, `UseCard`), so
  // they are read under it too and used from the copies -- this function is the
  // one place in this file that runs on another thread.
  http::HttpClient* client = nullptr;
  fs::FileSystem* card = nullptr;
  {
    const std::lock_guard<std::mutex> held(mutex_);
    // A cursor abandoned with a built page on it holds that heap until something
    // reclaims it, and on a console with a worker this loop may be the only
    // thing running -- the overlay is closed, so no command is coming.
    ReapLocked(Now());
    Entry* found = nullptr;
    for (Entry& entry : cursors_) {
      if (entry.fetch == Fetch::kRunning) {
        found = &entry;
        break;
      }
    }
    if (found == nullptr || client_ == nullptr) {
      return false;
    }
    client = client_;
    card = filesystem_;
    id = found->id;
    asked = found->request;
    offset = found->offset;
    token = bearer_token_;
    url = config_.server.url;
  }

  http::Request request;
  if (asked.kind == ipc::ListKind::kPlatforms) {
    request.url = url + "/api/platforms";
  } else {
    // `order_by=id&order_dir=asc` is a **total** order, which is what makes
    // "every rom exactly once across pages" true: the endpoint's default orders
    // by name, and a name is not unique in a RomM library, so a tie straddling
    // a page boundary returns one rom twice and another never
    // (`rom_index.cpp` says the same thing for the same reason).
    //
    // The three `with_*` index flags default to true and each page would
    // otherwise haul the whole library's `rom_id_index` and a freshly
    // aggregated `filter_values` through a JSON parser on a sysmodule heap, all
    // of it discarded here. Turning them off is most of what this request costs.
    request.url = url + "/api/roms?limit=" + std::to_string(asked.page_size) +
                  "&offset=" + std::to_string(offset) +
                  "&order_by=id&order_dir=asc"
                  "&with_char_index=false&with_filter_values=false&with_rom_id_index=false";
    if (asked.platform_id > 0) {
      request.url += "&platform_ids=" + std::to_string(asked.platform_id);
    }
    if (!asked.search.empty()) {
      request.url += "&search_term=" + http::EncodeQueryValue(asked.search);
    }
  }
  if (!token.empty()) {
    request.headers.push_back({"Authorization", "Bearer " + token});
  }
  request.timeout = kRequestTimeout;

  const http::Result sent = client->Send(request);

  ipc::ListPage built;
  std::vector<PlatformRow> platforms;
  // Two names for three outcomes. A request that did not complete and a server
  // that refused it are both `kOffline`: from a screen's point of view there is
  // no library to read either way. A server that answered `200` with something
  // this client cannot parse is `kInternal`, deliberately not `kOffline` --
  // "reached it and could not read it" sends a user somewhere very different
  // from "could not reach it", and it is what a truncated body earns. The one
  // thing neither may be is a short page: that reads as the end of the library.
  ipc::Error failure = ipc::Error::kOk;
  if (!sent.ok() || !sent.successful()) {
    failure = ipc::Error::kOffline;
  } else if (asked.kind == ipc::ListKind::kPlatforms) {
    // The cut is not reported: there is no field on `ListPage` for "this list
    // was cut", and `kMaxPlatforms` is a backstop against a server that is not
    // a RomM rather than a bound a console meets.
    if (!ParsePlatforms(sent.response.body, &platforms).ok()) {
      failure = ipc::Error::kInternal;
    }
  } else {
    RomPage fetched;
    if (!ParseRomPage(sent.response.body, &fetched).ok()) {
      failure = ipc::Error::kInternal;
    } else {
      // Built outside the lock: it reads the card for `on_disk` and the queue
      // for `queued`, and both take locks of their own.
      built = BuildRomPage(fetched, offset, asked.page_size, card);
    }
  }

  const std::lock_guard<std::mutex> held(mutex_);
  const auto found_again = Find(id);
  if (found_again == cursors_.end() || found_again->fetch != Fetch::kRunning) {
    // The cursor was reclaimed, evicted or ended while the request was in
    // flight. The answer is dropped rather than resurrecting it -- a page
    // belonging to a list nobody is reading is heap on a sysmodule.
    return true;
  }
  Entry* entry = &*found_again;
  if (failure != ipc::Error::kOk) {
    entry->fetch = Fetch::kFailed;
    entry->failure = failure;
    ++entry->consecutive_failures;
    // Doubling, capped. `1 << n` is bounded by the cap below rather than by the
    // shift, so a cursor that has failed thirty times does not overflow it.
    const int doublings = std::min(entry->consecutive_failures - 1, 8);
    entry->not_before = Now() + std::min(kMaxRetryBackoff, kRetryBackoff * (1 << doublings));
    return true;
  }

  entry->consecutive_failures = 0;
  entry->failure = ipc::Error::kOk;
  if (asked.kind == ipc::ListKind::kPlatforms) {
    entry->platforms = std::move(platforms);
  }
  entry->ready = std::move(built);
  entry->fetch = Fetch::kReady;
  return true;
}

}  // namespace rommsync::lists

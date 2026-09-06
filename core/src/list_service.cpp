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

/// A string field that must be present and free of embedded NULs, and **may be
/// empty**.
///
/// `json::Reader::Required` refuses an empty string, which is right for an
/// identifier and wrong for every one of these: RomM leaves `name` empty on an
/// unidentified rom and `custom_name` empty on every platform nobody renamed,
/// and refusing those would fail the whole page over a row that draws fine as a
/// file name. An embedded NUL is still refused, for `rom_index.cpp`'s reason:
/// every C API downstream stops at one, so the value that got used would not be
/// the value that was checked.
json::Error ReadText(const json::Value& object, const char* key, std::string* out) {
  const json::Value* value = object.Find(key);
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

json::Error ReadInteger(const json::Value& object, const char* key, std::int64_t* out) {
  const json::Value* value = object.Find(key);
  if (value == nullptr) {
    return Fail(key, "missing");
  }
  if (!value->is_integer()) {
    return Fail(key, "expected a whole number");
  }
  *out = value->integer();
  return {};
}

json::Error ReadFlag(const json::Value& object, const char* key, bool* out) {
  const json::Value* value = object.Find(key);
  if (value == nullptr) {
    return Fail(key, "missing");
  }
  if (!value->is_bool()) {
    return Fail(key, "expected true or false");
  }
  *out = value->boolean();
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
  if (json::Error error = ReadInteger(document.value, "total", &page.total); !error.ok()) {
    return error;
  }
  const json::Value* items = document.value.Find("items");
  if (items == nullptr || !items->is_array()) {
    return Fail("items", "expected an array of roms");
  }

  page.roms.reserve(items->size());
  std::size_t index = 0;
  for (const json::Value& item : items->elements()) {
    const std::string where = "items[" + std::to_string(index++) + "].";
    RomRow rom;
    if (json::Error error = ReadInteger(item, "id", &rom.id); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadText(item, "name", &rom.name); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadText(item, "fs_name", &rom.fs_name); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadText(item, "platform_fs_slug", &rom.platform_fs_slug);
        !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadInteger(item, "fs_size_bytes", &rom.size_bytes); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadFlag(item, "has_multiple_files", &rom.has_multiple_files);
        !error.ok()) {
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
    PlatformRow platform;
    if (json::Error error = ReadInteger(item, "id", &platform.id); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadText(item, "fs_slug", &platform.fs_slug); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadText(item, "name", &platform.name); !error.ok()) {
      return At(where, error);
    }
    if (json::Error error = ReadInteger(item, "rom_count", &platform.rom_count); !error.ok()) {
      return At(where, error);
    }
    // What RomM's own UI draws: `custom_name` when the user renamed the
    // platform and `name` otherwise. Read leniently, because it is absent on
    // nothing this client has seen and a platform is still a platform without
    // it -- unlike the four above, none of which the browser can draw a row or
    // descend into a list without.
    std::string display_name;
    if (ReadText(item, "display_name", &display_name).ok() && !display_name.empty()) {
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

Service::Entry* Service::Find(ipc::Cursor cursor) {
  for (Entry& entry : cursors_) {
    if (entry.id == cursor) {
      return &entry;
    }
  }
  return nullptr;
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
  entry.kind = request.kind;
  entry.page_size = std::clamp(request.page_size, 1, ipc::kMaxPageSize);
  if (entry.kind == ipc::ListKind::kRoms) {
    entry.platform_id = request.platform_id;
    entry.search = request.search;
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
  Entry* entry = Find(cursor);
  if (entry == nullptr) {
    return ipc::Error::kBadCursor;
  }
  cursors_.erase(cursors_.begin() + (entry - cursors_.data()));
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

  std::int64_t index = entry.offset;
  while (index < count && static_cast<std::int32_t>(page->items.size()) < entry.page_size) {
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
    } else {
      const download::QueueEntry& row = rows[static_cast<std::size_t>(index - (complaint ? 1 : 0))];
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
    }
    if (!ipc::AppendIfItFits(page, std::move(item))) {
      break;
    }
    ++index;
  }

  if (page->items.empty() && index < count) {
    // One row does not fit a payload on its own. Unreachable while every string
    // is cut at `kMaxRowTextBytes` -- a queue row is a few hundred bytes against
    // an 8 KiB cap -- and named rather than left as a `break`, because the shape
    // it would otherwise take is a page that never advances and a browser that
    // asks for it once a frame forever. `kTooLarge` is what a bug on *this* side
    // is called (`ipc.hpp`).
    return ipc::Error::kTooLarge;
  }
  entry.offset = index;
  page->has_more = index < count;
  return ipc::Error::kOk;
}

ipc::Error Service::FillFromPlatformsLocked(Entry& entry, ipc::ListPage* page) {
  const std::int64_t count = static_cast<std::int64_t>(entry.platforms.size());
  std::int64_t index = entry.offset;
  while (index < count && static_cast<std::int32_t>(page->items.size()) < entry.page_size) {
    const PlatformRow& platform = entry.platforms[static_cast<std::size_t>(index)];
    ipc::ListItem item;
    PutInteger(&item, keys::kPlatformId, platform.id);
    PutText(&item, keys::kPlatformFsSlug, platform.fs_slug);
    PutText(&item, keys::kPlatformName, platform.name);
    PutInteger(&item, keys::kPlatformRomCount, platform.rom_count);
    // Read off the configuration in force *here*, which is the point of the
    // field: the overlay never opens `config.ini` to decide whether a platform
    // has a folder (#25).
    PutFlag(&item, keys::kPlatformMapped, config_.Platform(platform.fs_slug) != nullptr);
    if (!ipc::AppendIfItFits(page, std::move(item))) {
      break;
    }
    ++index;
  }

  if (page->items.empty() && index < count) {
    return ipc::Error::kTooLarge;  // see `FillFromQueueLocked`
  }
  entry.offset = index;
  // Strictly the rows left. A snapshot cut at `kMaxPlatforms` *ends* at that
  // row: keeping `has_more` true past the last one would have the browser ask
  // for a page that comes back empty and says there is more, forever. See
  // `kMaxPlatforms` for why the cut is not reachable against a RomM 5.2.0.
  page->has_more = index < count;
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
  return page;
}

ipc::Error Service::ListNext(ipc::Cursor cursor, ipc::ListPage* page) {
  const TimePoint now = Now();
  const std::lock_guard<std::mutex> held(mutex_);
  ReapLocked(now);
  Entry* entry = Find(cursor);
  if (entry == nullptr) {
    return ipc::Error::kBadCursor;
  }
  entry->touched = now;

  if (entry->kind == ipc::ListKind::kQueue) {
    return FillFromQueueLocked(*entry, page);
  }
  if (entry->kind == ipc::ListKind::kPlatforms && entry->platforms_loaded) {
    return FillFromPlatformsLocked(*entry, page);
  }

  switch (entry->fetch) {
    case Fetch::kReady: {
      entry->fetch = Fetch::kIdle;
      if (entry->kind == ipc::ListKind::kPlatforms) {
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
  ipc::ListKind kind = ipc::ListKind::kPlatforms;
  std::int64_t platform_id = 0;
  std::string search;
  std::int32_t page_size = 0;
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
    kind = found->kind;
    platform_id = found->platform_id;
    search = found->search;
    page_size = found->page_size;
    offset = found->offset;
    token = bearer_token_;
    url = config_.server.url;
  }

  http::Request request;
  if (kind == ipc::ListKind::kPlatforms) {
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
    request.url = url + "/api/roms?limit=" + std::to_string(page_size) +
                  "&offset=" + std::to_string(offset) +
                  "&order_by=id&order_dir=asc"
                  "&with_char_index=false&with_filter_values=false&with_rom_id_index=false";
    if (platform_id > 0) {
      request.url += "&platform_ids=" + std::to_string(platform_id);
    }
    if (!search.empty()) {
      request.url += "&search_term=" + http::EncodeQueryValue(search);
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
  } else if (kind == ipc::ListKind::kPlatforms) {
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
      built = BuildRomPage(fetched, offset, page_size, card);
    }
  }

  const std::lock_guard<std::mutex> held(mutex_);
  Entry* entry = Find(id);
  if (entry == nullptr || entry->fetch != Fetch::kRunning) {
    // The cursor was reclaimed, evicted or ended while the request was in
    // flight. The answer is dropped rather than resurrecting it -- a page
    // belonging to a list nobody is reading is heap on a sysmodule.
    return true;
  }
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
  if (kind == ipc::ListKind::kPlatforms) {
    entry->platforms = std::move(platforms);
  }
  entry->ready = std::move(built);
  entry->fetch = Fetch::kReady;
  return true;
}

}  // namespace rommsync::lists

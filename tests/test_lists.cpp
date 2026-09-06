// M5-4 (#31): the three lists, paged over IPC.
//
// Everything here goes through `ipc::Dispatch` over a real `SdEngine` rather
// than by calling `lists::Service` directly, because most of what this issue
// has to be right about is only visible on the wire: a page that fits
// `kMaxPayloadBytes`, a field the browser reads by name, a cursor the overlay
// can be handed. `cursors` is the one exception -- the TTL and the retry
// backoff are about a clock, and the engine holds no seam to inject one.
//
// The unit scenarios (`caps`, `cursors`, `queue`) need no server and must stay
// checked with docker stopped: what they pin is a bound on a payload, an
// abandoned cursor, and a list served off a card while the server is gone --
// none of which a running RomM helps with, and the last of which is the whole
// point of the `queue` kind.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "harness.hpp"
#include "rig.hpp"
#include "rommsync/config.hpp"
#include "rommsync/download.hpp"
#include "rommsync/host/curl_http_client.hpp"
#include "rommsync/host/native_file_system.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/json.hpp"
#include "rommsync/list_service.hpp"

namespace config = rommsync::config;
namespace download = rommsync::download;
namespace fs = rommsync::fs;
namespace http = rommsync::http;
namespace ipc = rommsync::ipc;
namespace lists = rommsync::lists;
namespace sysmodule = rommsync::sysmodule;

namespace {

namespace keys = ipc::list_keys;

/// An engine over a sandbox, the service in front of it, and the two backends
/// the console does not have yet.
///
/// Deliberately the same shape as `test_engine.cpp`'s `Console`: a list is
/// something a user opens on a console, and driving it through anything other
/// than the command table would be testing a different object than the one that
/// ships.
class Console {
 public:
  Console(checks::Checks& checks, std::string_view name) : sandbox(checks, name) {
    directory = sandbox.Host(harness::kConfigDir) + "/";
  }

  void Boot() {
    engine.Load(directory);
    core = std::make_unique<ipc::ServiceCore>(engine);
  }

  ipc::Error Call(ipc::Command command, const std::string& request, std::string* response) {
    return ipc::Dispatch(*core, static_cast<std::uint32_t>(command), request, response);
  }

  /// `ListBegin`, through the wire. `*cursor` is set only on `kOk`.
  ipc::Error Open(const ipc::ListRequest& request, ipc::Cursor* cursor) {
    std::string response;
    const ipc::Error answer =
        Call(ipc::Command::kListBegin, ipc::EncodeListRequest(request), &response);
    if (answer == ipc::Error::kOk) {
      *cursor = ipc::DecodeCursor(response).value;
    }
    return answer;
  }

  /// `ListNext`, through the wire. `*bytes` is what the response actually
  /// weighed, which is the only number the payload cap is about.
  ipc::Error Next(ipc::Cursor cursor, ipc::ListPage* page, std::size_t* bytes = nullptr) {
    std::string response;
    const ipc::Error answer = Call(ipc::Command::kListNext, ipc::EncodeCursor(cursor), &response);
    if (bytes != nullptr) {
      *bytes = response.size();
    }
    if (answer == ipc::Error::kOk) {
      *page = ipc::DecodeListPage(response).value;
    }
    return answer;
  }

  ipc::Error Close(ipc::Cursor cursor) {
    std::string response;
    return Call(ipc::Command::kListEnd, ipc::EncodeCursor(cursor), &response);
  }

  ipc::Error Enqueue(std::int64_t rom_id) {
    std::string response;
    return Call(ipc::Command::kEnqueue, ipc::EncodeRomId(rom_id), &response);
  }

  /// Ask for one page and drive `Pump()` until it is there, or give up.
  ///
  /// The loop is the engine's worker as the console will have it, minus the
  /// thread: a `pending` answer is not an error and not a wait, it is "ask
  /// again" (`ipc.hpp`). `kAttempts` bounds it so a bug that never resolves a
  /// page fails the test rather than hanging it.
  ipc::Error Await(ipc::Cursor cursor, ipc::ListPage* page, std::size_t* bytes = nullptr) {
    constexpr int kAttempts = 8;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
      *page = ipc::ListPage{};
      const ipc::Error answer = Next(cursor, page, bytes);
      if (answer != ipc::Error::kOk || !page->pending) {
        return answer;
      }
      engine.PumpLists();
    }
    return ipc::Error::kInternal;
  }

  harness::Sandbox sandbox;
  std::string directory;
  sysmodule::SdEngine engine;
  std::unique_ptr<ipc::ServiceCore> core;
};

// --- reading a page -----------------------------------------------------------

std::string Text(const ipc::ListItem& item, std::string_view key) {
  const ipc::ListValue* value = item.Find(key);
  return value != nullptr && value->type == ipc::ListValue::Type::kString ? value->text
                                                                          : std::string();
}

std::int64_t Number(const ipc::ListItem& item, std::string_view key) {
  const ipc::ListValue* value = item.Find(key);
  return value != nullptr && value->type == ipc::ListValue::Type::kInteger ? value->number : -1;
}

/// -1 when the field is missing or is not a bool, so "absent" and "false" are
/// two different answers -- the browser reads every field by name and renders a
/// row of blanks when one is missing (`overlay_library_model.cpp`).
int Flag(const ipc::ListItem& item, std::string_view key) {
  const ipc::ListValue* value = item.Find(key);
  if (value == nullptr || value->type != ipc::ListValue::Type::kBool) {
    return -1;
  }
  return value->flag ? 1 : 0;
}

/// Every item of a list, in order, with a bound on how many pages it took.
///
/// `has_more` is what ends it, not an empty page: a `pending` page is empty and
/// is not the end of anything.
struct Walk {
  std::vector<ipc::ListItem> items;
  std::size_t pages = 0;
  std::size_t widest = 0;
  ipc::Error error = ipc::Error::kOk;
};

Walk WalkList(Console& console, const ipc::ListRequest& request, ipc::Cursor* keep = nullptr) {
  Walk walk;
  ipc::Cursor cursor = 0;
  walk.error = console.Open(request, &cursor);
  if (walk.error != ipc::Error::kOk) {
    return walk;
  }
  for (int page_number = 0; page_number < 64; ++page_number) {
    ipc::ListPage page;
    std::size_t bytes = 0;
    walk.error = console.Await(cursor, &page, &bytes);
    if (walk.error != ipc::Error::kOk) {
      break;
    }
    walk.widest = std::max(walk.widest, bytes);
    ++walk.pages;
    for (ipc::ListItem& item : page.items) {
      walk.items.push_back(std::move(item));
    }
    if (!page.has_more) {
      break;
    }
  }
  if (keep != nullptr) {
    *keep = cursor;
  } else {
    console.Close(cursor);
  }
  return walk;
}

// --- a client that answers without a server -----------------------------------

/// One rom, as `GET /api/roms` would send it.
std::string RomJson(std::int64_t id, const std::string& name, const std::string& fs_name) {
  return "{\"id\":" + std::to_string(id) + ",\"name\":" + rommsync::json::Quote(name) +
         ",\"fs_name\":" + rommsync::json::Quote(fs_name) +
         ",\"platform_fs_slug\":\"gba\",\"fs_size_bytes\":4096,\"has_multiple_files\":false}";
}

/// A client with a library in it, paged the way RomM pages one.
///
/// It exists for the one case a real RomM cannot be asked to produce on demand:
/// a rom whose `name` and `fs_name` are pathological. Seeding those into the
/// fixture would mean a commercial-looking file in `roms.manifest` and a
/// re-scan; scripting the response is the same test without either.
class FakeClient : public http::HttpClient {
 public:
  explicit FakeClient(std::vector<std::pair<std::string, std::string>> library)
      : library_(std::move(library)) {}

  /// What `GET /api/platforms` answers. A bare array, as 5.2.0 sends one.
  void SetPlatforms(std::string body) { platforms_ = std::move(body); }

  /// Report this many roms in `total` however few are served -- what a library
  /// that lost a rom between two requests looks like from here.
  void OverCount(std::size_t total) { over_count_ = total; }

  http::Result Send(const http::Request& request) override {
    ++requests;
    last_url = request.url;
    if (request.url.find("/api/platforms") != std::string::npos) {
      http::Result answer;
      answer.response.status = 200;
      answer.response.body = platforms_;
      return answer;
    }
    const std::size_t limit = static_cast<std::size_t>(Query(request.url, "limit", 10));
    const std::size_t offset = static_cast<std::size_t>(Query(request.url, "offset", 0));

    std::string items;
    for (std::size_t index = offset; index < library_.size() && index - offset < limit; ++index) {
      if (!items.empty()) {
        items += ",";
      }
      items += RomJson(static_cast<std::int64_t>(index) + 1, library_[index].first,
                       library_[index].second);
    }
    http::Result result;
    result.response.status = 200;
    result.response.body = "{\"items\":[" + items + "],\"total\":" +
                           std::to_string(over_count_ != 0 ? over_count_ : library_.size()) +
                           ",\"limit\":" + std::to_string(limit) +
                           ",\"offset\":" + std::to_string(offset) + "}";
    return result;
  }

  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    ++requests;
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }

  int requests = 0;
  std::string last_url;

 private:
  static long Query(const std::string& url, const std::string& key, long fallback) {
    const std::size_t at = url.find(key + "=");
    if (at == std::string::npos) {
      return fallback;
    }
    return std::strtol(url.c_str() + at + key.size() + 1, nullptr, 10);
  }

  std::vector<std::pair<std::string, std::string>> library_;
  std::string platforms_ = "[]";
  std::size_t over_count_ = 0;
};

/// A client that answers nothing and records that it was asked.
///
/// `queue` may never send a request, and the only way to say that out loud is
/// to hand the engine something that would notice.
class RefusingClient : public http::HttpClient {
 public:
  http::Result Send(const http::Request&) override {
    ++requests;
    http::Result result;
    result.error = http::Error::kConnectFailed;
    result.message = "the queue list must not reach the network";
    return result;
  }
  http::Result Download(const http::Request&, const http::DownloadTarget&) override {
    ++requests;
    http::Result result;
    result.error = http::Error::kConnectFailed;
    return result;
  }
  int requests = 0;
};

// --- the card -----------------------------------------------------------------

std::string ConfigWith(const std::string& url) {
  return "[server]\nurl = " + url + "\n[platform.gba]\nroms = /roms/gba\n" +
         "[platform.nes]\nroms = /roms/nes\n";
}

/// Put a file of exactly `size` bytes on the sandbox card.
///
/// The bytes are not the rom's: `on_disk` is a hint that stats a file, not a
/// digest (`list_service.hpp`), and a test that wrote 126 MB to prove it would
/// be testing the SD card.
bool PutOnCard(Console& console, const std::string& sd_path, std::int64_t size) {
  return console.sandbox.Write(sd_path, std::string(static_cast<std::size_t>(size), 'r'));
}

/// One queue entry, as `queue.json` writes it.
download::QueueEntry Entry(std::int64_t rom_id, const std::string& fs_name,
                           download::QueueState state, const std::string& message = "") {
  download::QueueEntry entry;
  entry.rom_id = rom_id;
  entry.fs_name = fs_name;
  entry.platform_fs_slug = "gba";
  entry.state = state;
  entry.size_bytes = 4096;
  entry.bytes_done = state == download::QueueState::kDone ? 4096 : 0;
  entry.attempts = state == download::QueueState::kFailed ? 3 : 0;
  entry.message = message;
  entry.queued_at = 1'700'000'000;
  return entry;
}

// --- scenarios ----------------------------------------------------------------

/// Nothing on this wire grows with a rom's name.
void Caps(checks::Checks& c) {
  // Four fields, at four lengths, and the last two are past anything a page of
  // 64 could carry: `kMaxPageSize` is a count cap and the byte cap is the
  // binding one (`ipc.hpp`).
  std::vector<std::pair<std::string, std::string>> library;
  for (int index = 0; index < 40; ++index) {
    const std::size_t width = index % 4 == 0 ? 8 : index % 4 == 1 ? 96 : index % 4 == 2 ? 512
                                                                                       : 4096;
    library.push_back({std::string(width, 'n') + std::to_string(index),
                       std::string(width, 'f') + std::to_string(index) + ".gba"});
  }

  Console console(c, "lists-caps");
  c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                 ConfigWith("http://127.0.0.1:1")),
           "a config with a server on it");
  console.Boot();
  FakeClient client(library);
  console.engine.UseServer(&client, "fixture-token");

  for (const std::int32_t asked : {1, 8, 64, 100000}) {
    ipc::ListRequest request;
    request.kind = ipc::ListKind::kRoms;
    request.page_size = asked;
    const Walk walk = WalkList(console, request);
    c.ExpectEq(static_cast<int>(walk.error), static_cast<int>(ipc::Error::kOk),
               "the whole library pages at page_size " + std::to_string(asked));
    c.ExpectEq(walk.items.size(), library.size(),
               "every rom arrives exactly once at page_size " + std::to_string(asked));
    c.Expect(walk.widest <= ipc::kMaxPayloadBytes,
             "no response exceeded the payload cap at page_size " + std::to_string(asked) +
                 " -- widest was " + std::to_string(walk.widest));

    std::set<std::int64_t> seen;
    for (const ipc::ListItem& item : walk.items) {
      seen.insert(Number(item, keys::kRomId));
    }
    c.ExpectEq(seen.size(), library.size(),
               "and no rom twice at page_size " + std::to_string(asked));
  }

  // The cut itself: a name is shortened rather than dropped, and the row still
  // says which rom it is.
  ipc::ListRequest one;
  one.kind = ipc::ListKind::kRoms;
  one.page_size = 1;
  ipc::Cursor cursor = 0;
  c.ExpectEq(static_cast<int>(console.Open(one, &cursor)), static_cast<int>(ipc::Error::kOk),
             "a rom list opens");
  ipc::ListPage page;
  c.ExpectEq(static_cast<int>(console.Await(cursor, &page)), static_cast<int>(ipc::Error::kOk),
             "and its first page arrives");
  c.ExpectEq(page.items.size(), std::size_t{1}, "one rom on it");
  if (!page.items.empty()) {
    c.Expect(Text(page.items[0], keys::kRomName).size() <= lists::kMaxRowTextBytes + 3,
             "a name is cut to the row bound");
    c.ExpectEq(Number(page.items[0], keys::kRomId), std::int64_t{1},
               "and the rom is still named by its id");
  }
  console.Close(cursor);

  // A multi-byte character is not split down the middle: the overlay is handed
  // this to draw, and half a code point is a byte no font has a glyph for.
  const std::string euro = "\xE2\x82\xAC";  // U+20AC, three bytes
  std::string wide;
  while (wide.size() < lists::kMaxRowTextBytes + 12) {
    wide += euro;
  }
  const std::string shortened = lists::Shorten(wide);
  c.Expect(shortened.size() <= lists::kMaxRowTextBytes + 3, "a wide name is cut to the bound");
  c.Expect(shortened.size() >= 3 && shortened.substr(shortened.size() - 3) == "...",
           "and says it was cut");
  const std::string body = shortened.substr(0, shortened.size() - 3);
  c.ExpectEq(body.size() % 3, std::size_t{0}, "the cut landed on a UTF-8 boundary");

  // The other bound, on the one list 5.2.0 refuses to page. It is a backstop
  // against a server that is not a RomM rather than something a console meets,
  // so the parser is driven directly: seeding two hundred and fifty-seven
  // platforms into the fixture would prove the same thing and cost a re-scan.
  std::string platforms = "[";
  for (std::size_t index = 0; index < lists::kMaxPlatforms + 4; ++index) {
    if (index != 0) {
      platforms += ",";
    }
    platforms += "{\"id\":" + std::to_string(index + 1) + ",\"fs_slug\":\"p" +
                 std::to_string(index) + "\",\"name\":\"Platform\",\"rom_count\":1}";
  }
  platforms += "]";
  std::vector<lists::PlatformRow> rows;
  bool truncated = false;
  c.Expect(lists::ParsePlatforms(platforms, &rows, &truncated).ok(),
           "a very long platform array parses");
  c.ExpectEq(rows.size(), lists::kMaxPlatforms, "and is cut at the bound");
  c.Expect(truncated, "which the parser says out loud");

  // And the shape that is not an envelope: this endpoint answers a bare array,
  // and reading it as one is the whole reason it is paged on this side.
  c.Expect(!lists::ParsePlatforms(R"({"items":[],"total":0})", &rows).ok(),
           "an envelope is not a platform list");
  lists::RomPage refused;
  c.Expect(!lists::ParseRomPage("[]", &refused).ok(),
           "and a rom list that lost its envelope is refused rather than read as empty");

  // A server that counts more than it serves. A rom deleted between two of these
  // requests leaves the offset past the new end, and an empty page that still
  // said "there is more" would be a browser asking for the same offset forever
  // -- nothing advanced.
  Console short_(c, "lists-caps-overcount");
  c.Expect(short_.sandbox.Write("/config/rommsync/config.ini",
                                ConfigWith("http://127.0.0.1:1")),
           "a config with a server on it");
  short_.Boot();
  FakeClient counting({{"one", "one.gba"}, {"two", "two.gba"}});
  counting.OverCount(500);
  short_.engine.UseServer(&counting, "fixture-token");
  ipc::ListRequest asked;
  asked.kind = ipc::ListKind::kRoms;
  asked.page_size = 8;
  const Walk cut = WalkList(short_, asked);
  c.ExpectEq(static_cast<int>(cut.error), static_cast<int>(ipc::Error::kOk),
             "a library that lost a rom still pages");
  c.ExpectEq(cut.items.size(), std::size_t{2}, "and serves what the server actually had");
  c.Expect(cut.pages <= 2, "ending rather than asking for the same offset forever -- " +
                               std::to_string(cut.pages) + " pages");
}

/// The two `platforms` lists a live RomM will not produce on demand: an empty
/// library, and one whose whole list fits inside a page.
///
/// The acceptance item asks for both, and a fixture with four platforms can only
/// show one of them -- emptying the fixture's library to prove the other would
/// leave every rig scenario in the suite with nothing to page.
void Platforms(checks::Checks& c) {
  struct Case {
    const char* what;
    const char* body;
    std::size_t rows;
  };
  const Case cases[] = {
      {"an empty platform list", "[]", 0},
      {"a platform list shorter than one page",
       R"([{"id":1,"fs_slug":"gba","name":"Game Boy Advance","rom_count":2}])", 1},
  };
  for (const Case& scenario : cases) {
    Console console(c, std::string("lists-platforms-") + std::to_string(scenario.rows));
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini",
                                   ConfigWith("http://127.0.0.1:1")),
             "a config with a server on it");
    console.Boot();
    FakeClient client({});
    client.SetPlatforms(scenario.body);
    console.engine.UseServer(&client, "fixture-token");

    ipc::ListRequest request;
    request.kind = ipc::ListKind::kPlatforms;
    request.page_size = ipc::kMaxPageSize;
    const Walk walk = WalkList(console, request);
    c.ExpectEq(static_cast<int>(walk.error), static_cast<int>(ipc::Error::kOk),
               std::string(scenario.what) + " is a list, not an error");
    c.ExpectEq(walk.items.size(), scenario.rows, std::string("rows on ") + scenario.what);
    c.ExpectEq(walk.pages, std::size_t{1}, std::string("answered in one page: ") + scenario.what);
    c.ExpectEq(client.requests, 1,
               std::string("and fetched once, not once a page: ") + scenario.what);
  }

  // A rom RomM has no metadata for. `name` is `string | null` on the list schema
  // (docs/API_CONTRACT.md), and a strict read of it would fail the whole page
  // over one unidentified file -- a library that stops listing rather than a row
  // that draws its file name instead.
  lists::RomPage page;
  c.Expect(lists::ParseRomPage(
               R"({"total":1,"items":[{"id":9,"name":null,"fs_name":"x.gba",)"
               R"("platform_fs_slug":"gba","fs_size_bytes":1,"has_multiple_files":false}]})",
               &page)
               .ok(),
           "a rom with no name is read rather than refused");
  c.ExpectEq(page.roms.size(), std::size_t{1}, "and it is on the page");
  if (!page.roms.empty()) {
    c.Expect(page.roms[0].name.empty(), "with an empty name rather than a guessed one");
    c.ExpectEq(page.roms[0].fs_name, std::string("x.gba"), "and its file name intact");
  }
  // The four that are not optional: a row the browser cannot draw or descend
  // into is a page of blanks on a console, so the page fails instead.
  c.Expect(!lists::ParseRomPage(
                R"({"total":1,"items":[{"id":9,"name":"x","fs_name":"x.gba",)"
                R"("fs_size_bytes":1,"has_multiple_files":false}]})",
                &page)
                .ok(),
           "a rom with no platform_fs_slug fails the page");
}

/// Cursors: the cap, the TTL, and what a reclaimed one answers.
void Cursors(checks::Checks& c) {
  Console console(c, "lists-cursors");
  console.Boot();

  // The cap plus one, with nothing closed, still leaves the service answering.
  std::vector<ipc::Cursor> opened;
  ipc::ListRequest request;
  request.kind = ipc::ListKind::kQueue;
  for (std::size_t index = 0; index < lists::kMaxCursors + 1; ++index) {
    ipc::Cursor cursor = 0;
    c.ExpectEq(static_cast<int>(console.Open(request, &cursor)), static_cast<int>(ipc::Error::kOk),
               "cursor " + std::to_string(index) + " opens");
    c.Expect(cursor != 0, "and is never zero");
    opened.push_back(cursor);
  }
  ipc::ListPage page;
  // Deliberately not fresh: `ListNext` replaces the page it is handed, whatever
  // the kind, and a caller reusing one across calls must not accumulate rows or
  // inherit a `pending` from an earlier fetch.
  page.items.push_back(ipc::ListItem{});
  page.pending = true;
  page.has_more = true;
  c.ExpectEq(static_cast<int>(console.Next(opened.back(), &page)),
             static_cast<int>(ipc::Error::kOk), "the newest cursor still pages");
  c.ExpectEq(page.items.size(), std::size_t{0}, "over an empty queue, into a reused page");
  c.Expect(!page.pending && !page.has_more, "which carries nothing from the last call");
  c.ExpectEq(static_cast<int>(console.Next(opened.front(), &page)),
             static_cast<int>(ipc::Error::kBadCursor),
             "and the oldest was reclaimed to make room for it");

  // Never issued, and already ended.
  c.ExpectEq(static_cast<int>(console.Next(999'999, &page)),
             static_cast<int>(ipc::Error::kBadCursor), "a cursor nobody issued is bad");
  c.ExpectEq(static_cast<int>(console.Close(opened.back())), static_cast<int>(ipc::Error::kOk),
             "an open cursor closes");
  c.ExpectEq(static_cast<int>(console.Next(opened.back(), &page)),
             static_cast<int>(ipc::Error::kBadCursor), "and paging it afterwards is bad");
  c.ExpectEq(static_cast<int>(console.Close(opened.back())),
             static_cast<int>(ipc::Error::kBadCursor), "so is closing it twice");

  // The TTL, and that an id is never handed out twice. Both are about a clock,
  // and `SdEngine` holds no seam to inject one -- so this half drives
  // `lists::Service` directly, which is the object that owns them anyway.
  config::Config configuration = config::Defaults();
  download::Queue queue;
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  lists::Service service(lists::Service::FixedConfig(configuration), queue,
                         [&now] { return now; });

  ipc::Cursor first = 0;
  c.ExpectEq(static_cast<int>(service.ListBegin(request, &first)),
             static_cast<int>(ipc::Error::kOk), "a cursor opens on the service");
  now += lists::kCursorTtl - std::chrono::seconds{1};
  c.ExpectEq(static_cast<int>(service.ListNext(first, &page)), static_cast<int>(ipc::Error::kOk),
             "it is alive just inside the TTL");
  now += lists::kCursorTtl + std::chrono::seconds{1};
  c.ExpectEq(static_cast<int>(service.ListNext(first, &page)),
             static_cast<int>(ipc::Error::kBadCursor), "and reclaimed just past it");
  c.ExpectEq(service.open_cursors(), std::size_t{0}, "the reclaim actually freed it");

  ipc::Cursor second = 0;
  c.ExpectEq(static_cast<int>(service.ListBegin(request, &second)),
             static_cast<int>(ipc::Error::kOk), "another cursor opens");
  c.Expect(second != first, "and never carries the reclaimed one's id");
}

/// The `queue` kind: served off the card, and never over the network.
void QueueList(checks::Checks& c) {
  Console console(c, "lists-queue");
  const std::vector<download::QueueEntry> entries = {
      Entry(11, "Alpha.gba", download::QueueState::kActive),
      Entry(12, "Beta.gba", download::QueueState::kQueued),
      Entry(13, "Gamma.gba", download::QueueState::kFailed, "the server answered 404"),
      Entry(14, "Delta.gba", download::QueueState::kDone),
      Entry(15, "Epsilon.gba", download::QueueState::kSkipped, "gba is not mapped to a folder"),
  };
  c.Expect(console.sandbox.Write("/config/rommsync/queue.json",
                                 download::SerializeQueue(entries)),
           "a queue is on the card");
  console.Boot();

  // Pointed at a server it must never call. The engine holds a client that
  // fails and counts, so "never touches the network" is asserted rather than
  // assumed -- which is what makes the criterion about a stopped RomM real.
  RefusingClient client;
  console.engine.UseServer(&client, "fixture-token");

  ipc::ListRequest request;
  request.kind = ipc::ListKind::kQueue;
  request.page_size = 2;
  const Walk walk = WalkList(console, request);
  c.ExpectEq(static_cast<int>(walk.error), static_cast<int>(ipc::Error::kOk), "the queue pages");
  c.ExpectEq(walk.items.size(), entries.size(), "every entry arrives, terminal ones included");
  c.ExpectEq(walk.pages, std::size_t{3}, "in ceil(5/2) pages");
  c.ExpectEq(client.requests, 0, "and the network was never touched");

  // The projection, whole. A field spelled differently on the two sides does
  // not fail to build -- it draws a row of blanks on a console (`ipc.hpp`).
  if (walk.items.size() == entries.size()) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const ipc::ListItem& item = walk.items[index];
      const download::QueueEntry& entry = entries[index];
      const std::string where = " on row " + std::to_string(index);
      c.ExpectEq(Number(item, keys::kQueueRomId), entry.rom_id, "rom_id" + where);
      c.ExpectEq(Text(item, keys::kQueueFsName), entry.fs_name, "fs_name" + where);
      c.ExpectEq(Text(item, keys::kQueuePlatformFsSlug), entry.platform_fs_slug,
                 "platform_fs_slug" + where);
      c.ExpectEq(Text(item, keys::kQueueState), std::string(download::ToString(entry.state)),
                 "state" + where);
      c.ExpectEq(Number(item, keys::kQueueBytesDone), entry.bytes_done, "bytes_done" + where);
      c.ExpectEq(Number(item, keys::kQueueSizeBytes), entry.size_bytes, "size_bytes" + where);
      c.ExpectEq(Number(item, keys::kQueueBytesPerSecond), std::int64_t{0},
                 "bytes_per_second" + where);
      c.ExpectEq(Number(item, keys::kQueueAttempts), static_cast<std::int64_t>(entry.attempts),
                 "attempts" + where);
      c.ExpectEq(Text(item, keys::kQueueMessage), entry.message, "message" + where);
      c.ExpectEq(item.fields.size(), std::size_t{9}, "and nothing else" + where);
    }
  }

  // Fewer than one page, and zero. Both are lists a user opens, and neither may
  // be an error or a page that claims there is more.
  Console few(c, "lists-queue-few");
  c.Expect(few.sandbox.Write("/config/rommsync/queue.json",
                             download::SerializeQueue({Entry(21, "One.gba",
                                                             download::QueueState::kQueued)})),
           "a queue of one is on the card");
  few.Boot();
  ipc::ListRequest wide;
  wide.kind = ipc::ListKind::kQueue;
  wide.page_size = ipc::kMaxPageSize;
  const Walk one = WalkList(few, wide);
  c.ExpectEq(one.items.size(), std::size_t{1}, "a queue of one is one page of one");
  c.ExpectEq(one.pages, std::size_t{1}, "and one page");

  Console none(c, "lists-queue-empty");
  none.Boot();
  const Walk empty = WalkList(none, wide);
  c.ExpectEq(static_cast<int>(empty.error), static_cast<int>(ipc::Error::kOk),
             "an empty queue is a list, not an error");
  c.ExpectEq(empty.items.size(), std::size_t{0}, "with nothing on it");
  c.ExpectEq(empty.pages, std::size_t{1}, "answered in one page");

  // What #22 left here: the complaint about `queue.json` itself. It used to
  // ride on `config_diagnostics()` under a `[downloads]` section because there
  // was nowhere else; this list is the somewhere else.
  Console corrupt(c, "lists-queue-corrupt");
  c.Expect(corrupt.sandbox.Write("/config/rommsync/queue.json", "\x01\x02 not json at all"),
           "a corrupt queue is on the card");
  corrupt.Boot();
  const Walk complained = WalkList(corrupt, wide);
  c.ExpectEq(complained.items.size(), std::size_t{1},
             "a discarded queue is one row rather than an empty screen");
  if (complained.items.size() == 1) {
    const ipc::ListItem& row = complained.items[0];
    c.ExpectEq(Number(row, keys::kQueueRomId), std::int64_t{0}, "the row names no rom");
    c.ExpectEq(Text(row, keys::kQueueFsName), std::string(download::kQueueFileName),
               "it names the file instead");
    c.Expect(!Text(row, keys::kQueueMessage).empty(), "and carries the reason it was discarded");
  }
}

// --- the fixture ---------------------------------------------------------------

/// A console pointed at this worktree's RomM through the fault proxy.
///
/// `on_disk` needs a card as well as a server, so the sandbox gets a
/// `NativeFileSystem` rooted at it -- the same mapping Horizon performs with
/// `sdmc:` (`native_file_system.hpp`).
struct Rig {
  explicit Rig(checks::Checks& checks, std::string_view name)
      : console(checks, name), client(rommsync::host::MakeCurlHttpClient()) {}

  bool Start(checks::Checks& c) {
    if (!harness::LoadFixture(&fixture)) {
      return false;
    }
    c.Expect(console.sandbox.Write("/config/rommsync/config.ini", ConfigWith(rig::BaseUrl())),
             "a config pointing at the fixture");
    console.sandbox.MakeDirs("/roms/gba");
    console.sandbox.MakeDirs("/roms/nes");
    console.Boot();
    card = rommsync::host::MakeNativeFileSystem(console.sandbox.root().string());
    console.engine.UseServer(client.get(), fixture.token);
    console.engine.UseCard(card.get());
    return true;
  }

  Console console;
  std::unique_ptr<http::HttpClient> client;
  std::unique_ptr<fs::FileSystem> card;
  harness::Fixture fixture;
};

/// `total` off a direct `GET /api/roms`, or -1.
std::int64_t LibraryTotal(http::HttpClient& client, const harness::Fixture& fixture,
                          const std::string& query) {
  const http::Result result = client.Send(
      harness::Authed(http::Method::kGet, rig::BaseUrl() + "/api/roms?limit=1" + query, fixture));
  if (!result.successful()) {
    return -1;
  }
  const std::string total = rig::JsonNumber(result.response.body, "total");
  return total.empty() ? -1 : std::strtoll(total.c_str(), nullptr, 10);
}

/// Paging the seeded library, and the two lists that come off it.
int Library(checks::Checks& c) {
  Rig rig_(c, "lists-library");
  if (!rig_.Start(c)) {
    return rig::kSkip;
  }
  if (!rig::Reachable(*rig_.client, rig::BaseUrl())) {
    std::cerr << "RomM is not answering at " << rig::BaseUrl() << " -- skipping\n";
    return rig::kSkip;
  }
  harness::ExpectDisarmed(c, *rig_.client, rig::BaseUrl(), "the proxy starts clean");

  // --- platforms: unpaged on the server, paged here.
  ipc::ListRequest platforms;
  platforms.kind = ipc::ListKind::kPlatforms;
  platforms.page_size = 1;
  const Walk paged = WalkList(rig_.console, platforms);
  c.ExpectEq(static_cast<int>(paged.error), static_cast<int>(ipc::Error::kOk),
             "the platform list pages");
  c.Expect(paged.items.size() >= 4, "the fixture's four platforms are all there -- got " +
                                        std::to_string(paged.items.size()));
  c.ExpectEq(paged.pages, paged.items.size(), "one platform a page at page_size 1");

  std::map<std::string, std::int64_t> platform_ids;
  std::int64_t rom_count_total = 0;
  for (const ipc::ListItem& item : paged.items) {
    const std::string slug = Text(item, keys::kPlatformFsSlug);
    platform_ids[slug] = Number(item, keys::kPlatformId);
    rom_count_total += Number(item, keys::kPlatformRomCount);
    c.Expect(Number(item, keys::kPlatformId) > 0, "a platform carries the id a rom list opens on");
    c.Expect(!Text(item, keys::kPlatformName).empty(), "and a name to draw");
    c.ExpectEq(item.fields.size(), std::size_t{5}, "and the five fields of the projection");
    // `mapped` is this side's answer, read off the configuration in force --
    // the overlay never opens `config.ini` to decide it (#25). Compared against
    // the engine's own `Config` rather than against a hand-written list of
    // slugs: `config::Defaults()` already maps every platform it knows, so a
    // second copy of the expectation here would be asserting what this test
    // guessed rather than what the console will draw.
    c.ExpectEq(Flag(item, keys::kPlatformMapped),
               rig_.console.engine.config().Platform(slug) != nullptr ? 1 : 0,
               "mapped is read off the config in force, for " + slug);
  }

  // The same list at a page size that holds it whole: same platforms, one page.
  platforms.page_size = ipc::kMaxPageSize;
  const Walk whole = WalkList(rig_.console, platforms);
  c.ExpectEq(whole.pages, std::size_t{1}, "the platform list fits one wide page");
  c.ExpectEq(whole.items.size(), paged.items.size(), "and holds the same platforms");

  // --- roms: every rom exactly once, in a stable order, at four page sizes.
  const std::int64_t total = LibraryTotal(*rig_.client, rig_.fixture, "");
  c.Expect(total > 0, "GET /api/roms reports a library");

  std::vector<std::int64_t> first_order;
  for (const std::int32_t size : {1, 2, 3, 5}) {
    ipc::ListRequest roms;
    roms.kind = ipc::ListKind::kRoms;
    roms.page_size = size;
    const Walk walk = WalkList(rig_.console, roms);
    c.ExpectEq(static_cast<int>(walk.error), static_cast<int>(ipc::Error::kOk),
               "the library pages at page_size " + std::to_string(size));
    c.ExpectEq(static_cast<std::int64_t>(walk.items.size()), total,
               "as many roms as GET /api/roms counts, at page_size " + std::to_string(size));
    c.Expect(walk.widest <= ipc::kMaxPayloadBytes, "and no page over the payload cap");

    std::vector<std::int64_t> order;
    for (const ipc::ListItem& item : walk.items) {
      order.push_back(Number(item, keys::kRomId));
      c.ExpectEq(item.fields.size(), std::size_t{8}, "a rom row carries its eight fields");
    }
    std::set<std::int64_t> unique(order.begin(), order.end());
    c.ExpectEq(unique.size(), order.size(),
               "no rom twice at page_size " + std::to_string(size));
    if (first_order.empty()) {
      first_order = order;
    } else {
      c.Expect(order == first_order,
               "the order is the same whatever the page size -- page_size " +
                   std::to_string(size));
    }
  }

  // --- the filter, and the id that opens it.
  const auto gba = platform_ids.find("gba");
  c.Expect(gba != platform_ids.end(), "the fixture has a gba platform");
  if (gba != platform_ids.end()) {
    ipc::ListRequest roms;
    roms.kind = ipc::ListKind::kRoms;
    roms.platform_id = gba->second;
    roms.page_size = 2;
    const Walk walk = WalkList(rig_.console, roms);
    const std::int64_t filtered =
        LibraryTotal(*rig_.client, rig_.fixture, "&platform_ids=" + std::to_string(gba->second));
    c.ExpectEq(static_cast<std::int64_t>(walk.items.size()), filtered,
               "a platform's rom list holds exactly that platform's roms");
    c.Expect(filtered > 0 && filtered < total, "and is a proper subset of the library");
    for (const ipc::ListItem& item : walk.items) {
      c.ExpectEq(Text(item, keys::kRomPlatformFsSlug), std::string("gba"),
                 "every row on it is a gba rom");
      c.ExpectEq(Flag(item, keys::kRomOnDisk), 0, "and none is on this empty card yet");
      c.ExpectEq(Flag(item, keys::kRomQueued), 0, "nor in this empty queue");
    }

    // --- on_disk and queued, which are this side's answers rather than RomM's.
    if (!walk.items.empty()) {
      const ipc::ListItem& rom = walk.items.front();
      const std::int64_t rom_id = Number(rom, keys::kRomId);
      const std::int64_t size_bytes = Number(rom, keys::kRomSizeBytes);
      const std::string fs_name = Text(rom, keys::kRomFsName);
      c.Expect(PutOnCard(rig_.console, "/roms/gba/" + fs_name, size_bytes),
               "the rom is put on the card at the size the server declares");
      c.ExpectEq(static_cast<int>(rig_.console.Enqueue(rom_id)),
                 static_cast<int>(ipc::Error::kOk), "and another press queues it");

      const Walk again = WalkList(rig_.console, roms);
      bool found = false;
      for (const ipc::ListItem& item : again.items) {
        if (Number(item, keys::kRomId) != rom_id) {
          continue;
        }
        found = true;
        c.ExpectEq(Flag(item, keys::kRomOnDisk), 1, "the row now says the card has it");
        c.ExpectEq(Flag(item, keys::kRomQueued), 1, "and that it is queued");
      }
      c.Expect(found, "the rom is still in the list");

      // A file of the right name at the wrong size is not that rom: it is a
      // half-finished transfer or a different dump, and greying the row would
      // leave a user with no way to fetch the one they asked for.
      c.Expect(PutOnCard(rig_.console, "/roms/gba/" + fs_name, size_bytes + 1),
               "the file is replaced with one of the wrong size");
      const Walk resized = WalkList(rig_.console, roms);
      for (const ipc::ListItem& item : resized.items) {
        if (Number(item, keys::kRomId) == rom_id) {
          c.ExpectEq(Flag(item, keys::kRomOnDisk), 0,
                     "a file of the wrong size is not the rom being on the card");
        }
      }
    }
  }

  c.Expect(rom_count_total >= total,
           "the platforms' rom counts cover the library -- " + std::to_string(rom_count_total) +
               " over " + std::to_string(total));

  // --- has_multiple_files, by value. It is on the *list* schema so #25 can grey
  // a disc set out without a second call per rom, and a projection that carried
  // the field and always said `false` would look exactly like this one.
  ipc::ListRequest every;
  every.kind = ipc::ListKind::kRoms;
  every.page_size = ipc::kMaxPageSize;
  const Walk library = WalkList(rig_.console, every);
  int multi = 0;
  for (const ipc::ListItem& item : library.items) {
    const int flag = Flag(item, keys::kRomHasMultipleFiles);
    c.Expect(flag >= 0, "every rom row carries has_multiple_files");
    multi += flag == 1 ? 1 : 0;
  }
  const std::int64_t multi_on_server =
      LibraryTotal(*rig_.client, rig_.fixture, "&search_term=Two%20Disc");
  c.Expect(multi_on_server > 0, "the fixture holds a disc set to grey out");
  c.ExpectEq(multi, static_cast<int>(multi_on_server),
             "and the list reports it as one rather than always answering false");

  // --- search_term, the other half of the `roms` filter.
  ipc::ListRequest searched;
  searched.kind = ipc::ListKind::kRoms;
  searched.search = "Synthetic";
  searched.page_size = 1;
  const Walk found_rows = WalkList(rig_.console, searched);
  const std::int64_t searched_total =
      LibraryTotal(*rig_.client, rig_.fixture, "&search_term=Synthetic");
  c.ExpectEq(static_cast<int>(found_rows.error), static_cast<int>(ipc::Error::kOk),
             "a searched rom list pages");
  c.Expect(searched_total > 0 && searched_total < total,
           "the search matches some of the library and not all of it");
  c.ExpectEq(static_cast<std::int64_t>(found_rows.items.size()), searched_total,
             "and the pages hold exactly what the server matched");
  for (const ipc::ListItem& item : found_rows.items) {
    // RomM matches case-insensitively and the fixture holds both
    // `Synthetic Nested Game` and `synthetic-large`, so the row is lowered
    // before it is compared rather than the term being chosen to avoid one.
    std::string name = Text(item, keys::kRomName);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    c.Expect(name.find("synthetic") != std::string::npos,
             "every row matches the term: " + Text(item, keys::kRomName));
  }
  return 0;
}

/// The three ways a page fails, and what each leaves behind.
int Faults(checks::Checks& c) {
  Rig rig_(c, "lists-faults");
  if (!rig_.Start(c)) {
    return rig::kSkip;
  }
  if (!rig::Reachable(*rig_.client, rig::BaseUrl())) {
    std::cerr << "RomM is not answering at " << rig::BaseUrl() << " -- skipping\n";
    return rig::kSkip;
  }
  harness::ExpectDisarmed(c, *rig_.client, rig::BaseUrl(), "the proxy starts clean");

  ipc::ListRequest roms;
  roms.kind = ipc::ListKind::kRoms;
  roms.page_size = 2;

  struct Case {
    const char* what;
    const char* spec;
    ipc::Error expected;
  };
  // `truncate` sends no `Content-Length`, so the transport cannot catch it
  // (fault_proxy.py) -- the cut body reaches the decoder and is refused there.
  // That is exactly the criterion: never a short page that looks like the end
  // of the library.
  const Case cases[] = {
      {"a stalled request", R"({"mode":"stall","seconds":30,"path":"/api/roms"})",
       ipc::Error::kOffline},
      {"a dropped connection", R"({"mode":"drop","bytes":64,"path":"/api/roms"})",
       ipc::Error::kOffline},
      {"a truncated body", R"({"mode":"truncate","bytes":64,"path":"/api/roms"})",
       ipc::Error::kInternal},
  };

  for (const Case& scenario : cases) {
    ipc::Cursor cursor = 0;
    c.ExpectEq(static_cast<int>(rig_.console.Open(roms, &cursor)),
               static_cast<int>(ipc::Error::kOk), std::string("a list opens before ") +
                                                      scenario.what);
    // One good page first, so the failure is genuinely mid-list rather than at
    // the door: a cursor that never served a page proves nothing about keeping
    // its offset.
    ipc::ListPage first;
    c.ExpectEq(static_cast<int>(rig_.console.Await(cursor, &first)),
               static_cast<int>(ipc::Error::kOk), "and serves a page");
    c.Expect(!first.items.empty(), "with roms on it");
    c.Expect(first.has_more, "and more behind it");

    c.Expect(rig::ArmFault(*rig_.client, rig::BaseUrl(), scenario.spec).successful(),
             std::string("armed ") + scenario.what);
    ipc::ListPage failed;
    const ipc::Error answer = rig_.console.Await(cursor, &failed);
    c.ExpectEq(std::string(ipc::ToString(answer)), std::string(ipc::ToString(scenario.expected)),
               std::string(scenario.what) + " fails the page");
    c.ExpectEq(failed.items.size(), std::size_t{0}, "and serves nothing rather than a short page");
    rig::DisarmFault(*rig_.client, rig::BaseUrl());

    // The cursor survives it. The wait is the backoff, which is the standing
    // rule applied to a list page (`list_service.hpp`): inside the window the
    // same failure is answered again with no request made.
    std::this_thread::sleep_for(lists::kRetryBackoff + std::chrono::milliseconds{300});
    ipc::ListPage retried;
    c.ExpectEq(static_cast<int>(rig_.console.Await(cursor, &retried)),
               static_cast<int>(ipc::Error::kOk),
               std::string("the cursor is usable again after ") + scenario.what);
    c.Expect(!retried.items.empty(), "and the retry serves the page that failed");
    for (const ipc::ListItem& item : retried.items) {
      for (const ipc::ListItem& before : first.items) {
        c.Expect(Number(item, keys::kRomId) != Number(before, keys::kRomId),
                 "which is the page after the last good one, not a repeat of it");
      }
    }
    rig_.console.Close(cursor);
  }

  // And the criterion the queue kind exists for: a fault that would break every
  // request does not touch it.
  c.Expect(rig::ArmFault(*rig_.client, rig::BaseUrl(),
                         R"({"mode":"drop","bytes":0,"path":"/api","count":50})")
               .successful(),
           "armed a drop over the whole API");
  ipc::ListRequest queue;
  queue.kind = ipc::ListKind::kQueue;
  const Walk served = WalkList(rig_.console, queue);
  c.ExpectEq(static_cast<int>(served.error), static_cast<int>(ipc::Error::kOk),
             "the queue is served with every request being dropped");
  rig::DisarmFault(*rig_.client, rig::BaseUrl());
  harness::ExpectDisarmed(c, *rig_.client, rig::BaseUrl(), "the proxy is left clean");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string scenario = argc > 1 ? argv[1] : "caps";
  checks::Checks checks;
  int code = 0;
  if (scenario == "caps") {
    Caps(checks);
  } else if (scenario == "platforms") {
    Platforms(checks);
  } else if (scenario == "cursors") {
    Cursors(checks);
  } else if (scenario == "queue") {
    QueueList(checks);
  } else if (scenario == "library") {
    code = Library(checks);
  } else if (scenario == "faults") {
    code = Faults(checks);
  } else {
    std::cerr << "unknown scenario: " << scenario << "\n";
    return 2;
  }
  if (checks.failures() != 0) {
    std::cerr << scenario << ": " << checks.failures() << " failure(s)\n";
    return 1;
  }
  return code;
}

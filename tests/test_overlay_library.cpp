// The overlay's library browser: the paging, the cursor stack, and what a press
// on a rom does.
//
// M4-3 (#25). The screen itself cannot be checked before the M8-1 gate --
// nothing in this repo draws a frame -- so everything about it that is a
// *decision* lives in `rommsync::overlay::LibraryBrowserModel`, and this is what
// holds it. The model calls nothing: it asks for one command at a time and is
// told what came back, which is exactly what lets a host test drive it through
// paging cases a console would need a ten-thousand-rom library to reproduce.
//
// The pages here are `ipc::ListPage`s built through `ipc::AppendIfItFits` and
// round-tripped through `EncodeListPage`/`DecodeListPage`, not hand-filled
// structs: the model reads its fields *by name* off a flat item, so a suite that
// skipped the codec would agree with itself while a real page decoded to empty
// rows. The field names are `ipc::list_keys`, which is the one copy #31's
// producer will fill from.
//
// No server, no console, no emulator -- pure decisions over pure data, so this
// never skips.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "checks.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace {

namespace ipc = rommsync::ipc;
namespace overlay = rommsync::overlay;

using checks::Checks;
using Command = overlay::LibraryBrowserModel::Command;

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

/// `needle` in `haystack`, but not as the tail of a longer identifier.
///
/// `sync::` is in `rommsync::overlay`, and a grep that did not know that would
/// fail every file in this directory on its own namespace. The rule the greps
/// below want is "names this symbol", which is this.
bool NamesToken(const std::string& haystack, const std::string& needle) {
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + 1)) {
    if (at == 0) {
      return true;
    }
    const char before = haystack[at - 1];
    const bool identifier =
        (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
        (before >= '0' && before <= '9') || before == '_' || before == ':';
    if (!identifier) {
      return true;
    }
  }
  return false;
}

// --- building pages -----------------------------------------------------------

ipc::ListItem Platform(std::int64_t id, const std::string& name, const std::string& fs_slug,
                       std::int64_t rom_count, bool mapped) {
  ipc::ListItem item;
  item.fields.push_back({std::string(ipc::list_keys::kPlatformId), ipc::ListValue::Integer(id)});
  item.fields.push_back({std::string(ipc::list_keys::kPlatformName), ipc::ListValue::Text(name)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kPlatformFsSlug), ipc::ListValue::Text(fs_slug)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kPlatformRomCount), ipc::ListValue::Integer(rom_count)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kPlatformMapped), ipc::ListValue::Flag(mapped)});
  return item;
}

/// A rom row. The four booleans are the whole of what the screen decides a row's
/// state from, so every scenario below is one of them turned.
ipc::ListItem Rom(std::int64_t rom_id, const std::string& name, std::int64_t size_bytes,
                  bool has_multiple_files = false, bool on_disk = false, bool queued = false,
                  const std::string& fs_slug = "gba") {
  ipc::ListItem item;
  item.fields.push_back({std::string(ipc::list_keys::kRomId), ipc::ListValue::Integer(rom_id)});
  item.fields.push_back({std::string(ipc::list_keys::kRomName), ipc::ListValue::Text(name)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kRomFsName), ipc::ListValue::Text(name + ".gba")});
  item.fields.push_back(
      {std::string(ipc::list_keys::kRomPlatformFsSlug), ipc::ListValue::Text(fs_slug)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kRomSizeBytes), ipc::ListValue::Integer(size_bytes)});
  item.fields.push_back({std::string(ipc::list_keys::kRomHasMultipleFiles),
                         ipc::ListValue::Flag(has_multiple_files)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kRomOnDisk), ipc::ListValue::Flag(on_disk)});
  item.fields.push_back({std::string(ipc::list_keys::kRomQueued), ipc::ListValue::Flag(queued)});
  return item;
}

ipc::ListItem QueueEntry(std::int64_t rom_id, const std::string& fs_name,
                         const std::string& state, std::int64_t bytes_done,
                         std::int64_t size_bytes, const std::string& message) {
  ipc::ListItem item;
  item.fields.push_back(
      {std::string(ipc::list_keys::kQueueRomId), ipc::ListValue::Integer(rom_id)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kQueueFsName), ipc::ListValue::Text(fs_name)});
  item.fields.push_back({std::string(ipc::list_keys::kQueueState), ipc::ListValue::Text(state)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kQueueBytesDone), ipc::ListValue::Integer(bytes_done)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kQueueSizeBytes), ipc::ListValue::Integer(size_bytes)});
  item.fields.push_back(
      {std::string(ipc::list_keys::kQueueMessage), ipc::ListValue::Text(message)});
  return item;
}

/// A page, through the wire the sysmodule will actually put it on.
///
/// `AppendIfItFits` is what a producer fills a page with (#31) and
/// `EncodeListPage`/`DecodeListPage` is what carries it, so a projection that
/// grew past `kMaxPayloadBytes` or a field name only this file believes in fails
/// here rather than on a console.
ipc::ListPage Page(Checks& checks, std::vector<ipc::ListItem> items, bool has_more) {
  ipc::ListPage page;
  for (ipc::ListItem& item : items) {
    checks.Expect(ipc::AppendIfItFits(&page, std::move(item)), "the item fits one page");
  }
  page.has_more = has_more;

  const std::string encoded = ipc::EncodeListPage(page);
  checks.Expect(ipc::Fits(encoded), "the page fits kMaxPayloadBytes");
  const ipc::Decoded<ipc::ListPage> decoded = ipc::DecodeListPage(encoded);
  checks.Expect(decoded.ok(), "the page round-trips through the codec");
  return decoded.ok() ? decoded.value : page;
}

ipc::ListPage Pending() {
  ipc::ListPage page;
  page.pending = true;
  return page;
}

// --- driving the model --------------------------------------------------------

/// Open the level the model is asking to open, and answer with `cursor`.
///
/// Returns the request it asked with, so a test can hold the filter against
/// what it descended into rather than trusting it.
ipc::ListRequest Begin(Checks& checks, overlay::LibraryBrowserModel& model,
                       ipc::Cursor cursor) {
  const Command command = model.Next();
  checks.Expect(command.kind == Command::Kind::kListBegin,
                "the model asks to open the list first");
  model.OnCursor(cursor);
  return command.request;
}

/// Answer the next `ListNext` with `page`. Fails the suite if the model asked
/// for something else, which is what makes "it stopped asking" visible.
void Deliver(Checks& checks, overlay::LibraryBrowserModel& model, const ipc::ListPage& page,
             ipc::Cursor expected_cursor, const std::string& what) {
  const Command command = model.Next();
  checks.Expect(command.kind == Command::Kind::kListNext, "the model asks for a page: " + what);
  if (command.kind != Command::Kind::kListNext) {
    return;
  }
  checks.ExpectEq(command.cursor, expected_cursor, "the page is asked for on the open cursor");
  model.OnPage(page);
}

/// Every row the view holds that is a real list row -- the `kMore` sentinel
/// dropped, because it is the screen's marker rather than the library's.
std::vector<overlay::LibraryRow> ListRows(const overlay::LibraryView& view) {
  std::vector<overlay::LibraryRow> rows;
  for (const overlay::LibraryRow& row : view.rows) {
    if (row.kind != overlay::RowKind::kMore) {
      rows.push_back(row);
    }
  }
  return rows;
}

const overlay::LibraryRow* MoreRow(const overlay::LibraryView& view) {
  for (const overlay::LibraryRow& row : view.rows) {
    if (row.kind == overlay::RowKind::kMore) {
      return &row;
    }
  }
  return nullptr;
}

/// Put the selection on the row for `rom_id`, however far down it is.
bool Select(overlay::LibraryBrowserModel& model, std::int64_t rom_id) {
  for (int step = 0; step < 512; ++step) {
    const overlay::LibraryView view = model.Render();
    if (view.selected >= 0 &&
        view.rows[static_cast<std::size_t>(view.selected)].rom_id == rom_id) {
      return true;
    }
    const int before = view.selected;
    model.MoveSelection(1);
    if (model.Render().selected == before) {
      return false;
    }
  }
  return false;
}

// --- the paging cases ---------------------------------------------------------

/// A page that exactly fills the request, then the page after it.
///
/// The boundary case: a producer that ends the library on a page boundary
/// answers a full page with `has_more` set and then an empty one, and a model
/// that read "empty" as "broken" would show the last full page as the whole
/// library or ask forever.
void CheckExactPageBoundary(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 7);

  std::vector<ipc::ListItem> first;
  for (int index = 0; index < 4; ++index) {
    first.push_back(Platform(index + 1, "Platform " + std::to_string(index), "p", 10, true));
  }
  Deliver(checks, model, Page(checks, std::move(first), true), 7, "the first full page");
  checks.ExpectEq(ListRows(model.Render()).size(), std::size_t{4}, "the first page is loaded");

  // The exact boundary: nothing left, and the producer says so on an empty page
  // rather than on the full one before it.
  Deliver(checks, model, Page(checks, {}, false), 7, "the empty page past the boundary");

  const overlay::LibraryView view = model.Render();
  checks.ExpectEq(ListRows(view).size(), std::size_t{4}, "the four rows survive the empty page");
  checks.Expect(MoreRow(view) == nullptr, "the list is finished, so there is no 'more' row");
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "a finished list asks for nothing further");
}

/// A first page that is empty and final -- an empty library, not a failure.
void CheckEmptyList(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 3);
  Deliver(checks, model, Page(checks, {}, false), 3, "the only page");

  const overlay::LibraryView view = model.Render();
  checks.Expect(view.rows.empty(), "an empty library draws no rows");
  checks.ExpectEq(view.selected, -1, "an empty list has no selection");
  checks.Expect(!view.headline.empty(), "an empty library still says something");
  checks.Expect(Contains(view.headline, "No platforms"), "and says what is empty");
  checks.Expect(model.Next().kind == Command::Kind::kNone, "it asks for nothing further");
}

/// A `pending` page is not an empty one.
///
/// The engine is still fetching it and the IPC thread did not wait (#31). A
/// model that counted it as an empty page would end the library at nothing.
void CheckPendingIsNotEmpty(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 11);

  Deliver(checks, model, Pending(), 11, "the page that is not there yet");
  checks.Expect(model.Render().busy, "a pending page reads as busy");
  checks.Expect(model.Render().rows.empty() || MoreRow(model.Render()) != nullptr,
                "and draws no library rows");

  Deliver(checks, model, Pending(), 11, "asked again rather than treated as the end");
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 3, true)}, false), 11,
          "the page, once the engine had it");

  const overlay::LibraryView view = model.Render();
  checks.ExpectEq(ListRows(view).size(), std::size_t{1}, "the page that arrived is drawn");
  checks.Expect(!view.busy, "and the screen is no longer busy");
}

/// A `ListNext` that fails mid-scroll.
///
/// The acceptance case: the loaded pages stay, and the failure is drawn on the
/// page that failed rather than over the screen. Then it is retried on the same
/// cursor, because a failed page does not invalidate one (#31).
void CheckFailedPageKeepsWhatLoaded(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 21);
  Deliver(checks, model,
          Page(checks, {Platform(1, "Game Boy", "gb", 3, true),
                        Platform(2, "Mega Drive", "md", 9, true)},
               true),
          21, "the first page");

  const Command failing = model.Next();
  checks.Expect(failing.kind == Command::Kind::kListNext, "the model asks for the second page");
  model.OnRefused(ipc::Error::kOffline);

  const overlay::LibraryView view = model.Render();
  checks.ExpectEq(ListRows(view).size(), std::size_t{2}, "the loaded rows are intact");
  checks.Expect(view.link == overlay::Link::kOk,
                "a failed page is not a screen that lost the sysmodule");
  const overlay::LibraryRow* more = MoreRow(view);
  checks.Expect(more != nullptr, "the failure is drawn as a row at the end of the loaded ones");
  if (more != nullptr) {
    checks.Expect(!more->label.empty(), "the failed page says so");
    checks.Expect(Contains(more->value, ipc::ToString(ipc::Error::kOffline)),
                  "and names the reason it failed");
    checks.Expect(more->selectable, "and can be pressed to try again");
  }
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "a failed page is not retried every frame");

  // Pressing it. The cursor is still the one that was open: a page that failed
  // does not invalidate it.
  model.MoveSelection(64);
  model.Activate();
  const Command retry = model.Next();
  checks.Expect(retry.kind == Command::Kind::kListNext, "pressing the row asks again");
  checks.ExpectEq(retry.cursor, ipc::Cursor{21}, "on the cursor that was already open");
  model.OnPage(Page(checks, {Platform(3, "SNES", "snes", 4, true)}, false));
  checks.ExpectEq(ListRows(model.Render()).size(), std::size_t{3},
                  "the retried page extends the loaded ones");
}

/// A `ListBegin` that fails: there are no loaded rows for the failure to sit
/// under, so it is the screen. Pressing A opens the list again.
void CheckFailedOpen(Checks& checks) {
  overlay::LibraryBrowserModel model;
  const Command open = model.Next();
  checks.Expect(open.kind == Command::Kind::kListBegin, "it opens the platform list");
  model.OnRefused(ipc::Error::kNotConfigured);

  const overlay::LibraryView view = model.Render();
  checks.Expect(view.rows.size() == 1 && view.rows[0].kind == overlay::RowKind::kMore,
                "a list that never opened draws only the failure");
  checks.Expect(!view.headline.empty(), "and says so as the headline");
  checks.Expect(!view.hint.empty(), "with something to do about it");
  checks.Expect(model.Next().kind == Command::Kind::kNone, "and does not reopen every frame");

  model.Activate();
  checks.Expect(model.Next().kind == Command::Kind::kListBegin, "pressing A opens it again");
}

/// A cursor the sysmodule reclaimed.
///
/// `kBadCursor` is the ordinary case rather than an error (#31): re-open and
/// reload. The rows already drawn stay until the first page off the new cursor
/// replaces them, so the screen does not blink empty -- and it must **replace**
/// them, because a fresh cursor starts at the beginning of the list.
void CheckReclaimedCursorReopens(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 31);
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 3, true)}, true), 31,
          "the first page");

  const Command next = model.Next();
  checks.Expect(next.kind == Command::Kind::kListNext, "it asks for the second page");
  model.OnRefused(ipc::Error::kBadCursor);

  const overlay::LibraryView during = model.Render();
  checks.ExpectEq(ListRows(during).size(), std::size_t{1},
                  "the loaded row stays on screen while the cursor is re-opened");
  const overlay::LibraryRow* more = MoreRow(during);
  checks.Expect(more != nullptr && more->tone != overlay::Tone::kWarn,
                "a reclaimed cursor is not drawn as a failure");

  const Command reopen = model.Next();
  checks.Expect(reopen.kind == Command::Kind::kListBegin, "it opens a new cursor");
  checks.Expect(reopen.request.kind == ipc::ListKind::kPlatforms,
                "for the same list it was reading");
  model.OnCursor(32);

  Deliver(checks, model,
          Page(checks, {Platform(1, "Game Boy", "gb", 3, true),
                        Platform(2, "Mega Drive", "md", 9, true)},
               false),
          32, "the first page off the new cursor");
  checks.ExpectEq(ListRows(model.Render()).size(), std::size_t{2},
                  "the reloaded page replaces the old rows rather than doubling them");
}

/// A producer answering `{items:[], has_more:true}` forever.
///
/// Not a contract case -- a bound on a bug. A `ListNext` per frame with nothing
/// to draw for it is an overlay hammering the sysmodule at 60 Hz, so the list
/// ends with the reason on the row instead.
void CheckEndlessEmptyPagesStop(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 41);
  for (int page = 0; page < overlay::kMaxEmptyPages; ++page) {
    Deliver(checks, model, Page(checks, {}, true), 41,
            "empty page " + std::to_string(page + 1));
  }
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "the model stops asking after kMaxEmptyPages empty pages");
  const overlay::LibraryRow* more = MoreRow(model.Render());
  checks.Expect(more != nullptr, "and says why it stopped");
}

/// Back-navigation.
///
/// The level underneath keeps its rows, its cursor and its selection while the
/// one above is open, and the level being left has its cursor closed -- which
/// is the exception #31 says is rare and this screen can make cheap.
void CheckBackRestoresThePlatformLevel(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 51);
  Deliver(checks, model,
          Page(checks, {Platform(1, "Game Boy", "gb", 3, true),
                        Platform(7, "Mega Drive", "md", 9, true)},
               false),
          51, "the platform page");

  model.MoveSelection(1);
  checks.ExpectEq(model.Render().selected, 1, "the selection moved to the second platform");
  model.Activate();

  const overlay::LibraryView roms = model.Render();
  checks.Expect(roms.level == overlay::LibraryLevel::kRoms, "A descends into the platform");
  checks.ExpectEq(roms.title, std::string("Mega Drive"), "and the level is named after it");
  checks.Expect(roms.can_go_back, "and B would now go back rather than close the screen");

  const ipc::ListRequest request = Begin(checks, model, 52);
  checks.Expect(request.kind == ipc::ListKind::kRoms, "the rom list is opened");
  checks.ExpectEq(request.platform_id, std::int64_t{7},
                  "filtered by the platform's id, not its slug");
  Deliver(checks, model, Page(checks, {Rom(100, "Sonic", 4 * 1024 * 1024)}, false), 52,
          "the rom page");
  checks.ExpectEq(model.open_cursors(), std::size_t{2},
                  "both levels hold a cursor while one is open above the other");

  checks.Expect(model.Back(), "B leaves the rom list");
  const Command close = model.Next();
  checks.Expect(close.kind == Command::Kind::kListEnd, "and the rom cursor is closed");
  checks.ExpectEq(close.cursor, ipc::Cursor{52}, "the one that was open above");
  model.OnEnded();

  const overlay::LibraryView back = model.Render();
  checks.Expect(back.level == overlay::LibraryLevel::kPlatforms, "the platform list is back");
  checks.ExpectEq(ListRows(back).size(), std::size_t{2}, "with its rows still loaded");
  checks.ExpectEq(back.selected, 1, "and the selection where it was left");
  checks.Expect(!back.can_go_back, "and B from here closes the screen");
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "the platform cursor is still open, so nothing is re-fetched");
  checks.ExpectEq(model.open_cursors(), std::size_t{1}, "and the rom cursor is not leaked");

  checks.Expect(!model.Back(), "B at the bottom of the stack tells the screen to close");
}

/// Closing the screen ends every cursor it still holds.
void CheckCloseEndsEveryCursor(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 61);
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 3, true)}, false), 61,
          "the platform page");
  model.Activate();
  Begin(checks, model, 62);
  Deliver(checks, model, Page(checks, {Rom(100, "Tetris", 512 * 1024)}, false), 62,
          "the rom page");

  model.Close();
  std::vector<ipc::Cursor> ended;
  for (int step = 0; step < 8; ++step) {
    const Command command = model.Next();
    if (command.kind == Command::Kind::kNone) {
      break;
    }
    checks.Expect(command.kind == Command::Kind::kListEnd,
                  "a closing screen sends nothing but ListEnd");
    ended.push_back(command.cursor);
    model.OnEnded();
  }
  checks.ExpectEq(ended.size(), std::size_t{2}, "both cursors are closed");
  checks.ExpectEq(model.open_cursors(), std::size_t{0}, "and none is left open");
}

// --- the rows -----------------------------------------------------------------

/// Every `Enqueue` outcome renders its own row state, and the reasons that are
/// not errors are predicted rather than sent.
void CheckEveryEnqueueOutcome(Checks& checks) {
  // The four predicted states, off the projection alone. Two of them are not
  // `ipc::Error`s: the sysmodule accepts both and the worker settles them
  // `kSkipped`/`kDone`, so a screen that did not predict them would let a user
  // press download and watch nothing happen.
  struct Predicted {
    ipc::ListItem item;
    overlay::RowState state;
    const char* what;
  };
  const std::vector<Predicted> predicted = {
      {Rom(1, "Ordinary", 1024), overlay::RowState::kReady, "an ordinary rom"},
      {Rom(2, "Disc set", 1024, true), overlay::RowState::kMultiFile, "a disc set"},
      {Rom(3, "Have it", 1024, false, true), overlay::RowState::kOnDisk, "a rom on the card"},
      {Rom(4, "Waiting", 1024, false, false, true), overlay::RowState::kQueued,
       "a rom already queued"},
  };

  for (const Predicted& scenario : predicted) {
    overlay::LibraryBrowserModel model;
    Begin(checks, model, 71);
    Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 4, true)}, false), 71,
            "the platform page");
    model.Activate();
    Begin(checks, model, 72);
    Deliver(checks, model, Page(checks, {scenario.item}, false), 72, "the rom page");

    const overlay::LibraryView view = model.Render();
    const std::vector<overlay::LibraryRow> drawn = ListRows(view);
    checks.ExpectEq(drawn.size(), std::size_t{1}, "the rom is drawn");
    if (drawn.empty()) {
      continue;
    }
    const overlay::LibraryRow& row = drawn[0];
    checks.ExpectEq(overlay::ToString(row.state), overlay::ToString(scenario.state),
                    std::string("the row state for ") + scenario.what);
    checks.Expect(!row.label.empty(), "every row carries a label");
    if (scenario.state == overlay::RowState::kReady) {
      checks.Expect(row.note.empty(), "a row that would download explains nothing");
      model.Activate();
      const Command command = model.Next();
      checks.Expect(command.kind == Command::Kind::kEnqueue, "and A sends Enqueue");
      checks.ExpectEq(command.rom_id, std::int64_t{1}, "for the rom under the selection");
    } else {
      checks.Expect(!row.note.empty(),
                    std::string("the row names the reason: ") + scenario.what);
      model.Activate();
      checks.Expect(model.Next().kind == Command::Kind::kNone,
                    std::string("and A sends nothing for ") + scenario.what);
    }
  }

  // `already_queued` and `already_on_disk` are distinguishable, which #25 asks
  // for in as many words: one says the rom is on its way, the other says it is
  // already there.
  checks.Expect(overlay::RowStateText(overlay::RowState::kQueued, "gb") !=
                    overlay::RowStateText(overlay::RowState::kOnDisk, "gb"),
                "queued and on-disk do not read the same");
  // And `unmapped_platform`/`multi_file_unsupported` name their reason rather
  // than saying "cannot download".
  checks.Expect(Contains(overlay::RowStateText(overlay::RowState::kUnmapped, "gba"), "gba"),
                "the unmapped reason names the slug the user has to map");
  checks.Expect(Contains(overlay::RowStateText(overlay::RowState::kMultiFile, {}), "disc set"),
                "the multi-file reason names what a disc set is");
}

/// The refusals the sysmodule does answer with, each on the row that was
/// pressed, and each with its own sentence.
void CheckRefusalsLandOnTheRow(Checks& checks) {
  const std::vector<ipc::Error> refusals = {
      ipc::Error::kDuplicate,  ipc::Error::kMultiFile,     ipc::Error::kUnknownRom,
      ipc::Error::kQueueFull,  ipc::Error::kNotConfigured, ipc::Error::kUnavailable,
  };

  std::vector<std::string> sentences;
  for (const ipc::Error error : refusals) {
    overlay::LibraryBrowserModel model;
    Begin(checks, model, 81);
    Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 4, true)}, false), 81,
            "the platform page");
    model.Activate();
    Begin(checks, model, 82);
    Deliver(checks, model,
            Page(checks, {Rom(1, "First", 1024), Rom(2, "Second", 2048)}, false), 82,
            "the rom page");

    checks.Expect(Select(model, 2), "the second rom can be selected");
    model.Activate();
    const Command command = model.Next();
    checks.Expect(command.kind == Command::Kind::kEnqueue, "A sends Enqueue");
    checks.ExpectEq(command.rom_id, std::int64_t{2}, "for the selected rom");
    model.OnRefused(error);

    const std::vector<overlay::LibraryRow> rows = ListRows(model.Render());
    checks.ExpectEq(rows.size(), std::size_t{2}, "both roms are still drawn");
    checks.Expect(rows[0].note.empty(),
                  std::string("the refusal does not land on the other row: ") +
                      ipc::ToString(error));
    checks.Expect(!rows[1].note.empty(),
                  std::string("the refused row says why: ") + ipc::ToString(error));
    checks.ExpectEq(overlay::ToString(rows[1].state),
                    overlay::ToString(overlay::EnqueueRefusalState(error)),
                    std::string("the row state for ") + ipc::ToString(error));
    sentences.push_back(rows[1].note);

    // Idempotent from the screen's side: the row now says what it says, and the
    // button stops sending.
    model.Activate();
    checks.Expect(model.Next().kind == Command::Kind::kNone,
                  std::string("a second press sends nothing after ") + ipc::ToString(error));
  }

  for (std::size_t left = 0; left < sentences.size(); ++left) {
    for (std::size_t right = left + 1; right < sentences.size(); ++right) {
      checks.Expect(sentences[left] != sentences[right],
                    std::string("two refusals do not read the same: ") +
                        ipc::ToString(refusals[left]) + " and " +
                        ipc::ToString(refusals[right]));
    }
  }

  // A `kDuplicate` is not a failure worth a dialog (#29) -- it is the same fact
  // about the console that `queued` on the projection carries, and is the same
  // row state.
  checks.Expect(overlay::EnqueueRefusalState(ipc::Error::kDuplicate) ==
                    overlay::RowState::kQueued,
                "already_queued is the queued row state, not a refusal");
}

/// An accepted `Enqueue` lands on the row it was sent for, even when the
/// selection moved while the call was out.
void CheckAcceptedEnqueueLandsOnItsOwnRow(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 91);
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 4, true)}, false), 91,
          "the platform page");
  model.Activate();
  Begin(checks, model, 92);
  Deliver(checks, model, Page(checks, {Rom(1, "First", 1024), Rom(2, "Second", 2048)}, false),
          92, "the rom page");

  model.Activate();
  const Command command = model.Next();
  checks.Expect(command.kind == Command::Kind::kEnqueue, "A sends Enqueue for the first rom");
  model.MoveSelection(1);
  model.OnEnqueued(3);

  const std::vector<overlay::LibraryRow> rows = ListRows(model.Render());
  checks.ExpectEq(overlay::ToString(rows[0].state), overlay::ToString(overlay::RowState::kQueued),
                  "the answer lands on the row it was sent for");
  checks.Expect(Contains(rows[0].note, "3"), "and the queue position is on it");
  checks.ExpectEq(overlay::ToString(rows[1].state), overlay::ToString(overlay::RowState::kReady),
                  "and not on the row the selection moved to");
}

/// An unmapped platform is walked into, and every rom inside says why it will
/// not download. Hidden is the one thing it may not be (#25).
void CheckUnmappedPlatformIsShownNotHidden(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 101);
  Deliver(checks, model, Page(checks, {Platform(5, "Neo Geo", "neogeo", 12, false)}, false), 101,
          "the platform page");

  const std::vector<overlay::LibraryRow> platforms = ListRows(model.Render());
  checks.ExpectEq(platforms.size(), std::size_t{1}, "the unmapped platform is drawn, not hidden");
  checks.Expect(Contains(platforms[0].note, "neogeo"), "and says which slug has no folder");

  model.Activate();
  const ipc::ListRequest request = Begin(checks, model, 102);
  checks.ExpectEq(request.platform_id, std::int64_t{5}, "it is still walked into");
  Deliver(checks, model, Page(checks, {Rom(9, "Metal Slug", 1024, false, false, false, "neogeo")},
                              false),
          102, "the rom page");

  const overlay::LibraryView view = model.Render();
  const std::vector<overlay::LibraryRow> roms = ListRows(view);
  checks.ExpectEq(roms.size(), std::size_t{1}, "the roms are drawn too");
  checks.ExpectEq(overlay::ToString(roms[0].state),
                  overlay::ToString(overlay::RowState::kUnmapped),
                  "and each one says it would be skipped");
  checks.Expect(Contains(roms[0].note, "neogeo"), "naming the slug");
  checks.Expect(Contains(view.headline, "neogeo"), "and the level says it once at the top");

  model.Activate();
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "pressing A on an unmapped rom sends nothing");
}

/// The queue level: served from `queue.json`, read-only, and carrying the
/// message that says why a rom never arrived (#22).
void CheckQueueLevel(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 111);
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 4, true)}, false), 111,
          "the platform page");

  model.OpenQueue();
  const ipc::ListRequest request = Begin(checks, model, 112);
  checks.Expect(request.kind == ipc::ListKind::kQueue, "the queue is its own list kind");
  Deliver(checks, model,
          Page(checks,
               {QueueEntry(1, "Tetris.gb", "active", 512 * 1024, 1024 * 1024, ""),
                QueueEntry(2, "Sonic.md", "failed", 0, 0, "the server closed the connection")},
               false),
          112, "the queue page");

  const overlay::LibraryView view = model.Render();
  const std::vector<overlay::LibraryRow> rows = ListRows(view);
  checks.ExpectEq(rows.size(), std::size_t{2}, "both entries are drawn");
  checks.Expect(Contains(rows[0].value, "512.0 KiB") && Contains(rows[0].value, "1.0 MiB"),
                "a running download shows the bytes against the total");
  checks.Expect(Contains(rows[1].note, "closed the connection"),
                "a failed entry keeps the reason a user opens this screen to read");
  checks.Expect(!rows[0].selectable && !rows[1].selectable, "the queue is read-only in v1");

  model.Activate();
  checks.Expect(model.Next().kind == Command::Kind::kNone, "so A on a queue row sends nothing");
  checks.Expect(model.Back(), "and B goes back to the library");
}

/// A rom with no declared length, and a nested single-file rom whose `fs_name`
/// has no extension.
///
/// `size_bytes == 0` is a real answer (#22) and must never be drawn as "0 B";
/// and a row's label comes off the projection's `name` rather than being
/// derived from `fs_name`, which for a nested rom is a *directory* (#21).
void CheckSizesAndNames(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 121);
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 4, true)}, false), 121,
          "the platform page");
  model.Activate();
  Begin(checks, model, 122);
  Deliver(checks, model,
          Page(checks, {Rom(1, "No Length", 0), Rom(2, "Synthetic Nested Game", 4096)}, false),
          122, "the rom page");

  const std::vector<overlay::LibraryRow> rows = ListRows(model.Render());
  checks.Expect(rows[0].value.empty(), "a rom with no declared length shows no size");
  checks.ExpectEq(rows[0].label, std::string("No Length"), "and still carries its name");
  checks.ExpectEq(rows[1].label, std::string("Synthetic Nested Game"),
                  "a name is the projection's, never derived from fs_name");
  checks.ExpectEq(rows[1].value, std::string("4.0 KiB"), "and the size is formatted once");
}

/// An item this build cannot read is dropped, not drawn half-filled.
void CheckMalformedItemsAreDropped(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 131);

  ipc::ListItem wrong_type = Platform(1, "Game Boy", "gb", 3, true);
  for (ipc::ListField& field : wrong_type.fields) {
    if (field.key == std::string(ipc::list_keys::kPlatformRomCount)) {
      field.value = ipc::ListValue::Text("three");
    }
  }
  ipc::ListItem missing = Platform(2, "Mega Drive", "md", 9, true);
  missing.fields.pop_back();

  Deliver(checks, model,
          Page(checks, {wrong_type, missing, Platform(3, "SNES", "snes", 4, true)}, false), 131,
          "a page with two items this build cannot read");

  const std::vector<overlay::LibraryRow> rows = ListRows(model.Render());
  checks.ExpectEq(rows.size(), std::size_t{1}, "only the item that decoded is drawn");
  checks.ExpectEq(rows[0].label, std::string("SNES"), "and it is the right one");
}

/// Every row a level can produce carries a label, in every state.
///
/// The guarantee `overlay_status_view.hpp` sets out: a row drawn as an empty
/// string is indistinguishable from an overlay that failed to read something.
void CheckEveryRowCarriesText(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 141);
  // A platform and a rom that reported no name at all, which is what the
  // fallbacks exist for.
  ipc::ListItem nameless = Platform(1, "", "gb", 3, true);
  Deliver(checks, model, Page(checks, {nameless}, true), 141, "a nameless platform");

  for (const overlay::LibraryRow& row : model.Render().rows) {
    checks.Expect(!row.label.empty(),
                  std::string("every row carries a label: ") + overlay::ToString(row.kind));
    const bool explains = row.state != overlay::RowState::kReady &&
                          row.state != overlay::RowState::kInert;
    checks.Expect(!explains || !row.note.empty(),
                  std::string("a row that will not download says why: ") +
                      overlay::ToString(row.state));
  }
  checks.Expect(!model.Render().title.empty(), "and the level is named");
  checks.Expect(!model.Render().headline.empty(), "and says one sentence about itself");
}

/// A sysmodule that went away, and one that came back.
///
/// Everything loaded came off cursors held by a session that has gone, so it is
/// dropped rather than continued: asking a new session to continue a cursor it
/// never issued is `kBadCursor` at best.
void CheckUnreachable(Checks& checks) {
  for (const overlay::Link link :
       {overlay::Link::kNotRunning, overlay::Link::kUnreadable, overlay::Link::kIncompatible}) {
    overlay::LibraryBrowserModel model;
    Begin(checks, model, 151);
    Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 3, true)}, true), 151,
            "the first page");

    model.Next();
    model.OnUnreachable(link);

    const overlay::LibraryView view = model.Render();
    checks.Expect(view.link == link, "the screen reports the link it was given");
    checks.Expect(!view.headline.empty(),
                  std::string("and says something about it: ") + overlay::ToString(link));
    checks.Expect(view.rows.empty(), "with no rows, which would be values it does not have");
    checks.Expect(model.Next().kind == Command::Kind::kNone,
                 "and sends nothing at a sysmodule that is not answering");

    model.OnLinkRestored();
    const Command reopened = model.Next();
    checks.Expect(reopened.kind == Command::Kind::kListBegin,
                  "a link that came back starts the browser again");
    checks.Expect(reopened.request.kind == ipc::ListKind::kPlatforms,
                  "at the platform list rather than at a cursor the new session never issued");
  }
}

/// The headline for an unreachable sysmodule is the status screen's.
///
/// It is the same diagnosis, and a second wording is the same console saying
/// two different things about the same problem depending on which screen is
/// open (`overlay_status_view.hpp` publishes `RenderUnreachable` for this).
void CheckUnreachableWordingIsShared(Checks& checks) {
  // `kIncompatible`'s hint is the two version numbers, so the interface the
  // handshake answered has to reach the view model -- a hint that said "it
  // speaks version 0" would be the diagnosis with the diagnosis taken out.
  constexpr std::uint32_t kOtherInterface = 99;
  for (const overlay::Link link :
       {overlay::Link::kNotRunning, overlay::Link::kUnreadable, overlay::Link::kIncompatible}) {
    overlay::LibraryBrowserModel model;
    model.OnUnreachable(link, kOtherInterface);
    const overlay::StatusView status = overlay::RenderUnreachable(link, kOtherInterface);
    const overlay::LibraryView view = model.Render();
    checks.ExpectEq(view.headline, status.headline,
                    std::string("the same headline as the status screen: ") +
                        overlay::ToString(link));
    checks.ExpectEq(view.hint, status.hint,
                    std::string("and the same hint: ") + overlay::ToString(link));
  }
}

/// Paging follows the selection rather than a scroll event.
///
/// A page is asked for when the selection comes within `kPrefetchRows` of the
/// end of the loaded ones, so a screen that draws more rows at once still works
/// and a user holding down does not stop at every page boundary.
void CheckPagingFollowsTheSelection(Checks& checks) {
  overlay::LibraryBrowserModel model;
  Begin(checks, model, 161);

  std::vector<ipc::ListItem> first;
  for (int index = 0; index < 3 * overlay::kPrefetchRows; ++index) {
    first.push_back(Platform(index + 1, "Platform " + std::to_string(index), "p", 1, true));
  }
  Deliver(checks, model, Page(checks, std::move(first), true), 161, "a long first page");
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "with the selection at the top, no second page is asked for");

  model.MoveSelection(2 * overlay::kPrefetchRows);
  const Command command = model.Next();
  checks.Expect(command.kind == Command::Kind::kListNext,
                "scrolling within kPrefetchRows of the end asks for the next page");
  checks.ExpectEq(command.cursor, ipc::Cursor{161}, "on the open cursor");
}

/// `Next()` hands out one command at a time, and asking twice without
/// answering gives the same one back.
///
/// A press that was handed out and dropped -- a frame that failed to report,
/// anything -- must come back rather than vanish: the user pressed download and
/// the rom would silently never be queued. And an answer to a command that was
/// not the one outstanding is ignored, so a page cannot be delivered into a
/// level it did not come from.
void CheckOneCommandAtATime(Checks& checks) {
  overlay::LibraryBrowserModel model;
  const Command first = model.Next();
  const Command again = model.Next();
  checks.Expect(first.kind == Command::Kind::kListBegin && again.kind == first.kind,
                "asking twice for the opening command gives the same one");

  model.OnCursor(201);
  Deliver(checks, model, Page(checks, {Platform(1, "Game Boy", "gb", 4, true)}, false), 201,
          "the platform page");
  model.Activate();
  Begin(checks, model, 202);
  Deliver(checks, model, Page(checks, {Rom(1, "First", 1024)}, false), 202, "the rom page");

  model.Activate();
  const Command press = model.Next();
  checks.Expect(press.kind == Command::Kind::kEnqueue, "A hands out an Enqueue");
  const Command repeat = model.Next();
  checks.Expect(repeat.kind == Command::Kind::kEnqueue && repeat.rom_id == press.rom_id,
                "and a press that was not answered is handed out again, not lost");

  // An answer to a command that is not the one outstanding changes nothing.
  model.OnPage(Page(checks, {Rom(2, "Ghost", 1024)}, false));
  checks.ExpectEq(ListRows(model.Render()).size(), std::size_t{1},
                  "a page answered against an Enqueue is ignored");

  model.OnEnqueued(1);
  checks.Expect(model.Next().kind == Command::Kind::kNone,
                "and once answered the press is retired");
}

// --- the code rather than the behaviour ---------------------------------------

/// Nothing under `overlay/` writes `config.ini`, and nothing there names a boot
/// flag.
///
/// The same grep `overlay.sync_actions` runs, repeated here for the one thing
/// it cannot cover: it scans a directory, so a rule about that directory is
/// only as good as the suites that scan it, and a screen added while one suite
/// is red would be checked by nothing. Both run on every `ctest`.
void CheckLibraryScreenWritesNothing(Checks& checks) {
  static constexpr const char* kForbidden[] = {
      "ofstream",    "fopen",  "fwrite", "WriteAtomically",     "atomic_file",
      "ApplyEdit",   "boot2",  "flags/", "atmosphere/contents", "SetSyncEnabled",
  };

  bool found_the_screen = false;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(ROMMSYNC_OVERLAY_SOURCE_DIR)) {
    const std::filesystem::path path = entry.path();
    const std::string extension = path.extension().string();
    if (extension != ".cpp" && extension != ".hpp") {
      continue;
    }
    if (path.filename().string() == "library_screen.cpp") {
      found_the_screen = true;
    }
    std::ifstream file(path);
    checks.Expect(file.good(), "the overlay source is readable: " + path.string());
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
      ++number;
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
        continue;
      }
      for (const char* token : kForbidden) {
        checks.Expect(!Contains(line, token),
                      path.filename().string() + ":" + std::to_string(number) + " names " +
                          token + "; the sysmodule owns every write");
      }
    }
  }
  checks.Expect(found_the_screen, "the library screen is in overlay/source/ and was scanned");
}

/// The library screen calls no engine.
///
/// `overlay/` links `core/` and `--gc-sections` drops what no screen references
/// (overlay/AGENTS.md), so the rule that the overlay owns no download logic is
/// not enforced by the link map -- a screen that reached for `download::Queue`
/// would build. Only a grep can say so.
void CheckLibraryScreenCallsNoEngine(Checks& checks) {
  static constexpr const char* kForbidden[] = {
      "download::",  "roms::",  "EnqueueRom", "RomIndex",
      "http::",      "sync::",  "auth::Gate", "state_db",
  };

  const std::filesystem::path screen =
      std::filesystem::path(ROMMSYNC_OVERLAY_SOURCE_DIR) / "library_screen.cpp";
  std::ifstream file(screen);
  checks.Expect(file.good(), "the library screen is readable");
  std::string line;
  int number = 0;
  while (std::getline(file, line)) {
    ++number;
    const std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
      continue;
    }
    for (const char* token : kForbidden) {
      checks.Expect(!NamesToken(line, token),
                    "library_screen.cpp:" + std::to_string(number) + " names " + token +
                        "; the overlay renders what the sysmodule reports and calls no engine");
    }
  }
}

/// The model names no libnx and no libultrahand type (hard rule 4).
void CheckModelStaysPortable(Checks& checks) {
  static constexpr const char* kForbidden[] = {
      "switch.h", "tesla.hpp", "Result", "tsl::", "libnx", "MAKERESULT",
  };
  for (const char* path : {ROMMSYNC_LIBRARY_MODEL_HDR, ROMMSYNC_LIBRARY_MODEL_SRC}) {
    std::ifstream file(path);
    checks.Expect(file.good(), std::string("the view model is readable: ") + path);
    std::string line;
    int number = 0;
    while (std::getline(file, line)) {
      ++number;
      const std::size_t first = line.find_first_not_of(" \t");
      if (first != std::string::npos && line.compare(first, 2, "//") == 0) {
        continue;
      }
      const std::size_t doc = line.find_first_not_of(" \t");
      if (doc != std::string::npos && line.compare(doc, 3, "///") == 0) {
        continue;
      }
      for (const char* token : kForbidden) {
        checks.Expect(!Contains(line, token),
                      std::string(path) + ":" + std::to_string(number) + " names " + token +
                          "; core/ names no host-only or libnx type (hard rule 4)");
      }
    }
  }
}

}  // namespace

int main() {
  Checks checks;
  CheckExactPageBoundary(checks);
  CheckEmptyList(checks);
  CheckPendingIsNotEmpty(checks);
  CheckFailedPageKeepsWhatLoaded(checks);
  CheckFailedOpen(checks);
  CheckReclaimedCursorReopens(checks);
  CheckEndlessEmptyPagesStop(checks);
  CheckBackRestoresThePlatformLevel(checks);
  CheckCloseEndsEveryCursor(checks);
  CheckEveryEnqueueOutcome(checks);
  CheckRefusalsLandOnTheRow(checks);
  CheckAcceptedEnqueueLandsOnItsOwnRow(checks);
  CheckUnmappedPlatformIsShownNotHidden(checks);
  CheckQueueLevel(checks);
  CheckSizesAndNames(checks);
  CheckMalformedItemsAreDropped(checks);
  CheckEveryRowCarriesText(checks);
  CheckUnreachable(checks);
  CheckUnreachableWordingIsShared(checks);
  CheckPagingFollowsTheSelection(checks);
  CheckOneCommandAtATime(checks);
  CheckLibraryScreenWritesNothing(checks);
  CheckLibraryScreenCallsNoEngine(checks);
  CheckModelStaysPortable(checks);

  if (checks.failures() > 0) {
    std::cerr << checks.failures() << " failure(s)\n";
    return 1;
  }
  std::cout << "ok: the browser pages, keeps what loaded when a page fails, and says why a rom "
               "will not download\n";
  return 0;
}

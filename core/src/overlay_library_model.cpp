#include "rommsync/overlay_library_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {
namespace {

namespace keys = ipc::list_keys;

/// The label a level with no name falls back to. Never drawn for a platform
/// that reported one; it is here so `LibraryView::title` is non-empty in every
/// state, including a rom list opened from a platform whose `name` was empty.
constexpr const char* kUnnamed = "Unnamed";

/// One field, or nullptr. A wrapper only so the three readers below read as
/// three lists of fields rather than as three loops.
const ipc::ListValue* Field(const ipc::ListItem& item, std::string_view key,
                            ipc::ListValue::Type type) {
  const ipc::ListValue* value = item.Find(key);
  if (value == nullptr || value->type != type) {
    return nullptr;
  }
  return value;
}

bool ReadText(const ipc::ListItem& item, std::string_view key, std::string* out) {
  const ipc::ListValue* value = Field(item, key, ipc::ListValue::Type::kString);
  if (value == nullptr) {
    return false;
  }
  *out = value->text;
  return true;
}

bool ReadInteger(const ipc::ListItem& item, std::string_view key, std::int64_t* out) {
  const ipc::ListValue* value = Field(item, key, ipc::ListValue::Type::kInteger);
  if (value == nullptr) {
    return false;
  }
  *out = value->number;
  return true;
}

bool ReadFlag(const ipc::ListItem& item, std::string_view key, bool* out) {
  const ipc::ListValue* value = Field(item, key, ipc::ListValue::Type::kBool);
  if (value == nullptr) {
    return false;
  }
  *out = value->flag;
  return true;
}

/// "48 roms", "1 rom", "Empty" -- never a bare number, which reads as an id.
std::string RomCountText(std::int64_t count) {
  if (count <= 0) {
    return "Empty";
  }
  if (count == 1) {
    return "1 rom";
  }
  return std::to_string(count) + " roms";
}

/// A queue entry's right-hand column: how far it has got, or what it ended as.
///
/// `size_bytes == 0` is a real answer from a server that declared no length
/// (#22), so the bytes already moved are shown on their own rather than
/// against a total that does not exist. Never a synthesised percentage.
///
/// The rate is only shown while it means something: `bytes_per_second` is the
/// live worker's, so it is zero for every entry that is not the one being
/// downloaded, and "0 B/s" beside a queued row reads as a stalled download.
std::string QueueProgressText(const std::string& state, std::int64_t bytes_done,
                              std::int64_t size_bytes, std::int64_t bytes_per_second) {
  std::string text = state;
  if (bytes_done > 0 || size_bytes > 0) {
    text += " -- ";
    text += FormatBytes(bytes_done);
    if (size_bytes > 0) {
      text += " of ";
      text += FormatBytes(size_bytes);
    }
  }
  if (bytes_per_second > 0) {
    text += " at ";
    text += FormatBytes(bytes_per_second);
    text += "/s";
  }
  return text;
}

/// What a queue row has to say for itself under its name.
///
/// The message is why a rom never arrived, and this screen is the only place a
/// user can read it (#22): a `failed` row kept in the file with its reason
/// dropped is a row that says nothing. The attempt count goes in front of it
/// because a download on its fourth try and one that failed once are different
/// situations wearing the same message.
std::string QueueNoteText(const std::string& message, std::int64_t attempts) {
  if (attempts <= 1) {
    return message;
  }
  const std::string tries = "attempt " + std::to_string(attempts);
  return message.empty() ? tries : tries + " -- " + message;
}

}  // namespace

const char* ToString(LibraryLevel level) {
  switch (level) {
    case LibraryLevel::kPlatforms:
      return "platforms";
    case LibraryLevel::kRoms:
      return "roms";
    case LibraryLevel::kQueue:
      return "queue";
  }
  return "platforms";
}

const char* ToString(RowKind kind) {
  switch (kind) {
    case RowKind::kPlatform:
      return "platform";
    case RowKind::kRom:
      return "rom";
    case RowKind::kQueueEntry:
      return "queue_entry";
    case RowKind::kMore:
      return "more";
  }
  return "rom";
}

const char* ToString(RowState state) {
  switch (state) {
    case RowState::kReady:
      return "ready";
    case RowState::kQueued:
      return "queued";
    case RowState::kOnDisk:
      return "on_disk";
    case RowState::kUnmapped:
      return "unmapped_platform";
    case RowState::kMultiFile:
      return "multi_file_unsupported";
    case RowState::kRefused:
      return "refused";
    case RowState::kInert:
      return "inert";
  }
  return "inert";
}

RowState PredictEnqueue(const RomFacts& facts) {
  // A skip reason first, then where the rom already is. A disc set and an
  // unmapped platform are the two reasons it will *never* arrive, and that is
  // what a user needs before "it is already queued" -- a rom sitting in the
  // queue on an unmapped platform is one the worker will settle `kSkipped`.
  if (facts.has_multiple_files) {
    return RowState::kMultiFile;
  }
  if (!facts.platform_mapped) {
    return RowState::kUnmapped;
  }
  if (facts.on_disk) {
    return RowState::kOnDisk;
  }
  if (facts.queued) {
    return RowState::kQueued;
  }
  return RowState::kReady;
}

std::string RowStateText(RowState state, std::string_view fs_slug) {
  switch (state) {
    case RowState::kQueued:
      return "In the download queue";
    case RowState::kOnDisk:
      return "Already on the card";
    case RowState::kUnmapped:
      // The slug is what the user has to add a line for, so it is in the
      // sentence -- a "no folder for this platform" they cannot act on is worse
      // than none (docs/CONFIG.md keys the map by exactly this).
      return fs_slug.empty() ? std::string("No folder for this platform in config.ini")
                             : "No folder for " + std::string(fs_slug) + " in config.ini";
    case RowState::kMultiFile:
      return "A disc set; this version downloads single-file roms only";
    case RowState::kRefused:
      // The caller has the error and its own sentence; this is the fallback for
      // a row whose reason was not recorded, and it still says something.
      return "The sysmodule refused this download";
    case RowState::kReady:
    case RowState::kInert:
      return std::string();
  }
  return std::string();
}

Tone RowStateTone(RowState state) {
  switch (state) {
    case RowState::kQueued:
      return Tone::kGood;
    case RowState::kOnDisk:
      return Tone::kGood;
    case RowState::kUnmapped:
    case RowState::kMultiFile:
      return Tone::kWarn;
    case RowState::kRefused:
      return Tone::kBad;
    case RowState::kReady:
    case RowState::kInert:
      return Tone::kNeutral;
  }
  return Tone::kNeutral;
}

std::string EnqueueRefusalText(ipc::Error error) {
  switch (error) {
    case ipc::Error::kDuplicate:
      return RowStateText(RowState::kQueued, {});
    case ipc::Error::kMultiFile:
      return RowStateText(RowState::kMultiFile, {});
    case ipc::Error::kUnknownRom:
      // The library moved under the cursor, which #31 says a page is allowed to
      // do: a cursor holds an offset and a filter, not a snapshot.
      return "The server no longer has this rom";
    case ipc::Error::kQueueFull:
      return "The download queue is full";
    case ipc::Error::kNotConfigured:
      return "No server set";
    case ipc::Error::kUnavailable:
      return "This sysmodule cannot download yet";
    case ipc::Error::kOffline:
      return "The server could not be reached";
    default:
      break;
  }
  return "The sysmodule refused this download";
}

RowState EnqueueRefusalState(ipc::Error error) {
  switch (error) {
    case ipc::Error::kDuplicate:
      // Not a failure worth a dialog (#29). The console's answer is "that rom
      // is in the queue", which is the same fact `queued` on the projection
      // carries and is drawn the same way.
      return RowState::kQueued;
    case ipc::Error::kMultiFile:
      return RowState::kMultiFile;
    default:
      break;
  }
  return RowState::kRefused;
}

// --- the model ----------------------------------------------------------------

LibraryBrowserModel::LibraryBrowserModel() { Reset(); }

void LibraryBrowserModel::Reset() {
  stack_.clear();
  closing_.clear();
  enqueueing_ = 0;
  enqueue_wanted_ = 0;
  issued_ = Command::Kind::kNone;
  closed_ = false;

  Level platforms;
  platforms.kind = LibraryLevel::kPlatforms;
  platforms.request.kind = ipc::ListKind::kPlatforms;
  platforms.title = "Library";
  stack_.push_back(std::move(platforms));
}

LibraryBrowserModel::Level& LibraryBrowserModel::top() { return stack_.back(); }
const LibraryBrowserModel::Level& LibraryBrowserModel::top() const { return stack_.back(); }

std::size_t LibraryBrowserModel::open_cursors() const {
  std::size_t open = closing_.size();
  for (const Level& level : stack_) {
    if (level.cursor != 0) {
      ++open;
    }
  }
  return open;
}

LibraryBrowserModel::Command LibraryBrowserModel::Next() {
  Command command;
  if (link_ != Link::kOk) {
    // Nothing to send and nothing to close: the session those cursors belonged
    // to is what went away. `OnLinkRestored` starts again.
    issued_ = Command::Kind::kNone;
    return command;
  }

  // Cursors first, and before anything this screen wants for itself: #31 caps
  // how many may be open at once, so a level the user has already left is
  // holding one of a small number away from the level they are looking at.
  if (!closing_.empty()) {
    command.kind = Command::Kind::kListEnd;
    command.cursor = closing_.front();
    issued_ = command.kind;
    issued_depth_ = stack_.size();
    return command;
  }

  if (closed_) {
    // On its way out. Every cursor it held is in `closing_` and has just been
    // drained above; re-opening the list to page it would hold a cursor nobody
    // is left to end.
    issued_ = Command::Kind::kNone;
    return command;
  }

  if (enqueue_wanted_ != 0) {
    command.kind = Command::Kind::kEnqueue;
    command.rom_id = enqueue_wanted_;
    // Not cleared here: `Next()` is a question about what to send, and asking it
    // twice without answering must give the same answer -- the queue would
    // otherwise be missing the press a caller asked about and forgot to report.
    // The press is retired by whichever `On...` answers it.
    enqueueing_ = enqueue_wanted_;
    issued_ = command.kind;
    issued_depth_ = stack_.size();
    return command;
  }

  Level& level = top();
  if (level.cursor == 0) {
    if (level.page_error != ipc::Error::kOk) {
      // The list could not be opened. Waiting to be asked again rather than
      // asking every frame -- `Activate` on the `kMore` row is the ask.
      issued_ = Command::Kind::kNone;
      return command;
    }
    command.kind = Command::Kind::kListBegin;
    command.request = level.request;
    level.loading = true;
    issued_ = command.kind;
    issued_depth_ = stack_.size();
    return command;
  }

  const bool near_the_end =
      level.rows.empty() ||
      level.selected + kPrefetchRows >= static_cast<int>(level.rows.size());
  if (level.has_more && level.page_error == ipc::Error::kOk && near_the_end) {
    command.kind = Command::Kind::kListNext;
    command.cursor = level.cursor;
    level.loading = true;
    issued_ = command.kind;
    issued_depth_ = stack_.size();
    return command;
  }

  level.loading = false;
  issued_ = Command::Kind::kNone;
  return command;
}

void LibraryBrowserModel::OnCursor(ipc::Cursor cursor) {
  // Only the command that was handed out, and only into the level it was asked
  // for. The screen sends synchronously, so nothing can arrive out of order
  // today -- this is what keeps that true when something else drives the model,
  // because a rom page answered into the platform level decodes to no rows at
  // all and starts that level counting toward `kMaxEmptyPages`.
  if (issued_ != Command::Kind::kListBegin || issued_depth_ != stack_.size()) {
    return;
  }
  issued_ = Command::Kind::kNone;
  Level& level = top();
  level.cursor = cursor;
  level.page_error = ipc::Error::kOk;
  level.empty_pages = 0;
  // A fresh cursor starts at the beginning of the list, so whatever is on
  // screen is about to be said again. `replace_rows` is what stops the second
  // telling from being appended to the first.
  level.replace_rows = !level.rows.empty();
  level.has_more = true;
}

void LibraryBrowserModel::OnPage(const ipc::ListPage& page) {
  if (issued_ != Command::Kind::kListNext || issued_depth_ != stack_.size()) {
    return;
  }
  issued_ = Command::Kind::kNone;
  Level& level = top();
  level.page_error = ipc::Error::kOk;

  if (page.pending) {
    // The engine is still fetching it and the IPC thread did not wait (#31).
    // Nothing is appended and nothing is concluded about the end of the list --
    // an empty `pending` page counted as an empty one would end the library.
    level.loading = true;
    return;
  }

  if (level.replace_rows) {
    level.rows.clear();
    level.unreadable_items = 0;
    level.truncated = false;
    level.replace_rows = false;
  }

  for (const ipc::ListItem& item : page.items) {
    LibraryRow row;
    bool read = false;
    switch (level.kind) {
      case LibraryLevel::kPlatforms:
        read = ReadPlatform(item, &row);
        break;
      case LibraryLevel::kRoms:
        read = ReadRom(item, level.platform_mapped, level.platform_fs_slug, &row);
        break;
      case LibraryLevel::kQueue:
        read = ReadQueueEntry(item, &row);
        break;
    }
    if (!read) {
      // An item this build cannot read is dropped rather than drawn
      // half-filled, for the reason every decoder in `ipc.hpp` refuses a
      // partial payload: defaulted fields render as a library with odd numbers
      // in it, and a user cannot tell that from a library that is odd.
      //
      // Counted rather than only dropped, though. The two halves ship
      // separately, so a field this build requires and that one omits empties a
      // whole page -- and a queue screen saying "nothing in the download queue"
      // while a download is running is the worst possible way to report a
      // contract mismatch.
      ++level.unreadable_items;
      continue;
    }
    level.rows.push_back(std::move(row));
  }

  if (page.items.empty() && page.has_more) {
    ++level.empty_pages;
    if (level.empty_pages >= kMaxEmptyPages) {
      // A producer bug rather than a contract case; see `kMaxEmptyPages`.
      level.has_more = false;
      level.page_error = ipc::Error::kInternal;
      level.loading = false;
      return;
    }
  } else {
    level.empty_pages = 0;
  }

  level.has_more = page.has_more;
  if (static_cast<int>(level.rows.size()) >= kMaxLoadedRows) {
    // Bounded, like everything else on this wire (`kMaxLoadedRows`). The row at
    // the end says the list was *cut* rather than that it ended, which is not
    // the same sentence: a user who is told a list finished stops looking.
    level.truncated = level.has_more || level.rows.size() > static_cast<std::size_t>(kMaxLoadedRows);
    level.has_more = false;
    level.rows.resize(static_cast<std::size_t>(kMaxLoadedRows));
  }
  level.loading = false;
  if (level.selected >= static_cast<int>(level.rows.size())) {
    level.selected = level.rows.empty() ? 0 : static_cast<int>(level.rows.size()) - 1;
  }
}

void LibraryBrowserModel::OnEnded() {
  if (issued_ != Command::Kind::kListEnd) {
    return;
  }
  issued_ = Command::Kind::kNone;
  if (!closing_.empty()) {
    closing_.erase(closing_.begin());
  }
}

void LibraryBrowserModel::OnEnqueued(std::int32_t position) {
  if (issued_ != Command::Kind::kEnqueue) {
    return;
  }
  issued_ = Command::Kind::kNone;
  const std::int64_t rom_id = enqueueing_;
  enqueueing_ = 0;
  enqueue_wanted_ = 0;
  if (rom_id == 0) {
    return;
  }
  for (LibraryRow& row : top().rows) {
    if (row.kind != RowKind::kRom || row.rom_id != rom_id) {
      continue;
    }
    row.state = RowState::kQueued;
    // The position is the one thing an accepted `Enqueue` says that the row did
    // not already know, and it is what makes a press on a long queue visible:
    // "In the download queue" alone reads the same for a rom that is next and
    // one that is fortieth.
    row.note = position > 0 ? "Queued, number " + std::to_string(position)
                            : RowStateText(RowState::kQueued, {});
    row.tone = RowStateTone(RowState::kQueued);
    return;
  }
}

void LibraryBrowserModel::OnRefused(ipc::Error error) {
  const Command::Kind issued = issued_;
  issued_ = Command::Kind::kNone;

  switch (issued) {
    case Command::Kind::kListEnd:
      // A cursor the sysmodule had already reclaimed. That is the ordinary case
      // rather than a failure (#31), and the cursor is gone either way.
      if (!closing_.empty()) {
        closing_.erase(closing_.begin());
      }
      return;

    case Command::Kind::kEnqueue: {
      const std::int64_t rom_id = enqueueing_;
      enqueueing_ = 0;
      enqueue_wanted_ = 0;
      for (LibraryRow& row : top().rows) {
        if (row.kind != RowKind::kRom || row.rom_id != rom_id) {
          continue;
        }
        row.state = EnqueueRefusalState(error);
        row.note = EnqueueRefusalText(error);
        row.tone = RowStateTone(row.state);
        return;
      }
      return;
    }

    case Command::Kind::kListBegin: {
      Level& level = top();
      level.loading = false;
      level.page_error = error;
      return;
    }

    case Command::Kind::kListNext: {
      Level& level = top();
      level.loading = false;
      if (error == ipc::Error::kBadCursor) {
        // Reclaimed, never issued, or ended (#31): re-open rather than report.
        // The rows already loaded stay on screen until the first page off the
        // new cursor replaces them, so this costs the user nothing visible.
        level.cursor = 0;
        level.replace_rows = !level.rows.empty();
        level.has_more = true;
        level.page_error = ipc::Error::kOk;
        return;
      }
      // A failed page is a failed page: the cursor is still usable, the loaded
      // rows are untouched, and the reason goes on the `kMore` row at the end
      // of them rather than over the screen.
      level.page_error = error;
      return;
    }

    case Command::Kind::kNone:
      return;
  }
}

void LibraryBrowserModel::OnUnreachable(Link link, std::uint32_t sysmodule_interface) {
  issued_ = Command::Kind::kNone;
  if (link == Link::kOk) {
    return;
  }
  link_ = link;
  sysmodule_interface_ = sysmodule_interface;
  enqueueing_ = 0;
  enqueue_wanted_ = 0;
}

void LibraryBrowserModel::OnLinkRestored() {
  link_ = Link::kOk;
  // Everything loaded came off cursors held by a session that has gone. Asking
  // the new one to continue them would be asking about cursor numbers it never
  // issued -- which is `kBadCursor` at best and somebody else's list at worst.
  Reset();
}

void LibraryBrowserModel::MoveSelection(int delta) {
  Level& level = top();
  if (level.rows.empty() && level.page_error == ipc::Error::kOk && !level.has_more) {
    return;
  }
  // The `kMore` row is drawn below the loaded ones when there is one, so the
  // selection may stand on it -- that is how a failed page is retried.
  const int rows = static_cast<int>(level.rows.size());
  const bool has_more_row =
      level.has_more || level.loading || level.page_error != ipc::Error::kOk;
  const int last = rows + (has_more_row ? 1 : 0) - 1;
  if (last < 0) {
    level.selected = 0;
    return;
  }
  level.selected = std::clamp(level.selected + delta, 0, last);
}

void LibraryBrowserModel::Activate() {
  Level& level = top();
  const int rows = static_cast<int>(level.rows.size());
  if (level.selected >= rows) {
    // The `kMore` row. A page in flight has nothing to press; a page that
    // failed is asked for again, which is either a `ListNext` on the cursor
    // that is still good or a `ListBegin` for a list that never opened.
    if (level.page_error != ipc::Error::kOk) {
      level.page_error = ipc::Error::kOk;
      level.empty_pages = 0;
      level.has_more = true;
    }
    return;

  }
  if (level.selected < 0 || rows == 0) {
    return;
  }

  const LibraryRow& row = level.rows[static_cast<std::size_t>(level.selected)];
  switch (row.kind) {
    case RowKind::kPlatform: {
      Level roms;
      roms.kind = LibraryLevel::kRoms;
      roms.request.kind = ipc::ListKind::kRoms;
      roms.request.platform_id = row.platform_id;
      roms.title = row.label;
      roms.platform_mapped = row.state != RowState::kUnmapped;
      roms.platform_fs_slug = row.fs_slug;
      stack_.push_back(std::move(roms));
      return;
    }

    case RowKind::kRom:
      // `kRefused` is pressable again on purpose. The four predicted states are
      // facts about the rom and do not change while the level is open, but a
      // refusal the sysmodule answered with is often a fact about the *moment*
      // -- a full queue that drains, a server that comes back -- and a row that
      // took one and went dead for the life of the screen would leave the user
      // no way to ask again. #25's idempotency rule is about `kDuplicate`,
      // which lands as `kQueued` rather than here.
      if (row.state != RowState::kReady && row.state != RowState::kRefused) {
        // Idempotent from the screen's side (#25): the row already says what
        // this press would achieve, and sending the command to hear the same
        // answer back is a round trip whose only effect is to make the sentence
        // arrive a frame later. Two of the four reasons are not even errors the
        // sysmodule would answer with.
        return;
      }
      enqueue_wanted_ = row.rom_id;
      return;

    case RowKind::kQueueEntry:
    case RowKind::kMore:
      return;
  }
}

bool LibraryBrowserModel::Back() {
  if (stack_.size() <= 1) {
    return false;
  }
  if (top().cursor != 0) {
    closing_.push_back(top().cursor);
  }
  stack_.pop_back();
  // The level underneath kept its rows, its cursor and its selection while this
  // one was open, which is the whole reason the stack holds levels rather than
  // a level: re-opening the platform list to get back to where the user was
  // would re-page the library they just walked down.
  return true;
}

void LibraryBrowserModel::OpenQueue() {
  if (top().kind == LibraryLevel::kQueue) {
    // Already here. Without this, holding the button pushes a queue level per
    // press and each one opens a cursor -- #31 caps how many may be open, so an
    // unbounded stack is a browser that runs the sysmodule out of them.
    return;
  }
  Level queue;
  queue.kind = LibraryLevel::kQueue;
  queue.request.kind = ipc::ListKind::kQueue;
  queue.title = "Downloads";
  stack_.push_back(std::move(queue));
}

void LibraryBrowserModel::Close() {
  closed_ = true;
  for (Level& level : stack_) {
    if (level.cursor != 0) {
      closing_.push_back(level.cursor);
      level.cursor = 0;
    }
  }
  enqueue_wanted_ = 0;
}

// --- reading the projections --------------------------------------------------

bool LibraryBrowserModel::ReadPlatform(const ipc::ListItem& item, LibraryRow* row) {
  std::int64_t id = 0;
  std::int64_t rom_count = 0;
  std::string name;
  std::string fs_slug;
  bool mapped = false;
  if (!ReadInteger(item, keys::kPlatformId, &id) ||
      !ReadText(item, keys::kPlatformName, &name) ||
      !ReadText(item, keys::kPlatformFsSlug, &fs_slug) ||
      !ReadInteger(item, keys::kPlatformRomCount, &rom_count) ||
      !ReadFlag(item, keys::kPlatformMapped, &mapped)) {
    return false;
  }

  row->kind = RowKind::kPlatform;
  // The slug when RomM has no display name for it: a row labelled with an empty
  // string is the one thing a view model here may never produce.
  row->label = !name.empty() ? name : (!fs_slug.empty() ? fs_slug : std::string(kUnnamed));
  row->value = RomCountText(rom_count);
  row->platform_id = id;
  row->fs_slug = fs_slug;
  row->rom_id = 0;
  row->selectable = true;
  // A platform with no folder is still walked into -- the roms are worth
  // reading, and the reason is on every row inside. It is `kUnmapped` rather
  // than `kInert` so the reason is on the platform row too.
  row->state = mapped ? RowState::kInert : RowState::kUnmapped;
  row->note = mapped ? std::string() : RowStateText(RowState::kUnmapped, fs_slug);
  row->tone = mapped ? Tone::kNeutral : RowStateTone(RowState::kUnmapped);
  return true;
}

bool LibraryBrowserModel::ReadRom(const ipc::ListItem& item, bool platform_mapped,
                                  const std::string& platform_fs_slug, LibraryRow* row) {
  std::int64_t rom_id = 0;
  std::int64_t size_bytes = 0;
  std::string name;
  std::string fs_name;
  std::string fs_slug;
  bool has_multiple_files = false;
  bool on_disk = false;
  bool queued = false;
  if (!ReadInteger(item, keys::kRomId, &rom_id) || !ReadText(item, keys::kRomName, &name) ||
      !ReadText(item, keys::kRomFsName, &fs_name) ||
      !ReadText(item, keys::kRomPlatformFsSlug, &fs_slug) ||
      !ReadInteger(item, keys::kRomSizeBytes, &size_bytes) ||
      !ReadFlag(item, keys::kRomHasMultipleFiles, &has_multiple_files) ||
      !ReadFlag(item, keys::kRomOnDisk, &on_disk) || !ReadFlag(item, keys::kRomQueued, &queued)) {
    return false;
  }

  row->kind = RowKind::kRom;
  // The name off the projection, never derived from `fs_name`: for a nested
  // single-file rom `fs_name` is the *directory's* name and carries no
  // extension (#21), and #92 may change what the file on the card is called.
  row->label = !name.empty() ? name : (!fs_name.empty() ? fs_name : std::string(kUnnamed));
  // Zero is a real answer from a server that declared no length (#22), and an
  // empty right-hand column says that better than "0 B" does.
  row->value = size_bytes > 0 ? FormatBytes(size_bytes) : std::string();
  row->rom_id = rom_id;
  // The level's slug when the projection carried none. The unmapped sentence
  // names the slug the user has to add a line for, and a rom row that lost it
  // would say "this platform" to somebody looking at a list of them.
  row->fs_slug = !fs_slug.empty() ? fs_slug : platform_fs_slug;
  row->platform_id = 0;
  row->selectable = true;
  row->state = PredictEnqueue({.platform_mapped = platform_mapped,
                               .has_multiple_files = has_multiple_files,
                               .on_disk = on_disk,
                               .queued = queued});
  row->note = RowStateText(row->state, row->fs_slug);
  row->tone = RowStateTone(row->state);
  return true;
}

bool LibraryBrowserModel::ReadQueueEntry(const ipc::ListItem& item, LibraryRow* row) {
  std::int64_t rom_id = 0;
  std::int64_t bytes_done = 0;
  std::int64_t size_bytes = 0;
  std::int64_t bytes_per_second = 0;
  std::int64_t attempts = 0;
  std::string fs_name;
  std::string fs_slug;
  std::string state;
  std::string message;
  if (!ReadInteger(item, keys::kQueueRomId, &rom_id) ||
      !ReadText(item, keys::kQueueFsName, &fs_name) ||
      !ReadText(item, keys::kQueuePlatformFsSlug, &fs_slug) ||
      !ReadText(item, keys::kQueueState, &state) ||
      !ReadInteger(item, keys::kQueueBytesDone, &bytes_done) ||
      !ReadInteger(item, keys::kQueueSizeBytes, &size_bytes) ||
      !ReadInteger(item, keys::kQueueBytesPerSecond, &bytes_per_second) ||
      !ReadInteger(item, keys::kQueueAttempts, &attempts) ||
      !ReadText(item, keys::kQueueMessage, &message)) {
    return false;
  }

  row->kind = RowKind::kQueueEntry;
  // The file, and the platform it is going to when there is no file name yet --
  // `Enqueue` records a rom id and nothing else, and the worker fills the rest
  // in (`download.hpp`), so a freshly queued row has an id and a slug.
  row->label = !fs_name.empty()
                   ? fs_name
                   : "Rom " + std::to_string(rom_id) +
                         (fs_slug.empty() ? std::string() : " on " + fs_slug);
  row->value = QueueProgressText(state, bytes_done, size_bytes, bytes_per_second);
  row->note = QueueNoteText(message, attempts);
  row->tone = row->note.empty() ? Tone::kNeutral : Tone::kWarn;
  row->rom_id = rom_id;
  row->fs_slug = fs_slug;
  row->platform_id = 0;
  // Read-only in v1: there is no `Dequeue` on this screen, so a queue row is
  // drawn and scrolled past rather than pressed.
  row->selectable = false;
  row->state = RowState::kInert;
  return true;
}

// --- rendering ----------------------------------------------------------------

LibraryView LibraryBrowserModel::Render() const {
  LibraryView view;
  view.link = link_;
  if (link_ != Link::kOk) {
    // The same three sentences the other screens use, because it is the same
    // diagnosis (`overlay_status_view.hpp`); the library has nothing of its own
    // to add about a sysmodule that is not there.
    const StatusView status = RenderUnreachable(link_, sysmodule_interface_);
    view.title = "Library";
    view.headline = status.headline;
    view.hint = status.hint;
    view.tone = status.tone;
    return view;
  }

  const Level& level = top();
  view.level = level.kind;
  view.title = level.title.empty() ? std::string(kUnnamed) : level.title;
  view.rows = level.rows;
  view.can_go_back = stack_.size() > 1;

  const bool has_more_row = level.has_more || level.loading ||
                            level.page_error != ipc::Error::kOk || level.truncated;
  if (has_more_row) {
    LibraryRow more;
    more.kind = RowKind::kMore;
    more.state = RowState::kInert;
    more.rom_id = 0;
    if (level.page_error != ipc::Error::kOk) {
      more.label = level.cursor == 0 ? "This list could not be opened"
                                     : "The next page could not be loaded";
      more.value = ipc::ToString(level.page_error);
      more.note = "Press A to try again";
      more.tone = Tone::kWarn;
      more.selectable = true;
    } else if (level.truncated) {
      // Cut, not finished. Two different sentences, because a user told a list
      // ended stops looking for what is not in it (`kMaxLoadedRows`).
      more.label = "Too many to list here";
      more.value = std::to_string(level.rows.size()) + " shown";
      more.note = "Narrowing the library is not in this version";
      more.tone = Tone::kWarn;
      more.selectable = false;
    } else {
      more.label = "Loading...";
      more.tone = Tone::kNeutral;
      // Nothing to press while a page is on its way. `MoveSelection` still lets
      // the selection rest here, which is what asks for the page after it.
      more.selectable = false;
    }
    view.rows.push_back(std::move(more));
  }

  view.selected = view.rows.empty()
                      ? -1
                      : std::clamp(level.selected, 0, static_cast<int>(view.rows.size()) - 1);

  // The headline. An empty level is a sentence rather than a blank screen, and
  // a level with **nothing loaded under the failure** is the one case where the
  // failure is the whole screen. The test is the loaded rows rather than the
  // cursor: a first `ListNext` that fails on an open cursor has no rows either,
  // and reading the cursor instead would leave the headline saying "Loading..."
  // over a row saying it had stopped.
  if (level.page_error != ipc::Error::kOk && level.rows.empty()) {
    view.headline = "This list could not be loaded";
    view.hint = "Press A to try again";
    view.tone = Tone::kWarn;
    return view;
  }
  if (level.unreadable_items > 0) {
    // Ahead of the empty-list sentence on purpose: a page whose every item
    // failed to decode leaves no rows, and "Nothing in the download queue" over
    // a running download is the worst way to report that the two halves
    // disagree about a field. It is `kIncompatible`'s diagnosis arriving one
    // level down, so it reads like it.
    view.headline = std::to_string(level.unreadable_items) +
                    (level.unreadable_items == 1 ? " row this overlay cannot read"
                                                 : " rows this overlay cannot read");
    view.hint = "Update the overlay: the sysmodule is sending a field it does not know";
    view.tone = Tone::kBad;
    return view;
  }
  if (level.rows.empty()) {
    if (level.loading || level.has_more) {
      view.headline = "Loading...";
      view.tone = Tone::kNeutral;
      return view;
    }
    switch (level.kind) {
      case LibraryLevel::kPlatforms:
        view.headline = "No platforms";
        view.hint = "The server's library is empty";
        break;
      case LibraryLevel::kRoms:
        view.headline = "No roms on this platform";
        break;
      case LibraryLevel::kQueue:
        view.headline = "Nothing in the download queue";
        view.hint = "Pick a rom from the library to download it";
        break;
    }
    view.tone = Tone::kNeutral;
    return view;
  }

  switch (level.kind) {
    case LibraryLevel::kPlatforms:
      view.headline = "Pick a platform";
      break;
    case LibraryLevel::kRoms:
      // The unmapped sentence is `RowStateText`'s rather than a second copy of
      // it: the level says it once at the top and every row inside says it
      // again, and two wordings for one problem read as two problems.
      view.headline = level.platform_mapped
                          ? "Pick a rom to download"
                          : RowStateText(RowState::kUnmapped, level.platform_fs_slug);
      view.hint = level.platform_mapped
                      ? std::string()
                      : "Nothing here can be downloaded until it has one";
      break;
    case LibraryLevel::kQueue:
      // Not "Downloads" a second time: the title already says that, and a
      // headline repeating it is a line a user learns to stop reading.
      view.headline = "What this console is downloading";
      break;
  }
  view.tone = level.kind == LibraryLevel::kRoms && !level.platform_mapped ? Tone::kWarn
                                                                         : Tone::kNeutral;
  return view;
}

}  // namespace rommsync::overlay

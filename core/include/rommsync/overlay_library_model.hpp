// The overlay's library browser -- platforms, then roms, then the download
// queue -- decided here and drawn over there.
//
// The same split `overlay_status_view.hpp`, `overlay_pairing_view.hpp` and
// `overlay_sync_actions.hpp` have, and for the same reason: what a screen
// *says* is a decision, and a decision put inside a `tsl::Gui` is untestable
// until someone has a console. `overlay/source/library_screen.cpp` turns a
// `LibraryView` into libultrahand elements and sends what `Next()` asks for;
// everything that could be wrong about the browser is `ctest -R overlay.library`.
//
// What is different from the other three is that this one has **state**. The
// other screens are a pure function of one `GetStatus`; a browser is a cursor
// stack, a set of loaded pages, a selection and a per-row answer to a press,
// and it is spread across as many IPC round trips as the user scrolls. So this
// is a small state machine rather than a `Render(status)` -- and it is a state
// machine on this side of the boundary precisely so a host test can drive it
// through the paging cases a console would take a library to reproduce.
//
// ## The loop
//
// The model never calls anything. It answers `Next()` with the one command the
// screen should send this frame, and is told what came back:
//
//   Command command = model.Next();     // kNone: nothing to do this frame
//   ... send it ...
//   model.OnCursor(cursor);             // ListBegin answered
//   model.OnPage(page);                 // ListNext answered
//   model.OnEnded();                    // ListEnd answered
//   model.OnEnqueued(position);         // Enqueue answered
//   model.OnRefused(error);             // the sysmodule refused it
//   model.OnUnreachable(link);          // the transport, or the handshake
//
// One command per frame, and one in flight at a time: the overlay draws on one
// thread and a `cmif` call from it is synchronous, so there is no second
// command to race. That is also what lets `OnRefused` take an error and nothing
// else -- the model remembers what it handed out.
//
// ## What it never does
//
// **The overlay never calls RomM** (docs/ARCHITECTURE.md). Every row here came
// off a `ListNext`, so the fault-proxy scenarios belong to the sysmodule's
// issues; this model's job is to render a page that failed *as a page that
// failed*, and keep the pages already loaded.
//
// **It never opens `config.ini`.** Whether a platform has a folder is
// `mapped` on the platform row, decided by the sysmodule, which is the half
// that owns the file.
//
// Hard rule 4 applies as it does to the rest of `core/`: no libnx header, no
// `Result`, no libultrahand type. What crosses is strings and enums, so the
// renderer picks the colour for a `Tone` and this file never names one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rommsync/ipc.hpp"
#include "rommsync/overlay_status_view.hpp"

namespace rommsync::overlay {

/// Which of the three lists is on screen. They are levels of one screen rather
/// than three screens: `Back()` walks them, and the level underneath keeps its
/// rows, its cursor and its selection while the one above is open.
enum class LibraryLevel { kPlatforms, kRoms, kQueue };
const char* ToString(LibraryLevel level);

/// What a row is, which is what pressing A on it does.
enum class RowKind {
  kPlatform,    ///< descend into its roms
  kRom,         ///< enqueue it, unless `RowState` says it would not take
  kQueueEntry,  ///< nothing; the queue is read-only in v1

  /// The end of the loaded rows, and what is happening there: a page in
  /// flight, or a page that failed and can be asked for again. It is a row
  /// rather than a banner because that is what puts the failure **on the page
  /// that failed** instead of over the whole screen -- the rows above it are
  /// still the library, and a user can still scroll and enqueue from them.
  kMore,
};
const char* ToString(RowKind kind);

/// Whether a row would download, and why not.
///
/// Four of these are decided *before* a press, off the row's own projection,
/// and the screen sends nothing for them -- the same shape `PredictSyncNow` has
/// (`overlay_sync_actions.hpp`). Two of them are not `ipc::Error`s at all:
/// `Enqueue` accepts an unmapped platform and a rom that is already on the card
/// and the worker settles them `kSkipped`/`kDone` (`download.hpp`), so a screen
/// that did not predict them would let a user press download and watch nothing
/// happen. #25 is explicit that both are drawn as *skipped, with the reason*,
/// never hidden.
enum class RowState {
  /// A press sends `Enqueue`.
  kReady,

  /// It is in the download queue. From the projection's `queued`, or from an
  /// `Enqueue` that answered `kOk` or `kDuplicate` -- the two are the same fact
  /// about the console and are deliberately the same row state.
  kQueued,

  /// The card already holds it. Distinct from `kQueued`: one says the rom is
  /// there, the other says it is on its way, and #25 requires them to be
  /// tellable apart.
  kOnDisk,

  /// Its platform has no folder in `config.ini`, so the worker would skip it.
  kUnmapped,

  /// A disc set. Out of scope for v1 (M3-4, #21), and refused at the door by
  /// `EnqueueRom` rather than queued and skipped.
  kMultiFile,

  /// The sysmodule refused the press for some other reason; `note` is which.
  kRefused,

  /// Nothing to press: a platform row, a queue row, the `kMore` row.
  kInert,
};
const char* ToString(RowState state);

/// One drawn row.
struct LibraryRow {
  RowKind kind = RowKind::kRom;
  RowState state = RowState::kInert;

  /// The left-hand text. **Never empty, in any state** -- the guarantee
  /// `overlay_status_view.hpp` sets out, for the reason it sets it out: a row
  /// drawn as an empty string is indistinguishable from an overlay that failed
  /// to read something.
  std::string label;

  /// The right-hand text: a size, a rom count, a queue entry's progress. May be
  /// empty, which is a row that has nothing to say on the right rather than a
  /// value that went missing.
  std::string value;

  /// The second line: why this row will not download, what the last press did,
  /// or what a row that is not a rom has to say for itself -- a queue entry's
  /// message, a failed page's "press A to try again".
  ///
  /// It is **not** a proxy for "this cannot be downloaded": a `kInert` queue row
  /// carries one, and a `kReady` rom carries none. Read `state` for that.
  /// Non-empty for every `state` that is not `kReady` or `kInert`, which is the
  /// half of the guarantee that holds: a row that will not download always says
  /// why.
  std::string note;

  Tone tone = Tone::kNeutral;

  /// Whether A does anything here. False for a queue row and for a `kMore` row
  /// with a page still in flight.
  bool selectable = true;

  /// `kRom` and `kQueueEntry`. Zero on the others.
  std::int64_t rom_id = 0;

  /// `kPlatform`. The id `ipc::ListRequest::platform_id` takes, and the slug
  /// the folder map is keyed by -- both, because the id is what opens the list
  /// and the slug is what a user reads in `config.ini` when the platform is
  /// unmapped.
  std::int64_t platform_id = 0;
  std::string fs_slug;
};

/// Everything the library screen draws, and nothing about how.
struct LibraryView {
  Link link = Link::kOk;
  LibraryLevel level = LibraryLevel::kPlatforms;

  /// What this level is called -- "Library", the platform's name, "Downloads".
  /// Never empty.
  std::string title;

  /// The one sentence under it. Never empty, in any state, including a level
  /// that is simply empty: "No platforms" is a fact, and a blank screen is not.
  std::string headline;

  /// What the user can do about it, or empty when there is nothing to do.
  std::string hint;

  Tone tone = Tone::kNeutral;

  std::vector<LibraryRow> rows;

  /// Into `rows`, and `-1` exactly when `rows` is empty.
  int selected = -1;

  /// `Back()` would move to the level underneath rather than close the screen.
  /// The screen draws the B prompt from it.
  bool can_go_back = false;
};

/// The browser's whole state: the cursor stack, the loaded pages, the selection
/// and the per-row answer to a press.
///
/// One instance per open screen. Constructing it opens nothing; the first
/// `Next()` asks for the platform list.
class LibraryBrowserModel {
 public:
  /// The one command the screen should send this frame.
  struct Command {
    enum class Kind { kNone, kListBegin, kListNext, kListEnd, kEnqueue };

    Kind kind = Kind::kNone;

    /// `kListBegin`.
    ipc::ListRequest request;

    /// `kListNext` and `kListEnd`.
    ipc::Cursor cursor = 0;

    /// `kEnqueue`.
    std::int64_t rom_id = 0;
  };

  LibraryBrowserModel();

  /// What to send now, and what the next `On...` is understood to be answering.
  ///
  /// `kNone` means there is nothing to do this frame -- everything loaded, or a
  /// page that failed and is waiting to be asked for again, or a link that is
  /// not `kOk`. Calling it again without answering the last one returns the
  /// same command: the model hands out one at a time, and a caller that dropped
  /// one is a caller that will be handed it again rather than one whose press
  /// went missing.
  ///
  /// Each `On...` below answers exactly the kind that was handed out and is
  /// ignored otherwise. The screen sends synchronously, so nothing arrives out
  /// of order today; the guard is what keeps a page from being answered into a
  /// level it did not come from if anything ever does.
  Command Next();

  /// `ListBegin` answered `cursor`.
  void OnCursor(ipc::Cursor cursor);

  /// `ListNext` answered `page`.
  ///
  /// A `pending` page is not an empty one (#31): nothing is appended, nothing
  /// is concluded about the end of the list, and the next `Next()` asks again.
  void OnPage(const ipc::ListPage& page);

  /// `ListEnd` answered. Nothing to record -- a closed cursor is one the model
  /// has already forgotten.
  void OnEnded();

  /// `Enqueue` answered `kOk`. `position` is 1-based among the entries the
  /// worker still has to do.
  void OnEnqueued(std::int32_t position);

  /// The sysmodule refused the command `Next()` handed out.
  ///
  /// `kBadCursor` on a `ListNext` is not a failure: it is a cursor the
  /// sysmodule reclaimed, which #31 says to expect and re-open rather than
  /// report. The level is re-opened from the start and its rows are replaced
  /// when the first fresh page arrives -- they stay on screen until then, so a
  /// reclaimed cursor is not a screen that blinks empty.
  void OnRefused(ipc::Error error);

  /// The command did not reach the sysmodule, or its answer could not be read.
  ///
  /// `link` must not be `kOk`; it is `ScreenFrame::Diagnose`'s verdict.
  /// `sysmodule_interface` is what `GetInterfaceVersion` answered, and matters
  /// only for `kIncompatible`, where the two version numbers are the whole
  /// diagnosis (`overlay_status_view.hpp`).
  void OnUnreachable(Link link, std::uint32_t sysmodule_interface = 0);

  /// The link came back. Everything loaded is dropped and the browser starts
  /// again at the platform list: the cursors belonged to a session that is
  /// gone, and a page from before it is not a page this one would answer with.
  void OnLinkRestored();

  /// Move the selection by `delta` rows, clamped at both ends. Also what asks
  /// for the next page: the model fetches ahead of the selection rather than on
  /// a scroll event, so a screen that draws more rows at once still works.
  void MoveSelection(int delta);

  /// A. Descend into a platform, enqueue a rom, or retry a page that failed.
  ///
  /// A press on a row whose `RowState` is not `kReady` sends nothing and leaves
  /// the reason already on the row. That is `Enqueue` being idempotent from the
  /// screen's side (#25): once a row says `queued`, the button stops sending.
  void Activate();

  /// B. Leave this level for the one underneath.
  ///
  /// False when there is none, which is the screen telling Tesla to pop it.
  /// The level being left has its cursor closed, so an abandoned cursor is the
  /// exception here rather than the rule (#31).
  bool Back();

  /// Open the download queue as a level above this one, unless it is already
  /// the level on top -- a held button would otherwise push one per frame, and
  /// each level opens a cursor out of the small number #31 allows.
  void OpenQueue();

  /// The screen is going away. Every open cursor is queued for `ListEnd`, so
  /// the caller should keep pumping `Next()` until it answers `kNone`.
  void Close();

  LibraryView Render() const;

  /// How many cursors this model believes are open, `Close()`'s pending ones
  /// included. Published for the test rather than for a screen: a browser that
  /// leaks one per platform the user opens would eat #31's cursor cap.
  std::size_t open_cursors() const;

 private:
  /// One level of the stack: a list, as far as it has been loaded.
  struct Level {
    LibraryLevel kind = LibraryLevel::kPlatforms;

    /// How it was opened, kept so a reclaimed cursor can be re-opened with the
    /// same filter rather than with a guess.
    ipc::ListRequest request;

    /// Zero before `ListBegin` answers, and after a `kBadCursor` retires one.
    ipc::Cursor cursor = 0;

    std::vector<LibraryRow> rows;
    int selected = 0;

    /// Another page would follow. True until a page says otherwise, so a level
    /// that has loaded nothing yet still asks for its first page.
    bool has_more = true;

    /// A page is in flight, or the last one came back `pending`.
    bool loading = false;

    /// The next page to arrive replaces the loaded rows rather than extending
    /// them. Set when a reclaimed cursor is re-opened: the fresh cursor starts
    /// at the beginning of the list.
    bool replace_rows = false;

    /// What went wrong with the last page, and nothing about the ones before
    /// it. `kOk` when nothing did.
    ipc::Error page_error = ipc::Error::kOk;

    /// The level stopped at `kMaxLoadedRows` with more still on the server.
    /// Not the same as reaching the end, and drawn as a different sentence.
    bool truncated = false;

    /// Items on the pages loaded so far that this build could not read. Kept
    /// and rendered rather than only dropped: a page whose items all failed to
    /// decode is otherwise indistinguishable from an empty list, and "Nothing
    /// in the download queue" over a running download is the worst way to say
    /// "the two halves disagree about a field".
    int unreadable_items = 0;

    /// Consecutive pages that came back empty with `has_more` still set. A
    /// producer that answered that forever would have the overlay asking
    /// forever, at a page per frame; see `kMaxEmptyPages`.
    int empty_pages = 0;

    /// `kRoms`: what the platform row said, so a rom row can be greyed with the
    /// reason before a press.
    bool platform_mapped = true;
    std::string platform_fs_slug;

    /// The level's name, from the row that opened it.
    std::string title;
  };

  Level& top();
  const Level& top() const;

  /// Fill in a row from one `ipc::ListItem`, or answer false for an item this
  /// build cannot read. A malformed item is dropped rather than rendered
  /// half-filled, for `ipc::Decoded`'s reason: three defaulted fields render as
  /// a library with odd numbers in it.
  static bool ReadPlatform(const ipc::ListItem& item, LibraryRow* row);
  static bool ReadRom(const ipc::ListItem& item, bool platform_mapped,
                      const std::string& platform_fs_slug, LibraryRow* row);
  static bool ReadQueueEntry(const ipc::ListItem& item, LibraryRow* row);

  /// Everything loaded, dropped. The next `Next()` starts at the platform list.
  void Reset();

  std::vector<Level> stack_;

  /// Cursors waiting for a `ListEnd`: a level that has been left, and every
  /// open one after `Close()`.
  std::vector<ipc::Cursor> closing_;

  /// The rom the last `kEnqueue` was for, so its answer lands on the right row
  /// even if the selection moved while the call was out.
  std::int64_t enqueueing_ = 0;

  /// A press that is waiting to be sent. Zero when there is none.
  std::int64_t enqueue_wanted_ = 0;

  /// What `Next()` last handed out, so `OnRefused` knows what was refused.
  Command::Kind issued_ = Command::Kind::kNone;

  /// How deep the stack was when it was handed out. A page is answered into the
  /// level it was asked for or not at all: `top()` moves under `Back()`,
  /// `Activate()` and `OpenQueue()`, and a rom page decoded as a platform page
  /// drops every item and starts a level counting toward `kMaxEmptyPages`.
  std::size_t issued_depth_ = 0;

  /// `Close()` has been called. The browser asks for nothing further -- a
  /// screen on its way out that re-opened the list it was closing would hold a
  /// cursor nobody will ever end.
  bool closed_ = false;

  Link link_ = Link::kOk;

  /// What `GetInterfaceVersion` last answered, for `kIncompatible`'s hint.
  std::uint32_t sysmodule_interface_ = 0;
};

/// How far ahead of the selection the next page is asked for.
///
/// A page is fetched when the selection comes within this many rows of the end
/// of the loaded ones, rather than when it reaches them: a `ListNext` is a
/// round trip and possibly a fetch from RomM, and a user holding down on the
/// stick should not stop at the bottom of every page.
inline constexpr int kPrefetchRows = 8;

/// How many rows one level may hold before it stops asking for more.
///
/// The one structure here that would otherwise grow with the size of the
/// library, which is the thing `ipc.hpp` bounds everything else against: a
/// `LibraryRow` is four strings, and a user scrolling a ten-thousand-rom
/// platform would accumulate one per rom on an overlay's heap. Eight full pages
/// is more than a person scrolls past looking for a game, and the row at the
/// end says the list was cut rather than pretending it ended.
///
/// Narrowing the list instead of scrolling it is `ListRequest::search`, which
/// no screen sends yet -- there is no keyboard in v1 (#25). Recorded there.
inline constexpr int kMaxLoadedRows = 8 * ipc::kMaxPageSize;

/// How many consecutive empty pages with `has_more` still set are tolerated
/// before the list is treated as ended.
///
/// Not a contract rule -- it is a bound on a *producer bug*. `ListNext` costs a
/// round trip per frame, so a sysmodule answering `{items:[], has_more:true}`
/// forever would have the overlay asking sixty times a second with nothing to
/// draw for it. The list ends with the reason on the `kMore` row instead.
inline constexpr int kMaxEmptyPages = 4;

/// The sentence a refused `Enqueue` gets, and how it should read.
///
/// Published for `SyncOutcomeText`'s reason: a refusal the screen predicted and
/// one the sysmodule answered with must be the same words, or the same console
/// says two different things about the same rom depending on which came first.
std::string EnqueueRefusalText(ipc::Error error);

/// What a refused `Enqueue` leaves the row in.
RowState EnqueueRefusalState(ipc::Error error);

/// Everything about a rom that decides whether pressing A would achieve
/// anything.
///
/// A struct rather than four `bool` parameters: four same-typed arguments at a
/// call site are transposable and nothing catches it, and a designated
/// initialiser reads as the sentence the projection already is.
struct RomFacts {
  /// Its platform has a folder in `config.ini`, as the *sysmodule* reads it.
  bool platform_mapped = true;
  bool has_multiple_files = false;
  bool on_disk = false;
  bool queued = false;
};

/// The state a rom row is in before anything is pressed.
///
/// The order is a skip reason first, then where the rom already is: a disc set
/// and an unmapped platform are the reasons it will *never* arrive, which is
/// what a user needs before "it is already queued".
RowState PredictEnqueue(const RomFacts& facts);

/// The sentence for a `RowState` that is not `kReady`, and its tone. Empty for
/// `kReady` and `kInert`, which have nothing to explain.
std::string RowStateText(RowState state, std::string_view fs_slug);
Tone RowStateTone(RowState state);

}  // namespace rommsync::overlay

#include "library_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ipc_client.hpp"
#include "library_pump.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_library_model.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us. Named
// rather than sprinkled through `Draw` for the same reason the other screens
// name their own: a layout is the one thing here that will be adjusted against
// a real panel in M8-2 (#44), and a person doing that should have one place to
// look.
constexpr s32 kHeadlineFont = 23;
constexpr s32 kBodyFont = 19;
constexpr s32 kNoteFont = 16;
constexpr s32 kTitleHeight = 30;
constexpr s32 kHeadlineHeight = 30;
constexpr s32 kRowHeight = 26;
constexpr s32 kNoteHeight = 20;
/// Where a row's right-hand value starts, measured from the drawer's right edge
/// rather than from its left: a size is short and a rom's name is not, so the
/// column that has to fit is the one on the right.
constexpr s32 kValueColumn = 150;
/// How far short of the drawer's right edge a line stops.
constexpr s32 kInset = 8;
/// How far a row's label and note are indented from the selection marker.
constexpr s32 kRowIndent = 18;

/// Whether the queue prompt is offered on this view.
bool ShowsQueuePrompt(const LibraryView& view) {
  return view.link == Link::kOk && view.level != LibraryLevel::kQueue;
}

/// What A does on the row under the selection, or nullptr when it does nothing.
///
/// The prompt is drawn from the row rather than from the level, because within
/// one rom list some rows download and some are already on the card -- a
/// standing "Download" prompt over a greyed row is a button that lies (#24's
/// rule, applied per row).
const char* ActionFor(const LibraryRow& row) {
  switch (row.kind) {
    case RowKind::kPlatform:
      return "Open";
    case RowKind::kRom:
      return row.state == RowState::kReady ? "Download" : nullptr;
    case RowKind::kMore:
      return row.selectable ? "Try again" : nullptr;
    case RowKind::kQueueEntry:
      break;
  }
  return nullptr;
}

}  // namespace

LibraryScreen::LibraryScreen(IpcClient& client) : client_(client) {
  view_ = model_.Render();
}

LibraryScreen::~LibraryScreen() { CloseLibrary(frame_, client_, model_); }

tsl::elm::Element* LibraryScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer rather than a `List` of `ListItem`s, for the reason the
  // other three screens use one and then some: the rows change under the
  // selection as pages arrive, and a `List` would have to be torn down and
  // rebuilt every time one did.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void LibraryScreen::update() { Pump(); }

bool LibraryScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                                HidAnalogStickState) {
  if (view_.link != Link::kOk) {
    // Nothing to browse and nothing to press. Not handled, so B still leaves
    // the screen and Tesla keeps its own bindings -- and the headline is
    // already saying why, so there is nothing to add.
    return false;
  }

  if ((keys_down & HidNpadButton_AnyDown) != 0) {
    model_.MoveSelection(1);
    view_ = model_.Render();
    return true;
  }
  if ((keys_down & HidNpadButton_AnyUp) != 0) {
    model_.MoveSelection(-1);
    view_ = model_.Render();
    return true;
  }
  if ((keys_down & HidNpadButton_A) != 0) {
    // A press on a row the model already knows would be refused sends nothing
    // and leaves the reason where it already is, on the row. Still *handled*:
    // the press was on a row this screen owns, and passing it on would give it
    // to Tesla.
    model_.Activate();
    view_ = model_.Render();
    return true;
  }
  if ((keys_down & HidNpadButton_B) != 0) {
    if (!model_.Back()) {
      // The bottom of the stack. Not handled, so Tesla's own B closes the
      // overlay rather than this screen inventing a second way out.
      return false;
    }
    view_ = model_.Render();
    return true;
  }
  if ((keys_down & HidNpadButton_Y) != 0) {
    model_.OpenQueue();
    view_ = model_.Render();
    return true;
  }
  return false;
}

void LibraryScreen::Pump() {
  PumpLibrary(frame_, client_, model_, link_);
  view_ = model_.Render();
}

s32 LibraryScreen::PromptRows() const {
  s32 rows = 0;
  if (view_.selected >= 0 &&
      ActionFor(view_.rows[static_cast<std::size_t>(view_.selected)]) != nullptr) {
    ++rows;
  }
  if (ShowsQueuePrompt(view_)) {
    ++rows;
  }
  if (view_.can_go_back) {
    ++rows;
  }
  return rows;
}

void LibraryScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                         s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: a rom's name is the user's data with no length this
  // screen can assume. Every string goes through `DrawBounded`, because a width
  // that came out `0` here means "no room" and `drawString` would read it as
  // "no limit" (`palette.hpp`).
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  const s32 label_width =
      width > kRowIndent + kValueColumn + kInset ? width - kRowIndent - kValueColumn - kInset : 0;
  const s32 value_width = width > kValueColumn + kInset ? kValueColumn - kInset : 0;
  const tsl::Color muted = MutedColor();

  s32 row = y;
  // Once one element has been dropped for want of room, nothing below it is
  // drawn either -- `sync_screen.cpp`'s rule, and for its reason: testing each
  // element on its own would let a short line take the place of a taller one
  // that did not fit.
  bool clipped = false;
  const auto fits = [&](s32 needed) { return !clipped && row + needed <= bottom; };
  const auto line = [&](const std::string& text, s32 font, tsl::Color color, s32 advance) {
    if (text.empty()) {
      return;
    }
    if (!fits(advance)) {
      clipped = true;
      return;
    }
    DrawBounded(renderer, text, x, row, font, color, line_width);
    row += advance;
  };

  line(view_.title, kHeadlineFont, tsl::gfx::Renderer::a(tsl::defaultTextColor), kTitleHeight);
  line(view_.headline, kBodyFont, ColorFor(view_.tone), kHeadlineHeight);
  line(view_.hint, kNoteFont, muted, kNoteHeight);

  // The prompts are reserved *before* the rows, not drawn after them. A list is
  // longer than the panel in the ordinary case, so a row loop that spent the
  // whole remaining height would push "Ⓐ Download" and "Ⓑ Back" off the bottom
  // for every list a user actually scrolls -- a control that is only visible
  // when the list happens to be short is one they never learn is there.
  const s32 prompts = PromptRows() * kRowHeight + kRowHeight / 2;
  const s32 rows_bottom = bottom - prompts;

  // The rows, from the selection rather than from the top: the list is longer
  // than the panel and the selected row is the one that has to be visible. Two
  // rows of context above it, so a scroll does not read as a jump.
  const int rows = static_cast<int>(view_.rows.size());
  int first = 0;
  if (view_.selected >= 0) {
    first = std::max(0, view_.selected - 2);
  }

  row += kRowHeight / 2;
  for (int index = first; index < rows; ++index) {
    const LibraryRow& entry = view_.rows[static_cast<std::size_t>(index)];
    if (clipped || row + kRowHeight > rows_bottom) {
      break;
    }
    const bool selected = index == view_.selected;
    // The marker rather than a filled highlight: `CustomDrawer` has no
    // selection of its own, and a caret is one glyph against a theme this
    // screen does not own.
    if (selected) {
      DrawBounded(renderer, ">", x, row, kBodyFont,
                           tsl::gfx::Renderer::a(tsl::defaultTextColor), kRowIndent);
    }
    // A row that will not download is drawn muted -- present, greyed, with its
    // reason under it. Never hidden: a rom that is silently missing from the
    // list is the worst version of "this cannot be downloaded" (#25).
    const bool live = entry.state == RowState::kReady || entry.state == RowState::kInert;
    const tsl::Color label_color =
        live ? tsl::gfx::Renderer::a(tsl::defaultTextColor) : muted;
    DrawBounded(renderer, entry.label, x + kRowIndent, row, kBodyFont, label_color,
                         label_width);
    if (!entry.value.empty() && value_width > 0) {
      DrawBounded(renderer, entry.value, x + width - kValueColumn, row, kBodyFont, muted,
                           value_width);
    }
    row += kRowHeight;

    // The reason, under the row it is about. Only for the selected row: a list
    // with a sentence under every greyed entry is a list a user cannot scan,
    // and the reason is only actionable for the row they are on.
    if (selected && !entry.note.empty()) {
      if (row + kNoteHeight > rows_bottom) {
        break;
      }
      DrawBounded(renderer, entry.note, x + kRowIndent, row, kNoteFont,
                           ColorFor(entry.tone), label_width);
      row += kNoteHeight;
    }
  }

  // The prompts, in the space held back for them above. A control that does
  // nothing on this row is not drawn at all rather than drawn greyed: unlike
  // the sync screen's two fixed buttons, these change with the selection, and a
  // prompt that appears and disappears as the user scrolls is what tells them
  // which rows are live.
  row = std::max(row, rows_bottom) + kRowHeight / 2;
  if (view_.selected >= 0) {
    const LibraryRow& entry = view_.rows[static_cast<std::size_t>(view_.selected)];
    const char* action = ActionFor(entry);
    if (action != nullptr) {
      line(Prompt(kGlyphA, action), kBodyFont, tsl::gfx::Renderer::a(tsl::defaultTextColor),
           kRowHeight);
    }
  }
  if (ShowsQueuePrompt(view_)) {
    line(Prompt(kGlyphY, "Downloads"), kBodyFont, muted, kRowHeight);
  }
  if (view_.can_go_back) {
    line(Prompt(kGlyphB, "Back"), kBodyFont, muted, kRowHeight);
  }
}

}  // namespace rommsync::overlay

#include "settings_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "conflicts_screen.hpp"
#include "ipc_client.hpp"
#include "library_screen.hpp"
#include "log_screen.hpp"
#include "pairing_screen.hpp"
#include "palette.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "settings_pump.hpp"
#include "sync_screen.hpp"

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
constexpr s32 kHeadlineHeight = 30;
constexpr s32 kHintHeight = 24;
constexpr s32 kSectionHeight = 28;
constexpr s32 kRowHeight = 26;
constexpr s32 kNoteHeight = 20;
/// Where a row's value starts, measured from the drawer's left edge: a key is
/// short and a folder path is not, so the column that has to fit is the right
/// one.
constexpr s32 kValueColumn = 150;
/// How far short of the drawer's right edge a line stops.
constexpr s32 kInset = 8;
/// How far a row is indented from the cursor marker.
constexpr s32 kRowIndent = 18;

}  // namespace

SettingsScreen::SettingsScreen(IpcClient& client) : client_(client), pump_(client) { Rebuild(); }

tsl::elm::Element* SettingsScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer rather than a `List` of `ListItem`s, for the reason the
  // other four screens use one: the rows are rebuilt whenever the sysmodule
  // answers, and a `List` would have to be torn down and rebuilt with them.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void SettingsScreen::update() {
  // The poll and the presses are `SettingsPump`'s, and no frame waits for any
  // of them (#329). The flat list is rebuilt on an answer, not on every frame.
  pump_.Frame();
  if (pump_.TakePairing()) {
    // Re-pair has started a pairing and discarded the old token. The code,
    // the address and the countdown are the pairing screen's (#27).
    tsl::changeTo<PairingScreen>(client_);
    return;
  }
  if (pump_.revision() != rendered_) {
    rendered_ = pump_.revision();
    Rebuild();
  }
}

bool SettingsScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                                 HidAnalogStickState) {
  if (view().link != Link::kOk) {
    // Nothing to scroll and nothing to press. Not handled, so B still leaves
    // the screen and Tesla keeps its own bindings -- and the headline is
    // already saying why, so there is nothing to add.
    return false;
  }

  if ((keys_down & HidNpadButton_AnyDown) != 0 && !entries_.empty()) {
    cursor_ = std::min(cursor_ + 1, static_cast<int>(entries_.size()) - 1);
    return true;
  }
  if ((keys_down & HidNpadButton_AnyUp) != 0 && !entries_.empty()) {
    cursor_ = std::max(cursor_ - 1, 0);
    return true;
  }
  if ((keys_down & HidNpadButton_A) != 0) {
    const SettingsRow* row = CursorRow();
    if (row == nullptr || !row->selectable) {
      // A press on a row that opens nothing. Not handled, so Tesla keeps A for
      // whatever it wants it for -- there is no sentence to draw, because the
      // prompt for A is only drawn over a row that answers it.
      return false;
    }
    if (row->change.has_value()) {
      // Copied out: the answer rebuilds the view `row` points into.
      pump_.PressChange(*row->change);
      return true;
    }
    switch (row->destination) {
      case Destination::kSync:
        tsl::changeTo<SyncScreen>(client_);
        return true;
      case Destination::kLibrary:
        tsl::changeTo<LibraryScreen>(client_);
        return true;
      case Destination::kConflicts:
        // The row is only drawn when `[sync] conflict_show` is on
        // (`overlay_settings_view.hpp`), so there is nothing to check here: a
        // second gate in the renderer would be a second place for the setting
        // to be read.
        tsl::changeTo<ConflictsScreen>(client_);
        return true;
      case Destination::kPairing:
        // The way to the pairing screen that discards nothing: it polls
        // `GetPairState` and shows a code that is already on its way (#27).
        // "Re-pair" is the destructive way in, and it is the button.
        tsl::changeTo<PairingScreen>(client_);
        return true;
      case Destination::kLog:
        tsl::changeTo<LogScreen>(client_);
        return true;
      case Destination::kNone:
        break;
    }
    return false;
  }
  if ((keys_down & HidNpadButton_X) != 0) {
    if (view().repair.state == ControlState::kInert) {
      return false;
    }
    pump_.PressRepair();
    return true;
  }
  return false;
}

void SettingsScreen::Rebuild() {
  const SettingsView& shown = view();
  const int was = cursor_;
  entries_.clear();
  for (std::size_t index = 0; index < shown.complaints.size(); ++index) {
    entries_.push_back(Entry{Entry::Kind::kComplaint, 0, index});
  }
  for (std::size_t section = 0; section < shown.sections.size(); ++section) {
    entries_.push_back(Entry{Entry::Kind::kSectionTitle, section, 0});
    for (std::size_t row = 0; row < shown.sections[section].rows.size(); ++row) {
      entries_.push_back(Entry{Entry::Kind::kRow, section, row});
    }
  }

  if (entries_.empty()) {
    cursor_ = -1;
    return;
  }
  if (was < 0) {
    // Opened on the menu rather than on the top of the list: the complaints are
    // above it and scroll into view with one press of Up, and the rows below it
    // are what a user came to read. A cursor parked on a complaint would put
    // the menu off screen on a file with eight of them.
    cursor_ = 0;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      const SettingsRow* row = entries_[index].kind == Entry::Kind::kRow
                                   ? &shown.sections[entries_[index].section]
                                          .rows[entries_[index].index]
                                   : nullptr;
      if (row != nullptr && row->selectable) {
        cursor_ = static_cast<int>(index);
        break;
      }
    }
    return;
  }
  // A list that shrank under the cursor -- a platform section that went away
  // between two polls -- moves it rather than leaving it past the end.
  cursor_ = std::min(was, static_cast<int>(entries_.size()) - 1);
}

const SettingsRow* SettingsScreen::CursorRow() const {
  const SettingsView& shown = view();
  if (cursor_ < 0 || static_cast<std::size_t>(cursor_) >= entries_.size()) {
    return nullptr;
  }
  const Entry& entry = entries_[static_cast<std::size_t>(cursor_)];
  if (entry.kind != Entry::Kind::kRow) {
    return nullptr;
  }
  return &shown.sections[entry.section].rows[entry.index];
}

s32 SettingsScreen::PromptRows() const {
  const SettingsView& shown = view();
  s32 rows = 0;
  const SettingsRow* row = CursorRow();
  if (row != nullptr && row->selectable) {
    ++rows;
  }
  if (shown.repair.state != ControlState::kInert) {
    ++rows;
    if (!shown.repair.refusal.empty()) {
      ++rows;
    }
  }
  return rows;
}

void SettingsScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                          s32 height) const {
  const SettingsView& shown = view();
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: a folder path is the user's own text with no length this
  // screen can assume. Every string goes through `DrawBounded`, because a width
  // that came out `0` here means "no room" and `drawString` would read it as
  // "no limit" (`palette.hpp`).
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  // Everything drawn at `x + kRowIndent` is bounded from there, not from `x`.
  // `library_screen.cpp` keeps the same two: a title or a note given
  // `line_width` runs `kRowIndent` past the drawer's right edge, and the
  // `[platform.*]` note is long enough to reach it.
  const s32 indented_width = width > kRowIndent + kInset ? width - kRowIndent - kInset : 0;
  const s32 value_width =
      width > kRowIndent + kValueColumn + kInset ? width - kRowIndent - kValueColumn - kInset : 0;
  const tsl::Color muted = MutedColor();
  const tsl::Color plain = tsl::gfx::Renderer::a(tsl::defaultTextColor);

  s32 row = y;
  // Once one element has been dropped for want of room, nothing below it is
  // drawn either -- the rule the other screens keep, and for its reason:
  // testing each element on its own would let a short line take the place of a
  // taller one that did not fit.
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

  line(shown.headline, kHeadlineFont, ColorFor(shown.tone), kHeadlineHeight);
  line(shown.hint, kBodyFont, muted, kHintHeight);
  line(shown.notice, kBodyFont, ColorFor(shown.notice_tone), kHintHeight);

  // The prompts are reserved *before* the list, not drawn after it: the folder
  // map alone is longer than the panel, so a loop that spent the whole
  // remaining height would push "Ⓧ Re-pair" off the bottom of every console
  // that has one (`library_screen.cpp`).
  const s32 prompts = PromptRows() * kRowHeight + kRowHeight / 2;
  const s32 list_bottom = bottom - prompts;

  // The list, drawn from the cursor rather than from the top, with two lines of
  // context above it so a scroll does not read as a jump.
  const int count = static_cast<int>(entries_.size());
  const int first = cursor_ >= 0 ? std::max(0, cursor_ - 2) : 0;

  row += kRowHeight / 2;
  for (int index = first; index < count; ++index) {
    const Entry& entry = entries_[static_cast<std::size_t>(index)];
    const bool selected = index == cursor_;
    // The marker rather than a filled highlight: `CustomDrawer` has no
    // selection of its own, and a caret is one glyph against a theme this
    // screen does not own.
    const auto marker = [&]() {
      if (selected) {
        DrawBounded(renderer, ">", x, row, kBodyFont, plain, kRowIndent);
      }
    };

    if (entry.kind == Entry::Kind::kComplaint) {
      const Line& complaint = shown.complaints[entry.index];
      if (clipped || row + kRowHeight > list_bottom) {
        break;
      }
      marker();
      DrawBounded(renderer, complaint.label, x + kRowIndent, row, kNoteFont,
                           ColorFor(complaint.tone), kValueColumn);
      DrawBounded(renderer, complaint.value, x + kRowIndent + kValueColumn, row, kNoteFont,
                           ColorFor(complaint.tone), value_width);
      row += kRowHeight;
      continue;
    }

    if (entry.kind == Entry::Kind::kSectionTitle) {
      const SettingsSection& section = shown.sections[entry.section];
      if (clipped || row + kSectionHeight > list_bottom) {
        break;
      }
      marker();
      DrawBounded(renderer, section.title, x + kRowIndent, row, kBodyFont, plain,
                           indented_width);
      row += kSectionHeight;
      // A section with no rows is a section with something to say -- a platform
      // switched off, a folder map too large to have been sent -- so the note
      // is drawn whether or not the cursor is on it.
      if (!section.note.empty()) {
        if (row + kNoteHeight > list_bottom) {
          break;
        }
        DrawBounded(renderer, section.note, x + kRowIndent, row, kNoteFont,
                             ColorFor(section.note_tone), indented_width);
        row += kNoteHeight;
      }
      continue;
    }

    const SettingsRow& entry_row = shown.sections[entry.section].rows[entry.index];
    if (clipped || row + kRowHeight > list_bottom) {
      break;
    }
    marker();
    DrawBounded(renderer, entry_row.label, x + kRowIndent, row, kBodyFont, muted,
                         kValueColumn);
    DrawBounded(renderer, entry_row.value, x + kRowIndent + kValueColumn, row, kBodyFont,
                         ColorFor(entry_row.tone), value_width);
    row += kRowHeight;

    // The note under the row it is about, and only for the row the cursor is
    // on: a list with a sentence under every folder is a list nobody can scan,
    // and a `roms` entry's note is only worth reading for the row being read.
    if (selected && !entry_row.note.empty()) {
      if (row + kNoteHeight > list_bottom) {
        break;
      }
      DrawBounded(renderer, entry_row.note, x + kRowIndent, row, kNoteFont,
                           ColorFor(entry_row.tone), indented_width);
      row += kNoteHeight;
    }
  }

  // The prompts, in the space held back for them. A is drawn only over a row
  // that answers it -- which is what tells a user which rows are live -- and
  // the button is drawn greyed with its sentence rather than hidden, because a
  // control that vanishes is one a user goes looking for (#24).
  row = std::max(row, list_bottom) + kRowHeight / 2;
  clipped = false;
  const SettingsRow* cursor_row = CursorRow();
  if (cursor_row != nullptr && cursor_row->selectable) {
    line(Prompt(kGlyphA, cursor_row->press_label), kBodyFont, plain, kRowHeight);
  }
  if (shown.repair.state != ControlState::kInert) {
    line(Prompt(kGlyphX, shown.repair.label), kBodyFont,
         shown.repair.state == ControlState::kLive ? plain : muted, kRowHeight);
    line(shown.repair.refusal, kNoteFont, ColorFor(shown.repair.refusal_tone), kNoteHeight);
  }
}

}  // namespace rommsync::overlay

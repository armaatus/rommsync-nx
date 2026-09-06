#include "conflicts_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "ipc_client.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_conflicts_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {
namespace {

// The screen's geometry, in the coordinate space `CustomDrawer` hands us. Named
// rather than sprinkled through `Draw` for the reason every screen here names
// its own: a layout is the one thing that will be adjusted against a real panel
// in M8-2 (#44), and a person doing that should have one place to look.
constexpr s32 kHeadlineFont = 23;
constexpr s32 kBodyFont = 19;
constexpr s32 kNoteFont = 16;
constexpr s32 kTitleHeight = 30;
constexpr s32 kHeadlineHeight = 30;
constexpr s32 kRowHeight = 26;
constexpr s32 kNoteHeight = 20;
/// How far short of the drawer's right edge a line stops.
constexpr s32 kInset = 8;
/// How far a row's label and note are indented from the selection marker.
constexpr s32 kRowIndent = 18;
/// Where a detail line's value starts. Wider than the library's value column:
/// the values here are digests and paths, not sizes.
constexpr s32 kDetailLabel = 120;

/// What A does on this view, or nullptr when it does nothing.
///
/// Drawn from the view rather than fixed, because within one list some entries
/// can be restored and some cannot -- a standing "Restore" prompt over an entry
/// whose backup is gone is a button that lies.
const char* ActionFor(const ConflictsView& view) {
  if (view.mode == ConflictsMode::kList) {
    return view.rows.empty() ? nullptr : "Open";
  }
  // `can_restore` rather than the mode: whether a restore is on offer is the
  // model's decision (`overlay_conflicts_view.hpp`), and re-deriving it here
  // would be a second place for it to live.
  return view.can_restore ? "Restore" : nullptr;
}

}  // namespace

ConflictsScreen::ConflictsScreen(IpcClient& client) : client_(client) {
  view_ = model_.Render();
}

tsl::elm::Element* ConflictsScreen::createUI() {
  auto* frame = new tsl::elm::OverlayFrame("rommsync", version());
  // A single drawer rather than a `List` of `ListItem`s, for `library_screen`'s
  // reason: the rows change under the selection as pages arrive, and the same
  // drawer has to render three modes.
  frame->setContent(new tsl::elm::CustomDrawer(
      [this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) {
        this->Draw(renderer, x, y, width, height);
      }));
  return frame;
}

void ConflictsScreen::update() { Pump(); }

bool ConflictsScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                                  HidAnalogStickState) {
  if (view_.link != Link::kOk) {
    // Nothing to read and nothing to press. Not handled, so B still leaves the
    // screen and Tesla keeps its own bindings -- and the headline already says
    // why, so there is nothing to add.
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
    // From the list this opens an entry; from an entry it asks for the
    // confirmation; from the confirmation it queues the restore. A press the
    // model already knows would be refused sends nothing and leaves the reason
    // on screen. Still *handled*: the press was on this screen's own control.
    model_.Activate();
    view_ = model_.Render();
    return true;
  }
  if ((keys_down & HidNpadButton_B) != 0) {
    if (!model_.Back()) {
      // Already at the list. Not handled, so the settings screen underneath is
      // popped back to rather than this screen inventing a second way out.
      return false;
    }
    view_ = model_.Render();
    return true;
  }
  return false;
}

void ConflictsScreen::Pump() {
  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    model_.OnUnreachable(link, frame_.sysmodule_interface());
    link_ = link;
    view_ = model_.Render();
    return;
  }
  if (link_ != Link::kOk) {
    link_ = Link::kOk;
    model_.OnLinkRestored();
  }

  const ConflictsModel::Command command = model_.Next();
  if (command.kind != ConflictsModel::Command::Kind::kNone) {
    Send(command);
  }
  view_ = model_.Render();
}

void ConflictsScreen::Send(const ConflictsModel::Command& command) {
  Result rc = 0;
  ipc::ConflictPage page;
  conflicts::RestoreReport report;

  switch (command.kind) {
    case ConflictsModel::Command::Kind::kListConflicts:
      rc = client_.ListConflicts(command.query, &page);
      break;
    case ConflictsModel::Command::Kind::kRestore:
      rc = client_.RestoreBackup(command.entry_id, &report);
      break;
    case ConflictsModel::Command::Kind::kNone:
      return;
  }

  if (R_FAILED(rc)) {
    // A refusal the sysmodule named, or the transport. Telling them apart is
    // the whole reason `DecodeError` exists (`ipc_client.hpp`): without it, a
    // sysmodule that is not running and a payload this build cannot read draw
    // the same sentence over an entry that is perfectly fine.
    ipc::Error error = ipc::Error::kOk;
    if (DecodeError(rc, &error)) {
      model_.OnRefused(error);
      return;
    }
    const Link link = frame_.Diagnose(rc);
    model_.OnUnreachable(link, frame_.sysmodule_interface());
    link_ = link;
    return;
  }

  switch (command.kind) {
    case ConflictsModel::Command::Kind::kListConflicts:
      model_.OnPage(page);
      return;
    case ConflictsModel::Command::Kind::kRestore:
      model_.OnRestored(report);
      return;
    case ConflictsModel::Command::Kind::kNone:
      return;
  }
}

s32 ConflictsScreen::PromptRows() const {
  s32 rows = 0;
  if (ActionFor(view_) != nullptr) {
    ++rows;
  }
  if (view_.can_go_back) {
    ++rows;
  }
  return rows;
}

void ConflictsScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                           s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: `drawString`'s `maxWidth` defaults to "no limit", and a
  // rom's name and an SD path are the user's data with no length this screen can
  // assume.
  const s32 bottom = y + height;
  const s32 line_width = width > kInset ? width - kInset : 0;
  const s32 body_width = width > kRowIndent + kInset ? width - kRowIndent - kInset : 0;
  const s32 value_width =
      width > kDetailLabel + kInset ? width - kDetailLabel - kInset : 0;
  const tsl::Color muted = MutedColor();
  const tsl::Color plain = tsl::gfx::Renderer::a(tsl::defaultTextColor);

  s32 row = y;
  // Once one element has been dropped for want of room, nothing below it is
  // drawn either -- `sync_screen.cpp`'s rule, and its reason: testing each
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
    renderer->drawString(text, false, x, row, font, color, line_width);
    row += advance;
  };

  line(view_.title, kHeadlineFont, plain, kTitleHeight);
  line(view_.headline, kBodyFont, ColorFor(view_.tone), kHeadlineHeight);
  line(view_.hint, kNoteFont, muted, kNoteHeight);

  // The prompts are reserved *before* the body, not drawn after it: a list or a
  // detail is longer than the panel in the ordinary case, and a control that is
  // only visible when the content happens to be short is one a user never
  // learns is there (`library_screen.cpp`).
  const s32 prompts = PromptRows() * kRowHeight + kRowHeight / 2;
  const s32 body_bottom = bottom - prompts;

  row += kRowHeight / 2;

  if (view_.mode == ConflictsMode::kList) {
    // From the selection rather than from the top, with two rows of context
    // above it so a scroll does not read as a jump.
    const int rows = static_cast<int>(view_.rows.size());
    int first = 0;
    if (view_.selected >= 0) {
      first = std::max(0, view_.selected - 2);
    }
    for (int index = first; index < rows; ++index) {
      const ConflictRow& entry = view_.rows[static_cast<std::size_t>(index)];
      if (clipped || row + kRowHeight > body_bottom) {
        break;
      }
      const bool selected = index == view_.selected;
      if (selected) {
        renderer->drawString(">", false, x, row, kBodyFont, plain, kRowIndent);
      }
      // An entry that cannot be restored is drawn muted -- present, greyed,
      // with its reason under it. Never hidden: an overwrite the screen did not
      // list is an overwrite the user cannot find the backup for, which is the
      // failure this screen exists to prevent.
      const bool live = entry.restorable == Restorability::kReady;
      renderer->drawString(entry.label, false, x + kRowIndent, row, kBodyFont,
                           live ? plain : muted, body_width);
      row += kRowHeight;
      if (!entry.value.empty()) {
        if (row + kNoteHeight > body_bottom) {
          break;
        }
        renderer->drawString(entry.value, false, x + kRowIndent, row, kNoteFont, muted,
                             body_width);
        row += kNoteHeight;
      }
      // The reason, under the row it is about, and only for the selected row: a
      // list with a sentence under every entry is a list a user cannot scan.
      if (selected && !entry.note.empty()) {
        if (row + kNoteHeight > body_bottom) {
          break;
        }
        renderer->drawString(entry.note, false, x + kRowIndent, row, kNoteFont,
                             ColorFor(entry.tone), body_width);
        row += kNoteHeight;
      }
    }
  } else {
    // The detail: a label column and a value column, so the two sides of the
    // comparison line up. They are what the user is here to read.
    for (const ConflictDetailLine& detail : view_.detail) {
      if (clipped || row + kNoteHeight > body_bottom) {
        break;
      }
      renderer->drawString(detail.label, false, x, row, kNoteFont, muted, kDetailLabel);
      renderer->drawString(detail.value, false, x + kDetailLabel, row, kNoteFont,
                           ColorFor(detail.tone), value_width);
      row += kNoteHeight;
    }
  }

  // The prompts, in the space held back for them above.
  row = std::max(row, body_bottom) + kRowHeight / 2;
  if (const char* action = ActionFor(view_); action != nullptr) {
    line(Prompt(kGlyphA, action), kBodyFont, plain, kRowHeight);
  }
  if (view_.can_go_back) {
    line(Prompt(kGlyphB, "Back"), kBodyFont, muted, kRowHeight);
  }
}

}  // namespace rommsync::overlay

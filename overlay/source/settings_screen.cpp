#include "settings_screen.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

#include "ipc_client.hpp"
#include "library_screen.hpp"
#include "pairing_screen.hpp"
#include "rommsync/auth.hpp"
#include "rommsync/core.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_settings_view.hpp"
#include "screen_frame.hpp"
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

/// How many frames pass between two `GetConfig`s.
///
/// A configuration changes when somebody edits it, which on this console is
/// never while the overlay is up -- unlike a `Status`, which moves under the
/// status screen every second. What one poll costs is the whole folder map
/// decoded and a few hundred rows rebuilt, so it is asked on a cadence and
/// immediately after anything this screen does. Half a second at 60 Hz.
constexpr int kPollFrames = 30;

}  // namespace

SettingsScreen::SettingsScreen(IpcClient& client) : client_(client) { Rebuild(); }

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
  if (since_poll_ > 0 && since_poll_ < kPollFrames) {
    ++since_poll_;
    return;
  }
  Poll();
}

bool SettingsScreen::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState,
                                 HidAnalogStickState) {
  if (view_.link != Link::kOk) {
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
    switch (row->destination) {
      case Destination::kSync:
        tsl::changeTo<SyncScreen>(client_);
        return true;
      case Destination::kLibrary:
        tsl::changeTo<LibraryScreen>(client_);
        return true;
      case Destination::kPairing:
        // The way to the pairing screen that discards nothing: it polls
        // `GetPairState` and shows a code that is already on its way (#27).
        // "Re-pair" is the destructive way in, and it is the button.
        tsl::changeTo<PairingScreen>(client_);
        return true;
      case Destination::kNone:
        break;
    }
    return false;
  }
  if ((keys_down & HidNpadButton_X) != 0) {
    if (view_.repair.state == ControlState::kInert) {
      return false;
    }
    PressRepair();
    return true;
  }
  return false;
}

void SettingsScreen::Poll() {
  since_poll_ = 1;

  // The port and the version handshake, both of which every screen needs and
  // none of which is this screen's own (`screen_frame.hpp`).
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    ShowUnreachable(link);
    return;
  }

  ipc::ConfigView config;
  const Result rc = client_.GetConfig(&config);
  if (R_FAILED(rc)) {
    // `GetConfig` is documented never to fail -- an unconfigured console has a
    // configuration -- so a failure is the transport or a payload this build
    // cannot read, and which of the two is `Diagnose`'s to say.
    ShowUnreachable(frame_.Diagnose(rc));
    return;
  }

  config_ = config;
  if (!config_.config.configured()) {
    // The console lost the server the second press was going to pair with, so
    // the first press goes with it. Only `confirming`: an outcome from a press
    // that already happened is still what the user needs to read.
    repair_.confirming = false;
  }
  Refresh();
}

void SettingsScreen::ShowUnreachable(Link link) {
  // The confirmation and the last answer go with the session they belonged to:
  // a half-pressed "Re-pair" surviving a sysmodule that went away would fire on
  // the first press after it came back. Every caller of this owes that reset,
  // which is most of why it is one function rather than five copies.
  repair_ = RepairState{};
  view_ = RenderSettingsUnreachable(link, frame_.sysmodule_interface());
  Rebuild();
}

void SettingsScreen::Refresh() {
  view_ = RenderSettings(config_, repair_);
  Rebuild();
}

void SettingsScreen::Rebuild() {
  const int was = cursor_;
  entries_.clear();
  for (std::size_t index = 0; index < view_.complaints.size(); ++index) {
    entries_.push_back(Entry{Entry::Kind::kComplaint, 0, index});
  }
  for (std::size_t section = 0; section < view_.sections.size(); ++section) {
    entries_.push_back(Entry{Entry::Kind::kSectionTitle, section, 0});
    for (std::size_t row = 0; row < view_.sections[section].rows.size(); ++row) {
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
                                   ? &view_.sections[entries_[index].section]
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

void SettingsScreen::PressRepair() {
  if (view_.repair.state == ControlState::kBlocked) {
    // Refused here rather than sent to be refused there: the screen already
    // knows the answer (`SetRepair`), and the refusal is already under the
    // greyed prompt. The same rule the sync screen keeps for a blocked button.
    return;
  }
  if (!repair_.confirming) {
    // The first press only asks. The action discards a working pairing and this
    // console has no dialog to put in front of it.
    repair_.confirming = true;
    repair_.outcome = RepairOutcome::kNone;
    Refresh();
    return;
  }
  repair_.confirming = false;

  // The port and the handshake, as before any other command.
  const Link link = frame_.Ready();
  if (link != Link::kOk) {
    ShowUnreachable(link);
    return;
  }

  // The half that can refuse for free, first. See the note in the header: until
  // `SdEngine::StartPairing` is real, discarding the token before asking would
  // leave a console that cannot pair again from here at all.
  auth::PairingStatus pairing;
  const Result start = client_.StartPair(&pairing);
  if (R_FAILED(start)) {
    ipc::Error error = ipc::Error::kOk;
    if (DecodeError(start, &error)) {
      // A refusal the sysmodule named, which is not a transport failure --
      // drawing `kUnavailable` as "sys-rommsync is not running" is exactly what
      // `DecodeError` exists to prevent (#25). Nothing was discarded.
      repair_.outcome = RepairOutcomeFor(error);
      Refresh();
      return;
    }
    ShowUnreachable(frame_.Diagnose(start));
    return;
  }

  // A pairing really is starting, so the old token goes. `Unpair` reports its
  // refusal as a `Result` -- it has no payload to carry one (`ipc::WriteOutcome`)
  // -- so the same two-way split applies here.
  const Result unpair = client_.Unpair();
  if (R_FAILED(unpair)) {
    ipc::Error error = ipc::Error::kOk;
    if (DecodeError(unpair, &error)) {
      repair_.outcome = RepairOutcome::kUnpairFailed;
      Refresh();
      return;
    }
    ShowUnreachable(frame_.Diagnose(unpair));
    return;
  }

  // The code, the address and the countdown are the pairing screen's (#27), and
  // it polls `GetPairState` off the attempt that has just started. Nothing is
  // rendered here first: a frame of this screen between the press and the code
  // would be a frame saying nothing.
  tsl::changeTo<PairingScreen>(client_);
}

const SettingsRow* SettingsScreen::CursorRow() const {
  if (cursor_ < 0 || static_cast<std::size_t>(cursor_) >= entries_.size()) {
    return nullptr;
  }
  const Entry& entry = entries_[static_cast<std::size_t>(cursor_)];
  if (entry.kind != Entry::Kind::kRow) {
    return nullptr;
  }
  return &view_.sections[entry.section].rows[entry.index];
}

s32 SettingsScreen::PromptRows() const {
  s32 rows = 0;
  const SettingsRow* row = CursorRow();
  if (row != nullptr && row->selectable) {
    ++rows;
  }
  if (view_.repair.state != ControlState::kInert) {
    ++rows;
    if (!view_.repair.refusal.empty()) {
      ++rows;
    }
  }
  return rows;
}

void SettingsScreen::Draw(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                          s32 height) const {
  // Nothing is drawn past the bounds `CustomDrawer` handed us, and nothing runs
  // off the right edge: `drawString`'s `maxWidth` defaults to "no limit", and a
  // folder path is the user's own text with no length this screen can assume.
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
    renderer->drawString(text, false, x, row, font, color, line_width);
    row += advance;
  };

  line(view_.headline, kHeadlineFont, ColorFor(view_.tone), kHeadlineHeight);
  line(view_.hint, kBodyFont, muted, kHintHeight);
  line(view_.notice, kBodyFont, ColorFor(view_.notice_tone), kHintHeight);

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
        renderer->drawString(">", false, x, row, kBodyFont, plain, kRowIndent);
      }
    };

    if (entry.kind == Entry::Kind::kComplaint) {
      const Line& complaint = view_.complaints[entry.index];
      if (clipped || row + kRowHeight > list_bottom) {
        break;
      }
      marker();
      renderer->drawString(complaint.label, false, x + kRowIndent, row, kNoteFont,
                           ColorFor(complaint.tone), kValueColumn);
      renderer->drawString(complaint.value, false, x + kRowIndent + kValueColumn, row, kNoteFont,
                           ColorFor(complaint.tone), value_width);
      row += kRowHeight;
      continue;
    }

    if (entry.kind == Entry::Kind::kSectionTitle) {
      const SettingsSection& section = view_.sections[entry.section];
      if (clipped || row + kSectionHeight > list_bottom) {
        break;
      }
      marker();
      renderer->drawString(section.title, false, x + kRowIndent, row, kBodyFont, plain,
                           indented_width);
      row += kSectionHeight;
      // A section with no rows is a section with something to say -- a platform
      // switched off, a folder map too large to have been sent -- so the note
      // is drawn whether or not the cursor is on it.
      if (!section.note.empty()) {
        if (row + kNoteHeight > list_bottom) {
          break;
        }
        renderer->drawString(section.note, false, x + kRowIndent, row, kNoteFont,
                             ColorFor(section.note_tone), indented_width);
        row += kNoteHeight;
      }
      continue;
    }

    const SettingsRow& entry_row = view_.sections[entry.section].rows[entry.index];
    if (clipped || row + kRowHeight > list_bottom) {
      break;
    }
    marker();
    renderer->drawString(entry_row.label, false, x + kRowIndent, row, kBodyFont, muted,
                         kValueColumn);
    renderer->drawString(entry_row.value, false, x + kRowIndent + kValueColumn, row, kBodyFont,
                         ColorFor(entry_row.tone), value_width);
    row += kRowHeight;

    // The note under the row it is about, and only for the row the cursor is
    // on: a list with a sentence under every folder is a list nobody can scan,
    // and a `roms` entry's note is only worth reading for the row being read.
    if (selected && !entry_row.note.empty()) {
      if (row + kNoteHeight > list_bottom) {
        break;
      }
      renderer->drawString(entry_row.note, false, x + kRowIndent, row, kNoteFont,
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
    line(Prompt(kGlyphA, "Open"), kBodyFont, plain, kRowHeight);
  }
  if (view_.repair.state != ControlState::kInert) {
    line(Prompt(kGlyphX, view_.repair.label), kBodyFont,
         view_.repair.state == ControlState::kLive ? plain : muted, kRowHeight);
    line(view_.repair.refusal, kNoteFont, ColorFor(view_.repair.refusal_tone), kNoteHeight);
  }
}

}  // namespace rommsync::overlay

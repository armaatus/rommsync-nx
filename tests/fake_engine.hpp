// The `ipc::Engine` the unit scenarios drive `ipc::ServiceCore` through.
//
// Shared rather than copied: `ipc.*` (tests/test_ipc.cpp) and `overlay.*`
// (tests/test_overlay_native.cpp) both need a service to talk to, and the second
// one talks to it through the *overlay's* real `IpcClient` -- so the two suites
// have to agree about what the sysmodule behind it does, and two copies of this
// class would be two things to keep in step (M9-7, #198).
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "rommsync/auth.hpp"
#include "rommsync/config.hpp"
#include "rommsync/conflict_log.hpp"
#include "rommsync/ipc.hpp"

namespace fakes {

namespace auth = rommsync::auth;
namespace config = rommsync::config;
namespace conflicts = rommsync::conflicts;
namespace ipc = rommsync::ipc;

class FakeEngine : public ipc::Engine {
 public:
  config::Config settings = config::Defaults();
  std::vector<config::Diagnostic> notes;
  ipc::EngineSnapshot snapshot;
  auth::PairingStatus pairing;

  ipc::Error set_enabled_error = ipc::Error::kOk;
  /// False models a write that reported success and did not take -- which is
  /// what `SetEnabled` answering with the *effective* state exists to catch.
  bool set_enabled_applies = true;

  ipc::Error apply_edit_error = ipc::Error::kOk;
  std::vector<config::Diagnostic> apply_edit_notes;
  ipc::ConfigEdit last_edit;

  /// What the last tick said about the emulators' folders (#230).
  std::vector<config::Diagnostic> sources;

  bool sync_accepted = true;
  int sync_requests = 0;

  ipc::Error start_pairing_error = ipc::Error::kOk;
  ipc::Error unpair_error = ipc::Error::kOk;

  ipc::Error enqueue_error = ipc::Error::kOk;
  std::int32_t enqueue_position = 1;
  std::int64_t last_rom_id = 0;
  ipc::Error dequeue_error = ipc::Error::kOk;

  ipc::Error list_begin_error = ipc::Error::kOk;
  ipc::Cursor issued_cursor = 7;
  ipc::ListRequest last_list;
  ipc::ListPage page;
  ipc::Error list_next_error = ipc::Error::kOk;
  ipc::Error list_end_error = ipc::Error::kOk;
  ipc::Cursor last_cursor = 0;

  const config::Config& config() const override { return settings; }
  const std::vector<config::Diagnostic>& config_diagnostics() const override { return notes; }
  std::vector<config::Diagnostic> source_notes() const override { return sources; }
  ipc::EngineSnapshot Snapshot() const override { return snapshot; }
  auth::PairingStatus pairing_status() const override { return pairing; }

  ipc::Error SetSyncEnabled(bool enabled) override {
    if (set_enabled_applies) {
      settings.sync.enabled = enabled;
    }
    return set_enabled_error;
  }

  ipc::Error ApplyConfigEdit(const ipc::ConfigEdit& edit,
                             std::vector<config::Diagnostic>* diagnostics) override {
    last_edit = edit;
    *diagnostics = apply_edit_notes;
    return apply_edit_error;
  }

  bool RequestSync() override {
    ++sync_requests;
    return sync_accepted;
  }

  /// #242: an application in the foreground.
  bool application_running_now = false;
  bool application_running() const override { return application_running_now; }

  ipc::Error StartPairing() override { return start_pairing_error; }
  ipc::Error Unpair() override { return unpair_error; }

  ipc::Error Enqueue(std::int64_t rom_id, std::int32_t* position) override {
    last_rom_id = rom_id;
    if (enqueue_error == ipc::Error::kOk) {
      *position = enqueue_position;
    }
    return enqueue_error;
  }

  ipc::Error Dequeue(std::int64_t rom_id) override {
    last_rom_id = rom_id;
    return dequeue_error;
  }

  ipc::Error ListBegin(const ipc::ListRequest& request, ipc::Cursor* cursor) override {
    last_list = request;
    if (list_begin_error == ipc::Error::kOk) {
      *cursor = issued_cursor;
    }
    return list_begin_error;
  }

  ipc::Error ListNext(ipc::Cursor cursor, ipc::ListPage* out) override {
    last_cursor = cursor;
    if (list_next_error == ipc::Error::kOk) {
      *out = page;
    }
    return list_next_error;
  }

  ipc::Error ListEnd(ipc::Cursor cursor) override {
    last_cursor = cursor;
    return list_end_error;
  }

  ipc::Error ListConflicts(const ipc::ConflictQuery& query, ipc::ConflictPage* page) override {
    last_conflict_query = query;
    page->offset = query.offset;
    page->total = static_cast<std::int32_t>(conflicts_.size());
    std::size_t at = static_cast<std::size_t>(query.offset);
    for (; at < conflicts_.size() &&
           page->entries.size() < static_cast<std::size_t>(query.limit);
         ++at) {
      if (!ipc::AppendIfItFits(page, conflicts_[at])) {
        break;
      }
    }

    page->has_more = at < conflicts_.size();
    return ipc::Error::kOk;
  }

  /// Taken, and -- unless `restore_runs_inline` is off -- finished on the spot
  /// with `restore_report`, which is what an engine with no worker does. With it
  /// off the restore stays `kRunning` until the test calls `FinishRestore`: a
  /// copy on a slow card, as far as anything on the wire can tell (#209).
  ipc::RestoreState RequestRestore(std::int64_t entry_id) override {
    std::lock_guard<std::mutex> lock(restore_mutex_);
    last_restore_id = entry_id;
    ++restore_requests;
    return TakeLocked(entry_id);
  }

  /// Keep local (#249): the same restore, counted apart, so a test can tell
  /// which of the two the wire asked for.
  ipc::RestoreState RequestKeepLocal(std::int64_t entry_id) override {
    std::lock_guard<std::mutex> lock(restore_mutex_);
    last_keep_local_id = entry_id;
    ++keep_local_requests;
    return TakeLocked(entry_id);
  }

  ipc::RestoreState TakeLocked(std::int64_t entry_id) {
    if (restore_.phase == ipc::RestorePhase::kRunning) {
      return restore_;
    }
    restore_ = ipc::RestoreState{};
    restore_.entry_id = entry_id;
    restore_.phase = restore_runs_inline ? ipc::RestorePhase::kDone : ipc::RestorePhase::kRunning;
    if (restore_runs_inline) {
      restore_.report = restore_report;
    }
    return restore_;
  }

  ipc::RestoreState restore_state() const override {
    std::lock_guard<std::mutex> lock(restore_mutex_);
    return restore_;
  }

  /// The copy a test was holding has finished, with `restore_report`.
  void FinishRestore() {
    std::lock_guard<std::mutex> lock(restore_mutex_);
    restore_.phase = ipc::RestorePhase::kDone;
    restore_.report = restore_report;
  }

  /// The sysmodule restarted: whatever it was doing, it no longer knows.
  void ForgetRestore() {
    std::lock_guard<std::mutex> lock(restore_mutex_);
    restore_ = ipc::RestoreState{};
  }

  std::vector<ipc::ConflictRow> conflicts_;
  ipc::ConflictQuery last_conflict_query;
  std::int64_t last_restore_id = 0;
  int restore_requests = 0;
  std::int64_t last_keep_local_id = 0;
  int keep_local_requests = 0;
  conflicts::RestoreReport restore_report;
  bool restore_runs_inline = true;

 private:
  /// The overlay's suites reach this engine from `IpcClient`'s worker thread
  /// while the test thread finishes the restore, so the one piece both touch is
  /// guarded.
  mutable std::mutex restore_mutex_;
  ipc::RestoreState restore_;
};

}  // namespace fakes

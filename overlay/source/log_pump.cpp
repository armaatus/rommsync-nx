#include "log_pump.hpp"

#include <cstddef>
#include <string>

#include "ipc_client.hpp"
#include "poller.hpp"
#include "rommsync/ipc.hpp"
#include "rommsync/overlay_log_view.hpp"
#include "rommsync/overlay_status_view.hpp"
#include "screen_frame.hpp"

namespace rommsync::overlay {

LogPump::LogPump(IpcClient& client)
    : client_(client),
      poller_(frame_, client_, ipc::Command::kGetLog, kFramesBetweenReads,
              ipc::EncodeLogRequest(ipc::kMaxLogLines)) {}

void LogPump::Frame() {
  const Poller::Event event = poller_.Frame();
  switch (event.kind) {
    case Poller::Event::Kind::kNone:
      return;
    case Poller::Event::Kind::kUnreachable:
      if (notice_.text == kStillApplying) {
        // A Y that was out when the link went: nothing can now say whether the
        // bundle was written, and "still applying" would outlive the outage.
        notice_ = RenderDiagnostics(DiagnosticsOutcome::kNoAnswer, "");
      }
      view_ = RenderLogUnreachable(event.link, frame_.sysmodule_interface());
      return;
    case Poller::Event::Kind::kStillApplying:
      // Out past the budget: the bundle is being written and will be. Lost to
      // a call that took the line instead: nothing can say whether it was, so
      // it is the sentence that says where to look.
      notice_ = RenderDiagnostics(
          poller_.sending() ? DiagnosticsOutcome::kApplying : DiagnosticsOutcome::kNoAnswer, "");
      view_.notice = notice_.text;
      view_.notice_tone = notice_.tone;
      return;
    case Poller::Event::Kind::kSent:
      Answered(event.rc);
      return;
    case Poller::Event::Kind::kRead:
      Read(event.rc);
      return;
  }
}

void LogPump::Read(Result rc) {
  ipc::LogTail tail;
  if (R_SUCCEEDED(rc)) {
    rc = client_.Decode(ipc::DecodeLogTail, &tail);
  }
  if (R_FAILED(rc)) {
    // `GetLog` is documented never to fail, so a failure is the transport or a
    // payload this build cannot read, and which is `Diagnose`'s to say.
    view_ = RenderLogUnreachable(frame_.Diagnose(rc), frame_.sysmodule_interface());
    return;
  }
  view_ = RenderLog(tail);
  view_.back = back_;
  view_.notice = notice_.text;
  view_.notice_tone = notice_.tone;
}

void LogPump::PressDiagnostics() {
  // One at a time: a second Y while the first is out goes nowhere, and the
  // first one's answer is what the notice will say.
  poller_.Send(ipc::Command::kWriteDiagnostics, DiagnosticsRequestPayload());
}

void LogPump::Answered(Result rc) {
  ipc::DiagnosticsResult result;
  ipc::Error refusal = ipc::Error::kOk;
  DiagnosticsOutcome outcome = DiagnosticsOutcome::kNoAnswer;
  if (R_SUCCEEDED(rc)) {
    if (R_SUCCEEDED(client_.Decode(ipc::DecodeDiagnosticsResult, &result))) {
      outcome = DiagnosticsOutcome::kWritten;
    }
  } else if (DecodeError(rc, &refusal) && refusal == ipc::Error::kWriteFailed) {
    outcome = DiagnosticsOutcome::kWriteFailed;
  } else if (DecodeError(rc, &refusal) && refusal == ipc::Error::kUnknownCommand) {
    outcome = DiagnosticsOutcome::kOutdated;
  }
  notice_ = RenderDiagnostics(outcome, result.sd_path);
  view_.notice = notice_.text;
  view_.notice_tone = notice_.tone;
}

void LogPump::Scroll(int rows, std::size_t room) {
  back_ = ScrollLog(back_, rows, view_.lines.size(), room);
  view_.back = back_;
}

}  // namespace rommsync::overlay

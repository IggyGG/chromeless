// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbBeginFrameDriver — see cb_begin_frame_driver.h for the full rationale
// (the renderer-vs-root-compositor distinction, the ack-chained pacing
// contract, and the external-begin-frame-control precondition).

#include "capture/build-integration/cb_begin_frame_driver.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "ui/compositor/compositor.h"

namespace cloud_browser {

namespace {

// Cadence guardrails, expressed as INTERVALS (not rates). NOTE the inversion:
// the *fastest* allowed rate (60 fps) is the *smallest* interval, hence
// kFastestRateInterval < kSlowestRateInterval. We refuse to run faster than
// 60 fps (each tick is a software composite + blit on the GPU-less worker —
// see the CPU note below) and slower than 1 fps (below that the stream is not
// "video" and viz's own 1s idle refresh dominates anyway).
//
// std::clamp(value, lo, hi) REQUIRES lo <= hi — and that holds here only
// because kFastestRateInterval (16.67ms) < kSlowestRateInterval (1000ms). Do
// NOT "fix" the apparent rate/interval name mismatch by swapping these in the
// clamp call: passing lo > hi to std::clamp is undefined behavior.
constexpr base::TimeDelta kFastestRateInterval = base::Hertz(60);  // ~16.67ms
constexpr base::TimeDelta kSlowestRateInterval = base::Hertz(1);   // 1000ms

base::TimeDelta ClampFrameInterval(base::TimeDelta requested) {
  return std::clamp(requested, kFastestRateInterval, kSlowestRateInterval);
}

}  // namespace

CbBeginFrameDriver::CbBeginFrameDriver(ui::Compositor* compositor,
                                       base::TimeDelta target_frame_interval)
    : compositor_(compositor),
      target_frame_interval_(ClampFrameInterval(target_frame_interval)) {
  DCHECK(compositor_);
}

CbBeginFrameDriver::~CbBeginFrameDriver() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Stop();
}

void CbBeginFrameDriver::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (running_) {
    return;
  }

  // Precondition check. IssueExternalBeginFrame is a silent no-op (it buffers
  // a single arg and returns) unless the compositor was constructed with
  // use_external_begin_frame_control=true. If the flag is missing the whole
  // fix is inert and video stays at ~0.5 fps — so make the misconfiguration
  // LOUD in the serial log instead of letting it masquerade as the original
  // bug. We still proceed (the loop is harmless when inert) so the failure is
  // observable rather than a hard crash on a deploy with the flag forgotten.
  if (!compositor_->use_external_begin_frame_control()) {
    LOG(ERROR)
        << "CbBeginFrameDriver: root compositor was NOT created with "
           "use_external_begin_frame_control=true — IssueExternalBeginFrame "
           "will not drive frames and video will remain starved (~0.5 fps). "
           "Check CbAuraPlatformData's WindowTreeHost construction.";
  }

  running_ = true;
  LOG(INFO) << "CbBeginFrameDriver: starting external BeginFrame loop at "
            << (1.0 / target_frame_interval_.InSecondsF()) << " fps target ("
            << target_frame_interval_.InMillisecondsF() << "ms interval)";
  IssueOneBeginFrame();
}

void CbBeginFrameDriver::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }
  running_ = false;
  next_frame_timer_.Stop();
  // Drop any in-flight ack: an IssueExternalBeginFrame issued before Stop()
  // may still invoke its completion callback asynchronously. Invalidating the
  // WeakPtr makes that late OnBeginFrameAck a no-op so we don't re-arm after
  // teardown.
  weak_factory_.InvalidateWeakPtrs();
  LOG(INFO) << "CbBeginFrameDriver: stopped external BeginFrame loop";
}

void CbBeginFrameDriver::IssueOneBeginFrame() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }

  const base::TimeTicks now = base::TimeTicks::Now();
  last_issue_time_ = now;

  // Belt-and-suspenders deque-drain guarantee. The PRIMARY drain path is the
  // renderer submitting damaging frames under this external BeginFrame: that
  // damage reaches the root Display (DisplayDamageTracker::OnDisplayDamaged →
  // needs_draw_), the Display draws+SWAPS (should_swap = should_draw &&
  // size_matches, and the FrameSinkVideoCapturer's CopyOutputRequest alone
  // already makes should_draw true on M147), the software output surface
  // returns a SUCCESS PresentationFeedback (SWAP_ACK), and Blink's
  // LayerTreeView::DidPresentCompositorFrame erases the matching entries from
  // its presentation-callback deque (which otherwise FATAL-DCHECKs at
  // layer_tree_view.cc:574 once >60 accrue). That path holds in steady state.
  // The ONE residual leak edge is size_matches==false during a surface-resize
  // race, where the Display would draw-without-swap and no success feedback
  // flows. ScheduleFullRedraw() forces root-surface damage every tick so the
  // Display always has a reason to swap — exactly the property the former
  // ScheduleCompositorKeepaliveRedraw keepalive provided, now retained as a
  // cheap, idempotent safety net (it costs nothing extra: the tick's
  // BeginFrame already drives a composite). Call it BEFORE issuing the
  // BeginFrame so the damage is present when the Display processes this tick.
  compositor_->ScheduleFullRedraw();

  // Build a manual-source BeginFrame, mirroring headless_web_contents_impl.cc.
  // kManualSourceId marks this as an externally-driven (not vsync) source.
  // deadline = now + interval is the renderer's main-frame budget for this
  // tick; viz tolerates us choosing it (a software, no-vsync deadline). We
  // pass no unthrottled_interval; on non-Mac/Android the arg then defaults to
  // base::TimeDelta() (zero), which trivially satisfies the BeginFrameArgs
  // unthrottled-interval DCHECK (0 <= interval * jitter).
  viz::BeginFrameArgs args = viz::BeginFrameArgs::Create(
      BEGINFRAME_FROM_HERE, viz::BeginFrameArgs::kManualSourceId,
      next_sequence_number_++, now, now + target_frame_interval_,
      target_frame_interval_, viz::BeginFrameArgs::NORMAL);

  // force=true: guarantee OnDisplayDidFinishFrame (→ OnBeginFrameAck) fires
  // for this BeginFrame even if the page is static and nothing draws, so the
  // ack-chain never stalls. See the header's PACING section.
  //
  // Robust against an unbound controller at start-of-day: the viz
  // external_begin_frame_controller_ is bound asynchronously once the GPU
  // channel is established, which may be AFTER our first Start(). If we issue
  // before then, ui::Compositor stashes this single arg in
  // pending_begin_frame_args_ and replays it on SetExternalBeginFrameController
  // (which then acks normally → the loop continues). Because we are strictly
  // ack-chained (the next issue happens only from OnBeginFrameAck), we never
  // issue a second time while one is pending, so we cannot trip ui::Compositor's
  // DCHECK(!pending_begin_frame_args_) for the pre-bind window.
  compositor_->IssueExternalBeginFrame(
      args, /*force=*/true,
      base::BindOnce(&CbBeginFrameDriver::OnBeginFrameAck,
                     weak_factory_.GetWeakPtr()));
}

void CbBeginFrameDriver::OnBeginFrameAck(const viz::BeginFrameAck& /*ack*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }

  // Hold the target cadence: schedule the next issue for one interval after
  // the PREVIOUS issue, not one interval after now. If frame production took
  // longer than the interval, residual is non-positive and we issue the next
  // BeginFrame immediately (delay clamped to zero); a fast frame waits out the
  // remainder. Either way we never overlap (the previous frame just acked, so
  // pending_frame_callback_ is clear — no overlap DCHECK risk).
  const base::TimeDelta elapsed = base::TimeTicks::Now() - last_issue_time_;
  const base::TimeDelta residual = target_frame_interval_ - elapsed;
  if (residual.is_positive()) {
    next_frame_timer_.Start(
        FROM_HERE, residual,
        base::BindOnce(&CbBeginFrameDriver::IssueOneBeginFrame,
                       weak_factory_.GetWeakPtr()));
  } else {
    IssueOneBeginFrame();
  }
}

}  // namespace cloud_browser

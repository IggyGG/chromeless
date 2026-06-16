// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// CbBeginFrameDriver — drives the offscreen root compositor (and, through
// the viz frame-sink hierarchy, the captured renderer) at a fixed target
// frame rate by issuing external BeginFrames.
//
// ===================================================================
// WHY THIS EXISTS — the renderer-vs-root-compositor distinction
// ===================================================================
//
// cb-chromium runs Aura on Xvfb (`Xvfb :99 -screen 0 1280x720x24`) with
// SwiftShader software GL (`--use-gl=angle --use-angle=swiftshader-webgl`).
// There is NO real hardware vsync BeginFrameSource. The video pipeline
// captures a WebContents via viz::mojom::FrameSinkVideoCapturer, which is a
// PULL consumer: it only delivers a new frame (OnFrameCaptured) when the
// captured renderer's CompositorFrameSinkSupport produces a NEW, damaging
// CompositorFrame.
//
// CRITICAL FACT (verified against Chromium branch-heads/7727 source):
// attaching a FrameSinkVideoCapturer does NOT cause BeginFrames to be sent
// to the captured renderer. compositor_frame_sink_support.cc computes
// `needs_begin_frame_` from client_needs_begin_frame_ / frame-timing /
// pending-surfaces / layer-context — there is NO "video capture" term.
// CompositorFrameSinkSupport::OnClientCaptureStarted only exempts the sink
// from throttling (FrameSinkManagerImpl::OnCaptureStarted →
// captured_frame_sink_ids_.insert + UpdateThrottling); it never requests a
// BeginFrame. With nothing else driving the renderer, viz's
// FrameSinkVideoCapturerImpl falls back to its idle refresh_frame_retry_timer_
// (bounded by kMaxRefreshDelay = 1 second), re-copying the *last* aggregated
// surface roughly once per second — which is exactly the ~0.5 fps the
// dual-ended wire measurement observed at the source (frames_encoded≈0.51fps).
//
// This corrects two stale assumptions previously recorded in the tree:
//   * cb_devtools_agent.cc CreateNewTarget's comment that "the framesink
//     capturer keeps the renderer awake" — it does NOT.
//   * The previous ScheduleCompositorKeepaliveRedraw comment "Capture is
//     unaffected (the copy path is independent of swap)" — capture is in
//     fact STARVED for the same root reason (no BeginFrame production), and
//     a root-compositor ScheduleFullRedraw() does not drive the renderer's
//     frame sink.
//
// THE FIX: drive BeginFrames ourselves. ui::Compositor::IssueExternalBeginFrame
// posts a BeginFrame into the Display's ExternalBeginFrameSourceMojo, which
// REPLACES the Display's default BeginFrameSource and is registered on the
// root frame sink. FrameSinkManagerImpl::RegisterBeginFrameSource +
// RecursivelyAttachBeginFrameSource propagate that source down the frame-sink
// hierarchy. The captured renderer's frame sink is a hierarchy CHILD of the
// root aura compositor's frame sink (DelegatedFrameHost::AttachToCompositor →
// ui::Compositor::AddChildFrameSink → HostFrameSinkManager::
// RegisterFrameSinkHierarchy(root, renderer)), so a single external
// BeginFrame on the root ticks the renderer's cc::Scheduler exactly like a
// vsync would. For an animating page the renderer commits + submits a new
// CompositorFrame → surface damage → the Display draws+swaps → the
// FrameSinkVideoCapturer's OnFrameDamaged fires → a NEW captured frame
// reaches the encoder. This same draw+swap also emits the present-acks that
// drain Blink's LayerTreeView presentation-callback deque, so this driver
// SUBSUMES the old ScheduleCompositorKeepaliveRedraw keepalive (which only
// drove the root UI compositor and existed to avoid the
// layer_tree_view.cc:574 FATAL-DCHECK at ~2.5min).
//
// ===================================================================
// PACING — why this is ack-chained, not a free-running RepeatingTimer
// ===================================================================
//
// ExternalBeginFrameSourceMojo::IssueExternalBeginFrame holds a single
// pending_frame_callback_ and DCHECKs `!pending_frame_callback_` on entry
// ("Got overlapping IssueExternalBeginFrame"). Issuing the next BeginFrame
// before the previous one is acknowledged trips that DCHECK and FATALs the
// worker. So we MUST self-clock off the ack: issue one BeginFrame with
// force=true, wait for the BeginFrameAck callback, then schedule the next
// issue after whatever delay keeps us at the target cadence.
//
// force=true is load-bearing for liveness: it calls Display::
// SetNeedsOneBeginFrame so OnDisplayDidFinishFrame (→ the ack callback)
// fires for EVERY issued BeginFrame even when the page is static and nothing
// draws. Without force, a static page would never ack and the ack-chain
// would stall. (force does NOT synthesize damage; a truly static page emits
// BeginFrameAck(has_damage=false) and capture legitimately does not advance
// until content changes — which is correct behavior.)
//
// ===================================================================
// PRECONDITION — compositor must be external-begin-frame-control enabled
// ===================================================================
//
// IssueExternalBeginFrame only does anything if the ui::Compositor was
// constructed with use_external_begin_frame_control=true (a const ctor arg
// that binds external_begin_frame_controller_ via the viz context factory).
// CbAuraPlatformData constructs the root WindowTreeHost's compositor with
// that flag set (see cb_aura_platform_data.cc). If the flag is NOT set, the
// controller is unbound and IssueExternalBeginFrame silently buffers a single
// arg and produces nothing — so the driver verifies the flag at Start() and
// LOG(ERROR)s loudly rather than failing silently.
//
// Cross-references:
//   * ui/compositor/compositor.h         IssueExternalBeginFrame
//   * components/viz/common/frame_sinks/begin_frame_args.h
//   * components/viz/service/frame_sinks/external_begin_frame_source_mojo.cc
//   * headless/lib/browser/headless_web_contents_impl.cc (the upstream
//     pattern this mirrors — manual-source-id BeginFrameArgs, force=true,
//     per-frame completion callback)

#ifndef CAPTURE_BUILD_INTEGRATION_CB_BEGIN_FRAME_DRIVER_H_
#define CAPTURE_BUILD_INTEGRATION_CB_BEGIN_FRAME_DRIVER_H_

#include <cstdint>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"

namespace ui {
class Compositor;
}  // namespace ui

namespace viz {
struct BeginFrameAck;
}  // namespace viz

namespace cloud_browser {

class CbBeginFrameDriver {
 public:
  // |compositor| is the root aura UI compositor (CbAuraPlatformData's
  // host()->compositor()). It MUST have been created with
  // use_external_begin_frame_control=true and MUST outlive this driver —
  // main_parts owns both and orders teardown so the driver is reset before
  // the Aura host. |target_frame_interval| is the cadence cap (the documented
  // target is 30 fps → ~33.3ms — NOT 60; the redraw cost is paid in software
  // on a GPU-less worker, see the .cc CPU note). The value is clamped to a
  // sane range in the ctor.
  CbBeginFrameDriver(ui::Compositor* compositor,
                     base::TimeDelta target_frame_interval);

  CbBeginFrameDriver(const CbBeginFrameDriver&) = delete;
  CbBeginFrameDriver& operator=(const CbBeginFrameDriver&) = delete;

  ~CbBeginFrameDriver();

  // Begin the ack-chained BeginFrame loop. Idempotent — a second call while
  // running is a no-op. Verifies the compositor's external-begin-frame-control
  // precondition and LOG(ERROR)s (but still proceeds) if it is not satisfied,
  // so a misconfiguration is loud in the serial log rather than a silent
  // 0.5 fps regression.
  //
  // LIFECYCLE CONSTRAINT: this driver is built to be started ONCE and stopped
  // ONCE (main_parts starts it in PreMainMessageLoopRun and only reset()s it
  // at teardown). A Start() issued immediately after a Stop() is NOT safe if
  // the BeginFrame that was in flight at Stop() has not yet been acknowledged
  // by viz: Stop() invalidates our ack WeakPtr but cannot retract the
  // BeginFrame already handed to ExternalBeginFrameSourceMojo (it clears its
  // pending_frame_callback_ only when the Display finishes that frame), so a
  // fresh issue inside that window trips viz's
  // DCHECK(!pending_frame_callback_) "Got overlapping IssueExternalBeginFrame".
  // If a restart-able driver is ever needed, gate the first post-Start issue
  // on the prior frame being known-drained (e.g. an epoch counter that lets
  // the stale ack land and clear viz's pending callback first).
  void Start();

  // Stop issuing BeginFrames. Any in-flight BeginFrame's ack is ignored (the
  // WeakPtr is invalidated) and the pending re-arm timer is cancelled.
  // Idempotent. See the Start() LIFECYCLE CONSTRAINT before pairing this with
  // a later Start().
  void Stop();

  bool running() const { return running_; }

 private:
  // Issue exactly one external BeginFrame (force=true) with the next
  // monotonically-increasing sequence number, binding OnBeginFrameAck as the
  // completion callback. Called first from Start(), then re-armed from the
  // ack path via next_frame_timer_.
  void IssueOneBeginFrame();

  // Completion callback for an issued BeginFrame. Runs on this sequence once
  // the Display has finished the frame (drawn or decided not to). Schedules
  // the next IssueOneBeginFrame() after the residual delay needed to hold the
  // target cadence (so a frame that took longer than the interval issues the
  // next immediately, and a fast frame waits out the remainder).
  void OnBeginFrameAck(const viz::BeginFrameAck& ack);

  SEQUENCE_CHECKER(sequence_checker_);

  const raw_ptr<ui::Compositor> compositor_;
  const base::TimeDelta target_frame_interval_;

  bool running_ = false;

  // Monotonically increasing; viz DCHECKs sequence_number >=
  // BeginFrameArgs::kStartingFrameNumber (1), so we start at 1.
  uint64_t next_sequence_number_ = 1;

  // When the most recent BeginFrame was issued — used to compute the residual
  // delay to the next one so the loop holds |target_frame_interval_| rather
  // than (interval + frame-production-time).
  base::TimeTicks last_issue_time_;

  // Re-arms IssueOneBeginFrame() after OnBeginFrameAck, honoring the residual
  // cadence delay. A one-shot per tick (re-Start()ed each ack).
  base::OneShotTimer next_frame_timer_;

  // Invalidated by Stop()/dtor so a late ack callback for an in-flight
  // BeginFrame (delivered async by the compositor) cannot re-enter the loop
  // after we've torn down.
  base::WeakPtrFactory<CbBeginFrameDriver> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_BEGIN_FRAME_DRIVER_H_

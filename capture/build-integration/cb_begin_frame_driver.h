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
// THE FIX (part 1 of 3): drive BeginFrames ourselves.
// ui::Compositor::IssueExternalBeginFrame posts a BeginFrame into the Display's
// ExternalBeginFrameSourceMojo, which REPLACES the Display's default
// BeginFrameSource and is registered on the root frame sink.
// FrameSinkManagerImpl::RegisterBeginFrameSource + RecursivelyAttachBeginFrame
// Source propagate that source DOWN the frame-sink hierarchy. The captured
// renderer's frame sink IS a hierarchy CHILD of the root aura compositor's
// frame sink (DelegatedFrameHost::AttachToCompositor → ui::Compositor::
// AddChildFrameSink → HostFrameSinkManager::RegisterFrameSinkHierarchy(root,
// renderer)), and RecursivelyAttachBeginFrameSource verifiably calls
// support->SetBeginFrameSource(our_external_source) on EVERY descendant
// support — so our external source IS the source set on the renderer's
// CompositorFrameSinkSupport. (Re-verified against branch-heads/7727:
// frame_sink_manager_impl.cc RecursivelyAttachBeginFrameSource walks
// mapping.children and calls SetBeginFrameSource per node; the renderer is a
// non-root sink with no source of its own.)
//
// ===================================================================
// THE BUG THIS FIX ALONE DID NOT CLOSE — the observer-subscription gate
// ===================================================================
//
// 2026-06-16, MEASURED ON LIVE STAGING: the part-1 driver above (commit
// 2e5d147), built+baked+rolled and confirmed-in-binary, DID NOT lift fps.
// fps_decoded_mean stayed at 0.15 with the exact pre-fix ~30s-burst signature.
// Root cause, re-verified against branch-heads/7727 viz source:
//
//   SetBeginFrameSource(source) does NOT make the support OBSERVE the source.
//   compositor_frame_sink_support.cc only calls begin_frame_source_->
//   AddObserver(this) from StartObservingBeginFrameSource(), reached ONLY when
//       needs_begin_frame_ = (client_needs_begin_frame_ ||
//                             !frame_timing_details_.empty() ||
//                             !pending_surfaces_.empty() ||
//                             layer_context_wants_begin_frames_)
//   is true. For an IDLE captured renderer all four terms are false, so the
//   renderer's support is NOT in our ExternalBeginFrameSource's observers_ set.
//   ExternalBeginFrameSource::OnBeginFrame iterates observers_ — with the
//   renderer absent, our 30 Hz ticks fan out to ZERO renderer observers and
//   the renderer produces nothing. (Our ack still fires every tick because
//   force=true routes display_->SetNeedsOneBeginFrame and the Display path
//   acks independent of any renderer observer — which is exactly why the
//   part-1 driver SPUN happily while delivering 0 fps. The ack-chain liveness
//   MASKED the dead renderer.) The ~30s bursts are the renderer's
//   intensive-timer / non-rAF damage events (and viz's kMaxRefreshDelay=1s
//   idle refresh) briefly re-subscribing the support, not our driver working.
//
// client_needs_begin_frame_ is set by the RENDERER's cc::Scheduler via the
// SetNeedsBeginFrame mojo call, which it raises only while it has pending
// animation/damage. Critically, requestAnimationFrame itself is GATED on
// receiving BeginFrames AND on the document being visible — Chromium stops rAF
// for hidden/occluded/out-of-view content (M52+). There is NO browser-process
// API to force a child frame sink to observe begin frames it did not request:
// HostFrameSinkManager exposes no RegisterBeginFrameSource (that lives on the
// viz-process FrameSinkManagerImpl), and ui::Compositor/RenderWidgetHostImpl
// expose no "force continuous renderer production" hook. The CDP mechanism
// that DOES bypass the gate — HeadlessExperimental.beginFrame, which "sends
// screenshotting BeginFrames even if needsBeginFrames is false" by injecting
// at the renderer's own widget compositor — is unavailable on our binary
// (spike-beginframe/findings.md: the domain is chrome-headless-shell-only).
//
// ===================================================================
// THE FIX (parts 2 + 3) — make each delivered tick a full frame, and keep
// the captured renderer subscribed
// ===================================================================
//
// Part 2 (launch flag, infra/launch-chromeless.sh):
//   --run-all-compositor-stages-before-draw sets LayerTreeSettings::
//   wait_for_all_pipeline_stages_before_draw. With it, the renderer's
//   cc::SchedulerStateMachine runs BeginMainFrame + commit + activate + draw
//   on EVERY BeginFrame instead of skipping the main-thread stages when
//   ShouldSendBeginMainFrame() sees no damage. So once the renderer IS
//   subscribed, each of our external ticks deterministically produces a
//   complete, fresh CompositorFrame the capturer can deliver — the canonical
//   headless-deterministic-capture configuration.
//
// Part 3 (visibility, already in cb_devtools_agent.cc CreateNewTarget +
//   Cb.startFrameSinkCapture): the captured WebContents is WasShown()+Focus()'d
//   so the renderer treats the document as VISIBLE — the precondition for its
//   cc::Scheduler to keep raising client_needs_begin_frame_ (rAF runs only on a
//   visible doc). This driver additionally LOGs (see the diagnostic API below)
//   whether the captured renderer is actually producing frames, so a future
//   regression where the page is NOT visible/animating is self-explaining in
//   the serial log instead of silently degrading to 0.15 fps.
//
// HONEST RESIDUAL: parts 1-3 sustain capture at the target fps for a VISIBLE,
// ANIMATING page (rAF / CSS animation / continuous damage). They do NOT, and
// architecturally CANNOT from the browser process alone, force a genuinely
// STATIC page (no rAF, nothing dirty) to emit fresh frames every 33ms — the
// renderer legitimately has nothing new to draw and unsubscribes. For a true
// constant-frame-rate wire on a static page, the encoder/track-source layer
// must hold-and-repeat the last frame (the capturer does not synthesize
// duplicates). That CFR-repeat is tracked separately; this driver's job is to
// guarantee that an animating page's frames are not throttled below its own
// production rate, and to make the BeginFrame path observable.
//
// This driver still SUBSUMES the old ScheduleCompositorKeepaliveRedraw: each
// tick's root draw+swap emits the present-acks that drain Blink's LayerTreeView
// presentation-callback deque (the layer_tree_view.cc:574 FATAL-DCHECK at
// ~2.5min), which the keepalive existed to prevent.
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

class WebContentsResolver;
class CloudBrowserFrameSinkCapturer;

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
  //
  // |resolver| and |capturer| are DIAGNOSTIC-ONLY, may be nullptr, and are NOT
  // used to drive frames — they exist so the driver's ~5s self-report can name
  // WHERE the BeginFrame dies (see SetDiagnosticSources). |resolver| resolves
  // the currently-captured WebContents → its RenderWidgetHost → FrameSinkId so
  // the log can show whether a renderer is even attached and which sink it is.
  // |capturer| exposes the running frame counters (frames_received /
  // frames_delivered) so the log can show whether the captured renderer is
  // actually PRODUCING frames under our ticks, vs. the ticks fanning out to a
  // renderer that never subscribed (the 2026-06-16 failure mode). Both are
  // raw, non-owning; main_parts owns them and orders teardown so the driver is
  // reset before either. If null, the diagnostic degrades gracefully (it logs
  // the ack/issue cadence it CAN see and notes the source is unavailable).
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

  // Wire the diagnostic-only observation sources (see ctor doc). Safe to call
  // before or after Start(); the periodic self-report starts with Start() and
  // reads whatever is wired at report time (so main_parts can construct the
  // driver early and attach the capturer/resolver once they exist). Passing
  // nullptr for either clears it. Does NOT affect frame production in any way.
  void SetDiagnosticSources(WebContentsResolver* resolver,
                            CloudBrowserFrameSinkCapturer* capturer);

 private:
  // Issue exactly one external BeginFrame (force=true) with the next
  // monotonically-increasing sequence number, binding OnBeginFrameAck as the
  // completion callback. Called first from Start(), then re-armed from the
  // ack path via next_frame_timer_.
  void IssueOneBeginFrame();

  // Completion callback for an issued BeginFrame. |issue_epoch| is the epoch
  // captured when the frame was issued; if it no longer matches issue_epoch_
  // the stall watchdog has since abandoned that frame and the ack is ignored.
  // Otherwise, runs on this sequence once the Display has finished the frame
  // (drawn or decided not to) and schedules the next IssueOneBeginFrame() after
  // the residual delay needed to hold the target cadence.
  void OnBeginFrameAck(uint64_t issue_epoch, const viz::BeginFrameAck& ack);

  // Fires only if no OnBeginFrameAck arrived within kStallWatchdogTimeout of an
  // issue — i.e. viz dropped the pending frame callback and the ack-chain
  // froze. Re-issues to restart the loop. See stall_watchdog_timer_.
  void OnStallWatchdog();

  // ~5s self-report (re-armed by diagnostic_timer_). Logs, in one line:
  //   * issued/acked BeginFrame counts since the last report (proves OUR loop
  //     is live — the part-1 driver spun fine here while producing 0 fps, so
  //     this number alone is NOT success);
  //   * whether a renderer is even attached to capture, its FrameSinkId, and
  //     whether the captured WebContents' RWHV reports visible (the
  //     precondition for the renderer to keep requesting BeginFrames);
  //   * the capturer's frames_received delta — the GROUND TRUTH of whether the
  //     captured renderer is actually producing frames under our ticks. If
  //     issued>>0 but frames_received≈0, the BeginFrame is dying at the
  //     renderer's observer-subscription gate (renderer idle / not subscribed),
  //     which is the exact 2026-06-16 failure and is now self-evident in-log
  //     instead of requiring a wire-side harness to infer.
  // This is the deliverable's "make the next measurement self-explaining about
  // which layer drops the tick" requirement.
  void EmitDiagnostic();

  SEQUENCE_CHECKER(sequence_checker_);

  const raw_ptr<ui::Compositor> compositor_;
  const base::TimeDelta target_frame_interval_;

  bool running_ = false;

  // Monotonically increasing; viz DCHECKs sequence_number >=
  // BeginFrameArgs::kStartingFrameNumber (1), so we start at 1.
  uint64_t next_sequence_number_ = 1;

  // Bumped by OnStallWatchdog when it abandons a frozen frame. Each issue
  // captures the current value; OnBeginFrameAck drops acks whose captured epoch
  // is stale (the watchdog already re-issued), so an abandoned frame's late ack
  // cannot re-arm a second concurrent issue.
  uint64_t issue_epoch_ = 0;

  // False until the first-ever BeginFrame ack arrives. In that boot window a
  // watchdog fire almost always means the viz external-begin-frame controller
  // is not bound yet (GPU channel still initializing on a cold microVM), and
  // ui::Compositor holds our first issue in pending_begin_frame_args_ for
  // replay-on-bind — so the watchdog must NOT re-issue (a second pre-bind
  // issue corrupts the bind/create sequence; observed live as viz FATAL
  // `!has_created_frame_sink_manager_`, viz_main_impl.cc:342). See
  // OnStallWatchdog's boot-window guard.
  bool first_ack_received_ = false;

  // Consecutive stall-watchdog fires with NO intervening OnBeginFrameAck.
  // Reset to 0 by every ack. This is the WARM-RESTORE generalization of the
  // `first_ack_received_` boot-window guard: `first_ack_received_` is a
  // one-way latch that only suppresses the pre-FIRST-ack unbound window, but
  // the GPU channel can go transiently UNBOUND AGAIN after acks have already
  // flowed — most notably on a warm-snapshot restore (`CV2-WARM
  // StartNativeSession`), which re-establishes the GPU channel and re-runs the
  // FrameSinkManager create sequence. In that second unbound window a watchdog
  // re-issue is the SAME double-issue that crashes the GPU process
  // (viz_main_impl.cc:342 `!has_created_frame_sink_manager_`), but
  // `first_ack_received_` is already true so the boot-window guard is bypassed.
  // We cannot distinguish "controller transiently unbound (WAIT — the stashed
  // frame replays on rebind)" from "controller bound but a pending callback was
  // genuinely dropped mid-stream (RE-ISSUE — the 2026-06-16 capture-start
  // Show() stall)" from a single missed ack — but we CAN distinguish them by
  // waiting one watchdog cycle: an unbound window resolves itself (the rebind
  // replays our stashed frame → an ack arrives → this counter resets to 0),
  // whereas a genuine mid-stream freeze stays frozen (a SECOND consecutive fire
  // with still no ack). So the watchdog WAITS on the first fire and only
  // re-issues once >= kWatchdogFiresBeforeReissue fires have accrued with no
  // ack. Costs at most one extra kStallWatchdogTimeout (~1s) of recovery
  // latency for a genuine freeze; buys crash-immunity for every unbound-window
  // re-issue (cold OR warm).
  int watchdog_fires_without_ack_ = 0;

  // When the most recent BeginFrame was issued — used to compute the residual
  // delay to the next one so the loop holds |target_frame_interval_| rather
  // than (interval + frame-production-time).
  base::TimeTicks last_issue_time_;

  // Re-arms IssueOneBeginFrame() after OnBeginFrameAck, honoring the residual
  // cadence delay. A one-shot per tick (re-Start()ed each ack).
  base::OneShotTimer next_frame_timer_;

  // STALL WATCHDOG. The ack-chain is re-armed ONLY by OnBeginFrameAck; if viz
  // ever drops the pending frame callback (observed 2026-06-16 on firecracker:
  // forcing the captured RenderWidgetHostView + its aura ancestors visible at
  // capture-start reconfigures the Display and silently discards the in-flight
  // ExternalBeginFrame callback, so the ack never fires and the loop freezes at
  // issued=0 — VERDICT=DRIVER-STALLED), the loop would otherwise die forever.
  // This one-shot, armed on every issue and cancelled by the ack, fires only if
  // NO ack arrived within kStallWatchdogTimeout (1s — two orders of magnitude
  // beyond the 33ms frame deadline, so a non-arrived ack at this point means a
  // genuinely dropped callback, NOT a slow frame). On fire it re-issues to
  // restart the chain. The long timeout also makes the re-issue safe against
  // viz's DCHECK(!pending_frame_callback_): after 1s with no
  // OnDisplayDidFinishFrame, the Display has finished (or been reset past) that
  // frame and cleared pending_frame_callback_, so a fresh issue does not
  // overlap. A legitimate frame never takes 1s (force=true acks a static frame
  // in one composite), so the watchdog never fires in steady state.
  base::OneShotTimer stall_watchdog_timer_;

  // --- diagnostic-only state (see ctor / SetDiagnosticSources) ---

  // Non-owning observation handles. Either may be null. Read only by
  // EmitDiagnostic; never used to drive frames.
  raw_ptr<WebContentsResolver> diag_resolver_ = nullptr;
  raw_ptr<CloudBrowserFrameSinkCapturer> diag_capturer_ = nullptr;

  // Repeating ~5s self-report. Armed in Start(), stopped in Stop().
  base::RepeatingTimer diagnostic_timer_;

  // Counters since the last EmitDiagnostic(), so the report shows a RATE
  // (issued/acked per interval) rather than an ever-growing total. Reset each
  // report.
  uint64_t issued_since_report_ = 0;
  uint64_t acked_since_report_ = 0;

  // Capturer frames_received at the last report, to compute the per-interval
  // delta (the ground-truth "is the renderer producing under our ticks").
  uint64_t last_reported_frames_received_ = 0;

  // Invalidated by Stop()/dtor so a late ack callback for an in-flight
  // BeginFrame (delivered async by the compositor) cannot re-enter the loop
  // after we've torn down.
  base::WeakPtrFactory<CbBeginFrameDriver> weak_factory_{this};
};

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_BEGIN_FRAME_DRIVER_H_

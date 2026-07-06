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
#include "base/strings/string_number_conversions.h"
#include "capture/build-integration/cb_active_webcontents_resolver.h"
#include "capture/framesink-capturer/capturer.h"
#include "components/viz/common/frame_sinks/begin_frame_args.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/compositor/compositor.h"

namespace cloud_browser {

namespace {

// Cadence of the driver's self-report. Decoupled from the BeginFrame interval:
// long enough that the log isn't spammy, short enough that a stuck-at-0.15fps
// regression is visible within a few lines of a 120s soak.
constexpr base::TimeDelta kDiagnosticInterval = base::Seconds(5);

// Stall-watchdog timeout. If no ack arrives within this of an issue, the
// pending frame callback was dropped (e.g. a Display reconfiguration from
// forcing the captured view visible) and the ack-chain has frozen. 1s is ~30x
// the 33ms frame deadline, so a non-arrived ack here is a genuine drop, not a
// slow frame — and the Display has long since cleared pending_frame_callback_,
// making the watchdog's re-issue safe against viz's overlap DCHECK.
constexpr base::TimeDelta kStallWatchdogTimeout = base::Seconds(1);

// How many CONSECUTIVE stall-watchdog fires (each kStallWatchdogTimeout apart)
// with NO intervening BeginFrame ack must accrue before OnStallWatchdog
// actually re-issues. The first fire always WAITS (re-arm only): a single
// missed ack cannot be distinguished between "controller transiently unbound —
// the stashed frame will replay on rebind, so waiting fixes it for free" and
// "controller bound but a pending callback was genuinely dropped mid-stream".
// Re-issuing into the unbound case is the double-issue that crashes the GPU
// process (viz_main_impl.cc:342 `!has_created_frame_sink_manager_`), so we
// never re-issue on the first fire. A genuine mid-stream freeze survives the
// wait (a SECOND fire with still no ack) and is then recovered by the re-issue.
// 2 => wait one full cycle then re-issue: crash-immune on any unbound window
// (cold boot OR warm-restore) at a cost of <=1s extra freeze-recovery latency.
constexpr int kWatchdogFiresBeforeReissue = 2;


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

  // Arm the ~5s self-report. Reset the per-interval counters so the first
  // report reflects only post-Start activity. base::Unretained is safe: the
  // timer is a member, so it cannot outlive `this`, and Stop()/dtor stop it.
  issued_since_report_ = 0;
  acked_since_report_ = 0;
  last_reported_frames_received_ = 0;
  diagnostic_timer_.Start(FROM_HERE, kDiagnosticInterval,
                          base::BindRepeating(&CbBeginFrameDriver::EmitDiagnostic,
                                              base::Unretained(this)));

  IssueOneBeginFrame();
}

void CbBeginFrameDriver::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }
  running_ = false;
  next_frame_timer_.Stop();
  stall_watchdog_timer_.Stop();
  diagnostic_timer_.Stop();
  // Drop any in-flight ack: an IssueExternalBeginFrame issued before Stop()
  // may still invoke its completion callback asynchronously. Invalidating the
  // WeakPtr makes that late OnBeginFrameAck a no-op so we don't re-arm after
  // teardown.
  weak_factory_.InvalidateWeakPtrs();
  LOG(INFO) << "CbBeginFrameDriver: stopped external BeginFrame loop";
}

void CbBeginFrameDriver::SetDiagnosticSources(
    WebContentsResolver* resolver,
    CloudBrowserFrameSinkCapturer* capturer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  diag_resolver_ = resolver;
  diag_capturer_ = capturer;
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
  // Tag this issue with the current epoch. The completion callback carries the
  // epoch so a LATE ack for a watchdog-abandoned frame (epoch bumped) is
  // ignored — preventing it from re-arming a second concurrent issue and
  // tripping viz's overlapping-IssueExternalBeginFrame DCHECK.
  const uint64_t issue_epoch = issue_epoch_;
  compositor_->IssueExternalBeginFrame(
      args, /*force=*/true,
      base::BindOnce(&CbBeginFrameDriver::OnBeginFrameAck,
                     weak_factory_.GetWeakPtr(), issue_epoch));
  ++issued_since_report_;

  // Arm the stall watchdog. If OnBeginFrameAck does not fire within
  // kStallWatchdogTimeout, the callback was dropped and the chain has frozen;
  // OnStallWatchdog() re-issues to recover. The ack cancels this.
  stall_watchdog_timer_.Start(FROM_HERE, kStallWatchdogTimeout,
                              base::BindOnce(&CbBeginFrameDriver::OnStallWatchdog,
                                             weak_factory_.GetWeakPtr()));
}

void CbBeginFrameDriver::OnBeginFrameAck(uint64_t issue_epoch,
                                        const viz::BeginFrameAck& /*ack*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }
  // Ignore a late ack for a frame the watchdog already abandoned (epoch bumped):
  // the watchdog has re-issued, so re-arming here would create a second
  // concurrent issue. Dropping it is safe — the watchdog's fresh issue owns the
  // chain now.
  if (issue_epoch != issue_epoch_) {
    return;
  }
  first_ack_received_ = true;
  // An ack means the controller is bound and the chain is live again, so any
  // prior unbound-window/freeze wait is over — clear the consecutive-fire
  // counter so the next stall starts a fresh wait-then-reissue cycle.
  watchdog_fires_without_ack_ = 0;
  ++acked_since_report_;

  // The ack arrived, so the chain is healthy — cancel the stall watchdog before
  // re-arming. (If we didn't, a slow-but-not-frozen frame could let the
  // watchdog and the normal re-arm both schedule an issue.)
  stall_watchdog_timer_.Stop();

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

void CbBeginFrameDriver::OnStallWatchdog() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!running_) {
    return;
  }
  // CV2 2026-07-02 boot-window guard, GENERALIZED 2026-07-06 to cover any
  // transiently-unbound window (cold boot OR warm-restore), not just the
  // pre-first-ack one.
  //
  // A single missed ack cannot be attributed from here: it is EITHER the viz
  // external-begin-frame controller being unbound (GPU channel initializing on
  // a cold boot, OR re-establishing on a warm-snapshot restore — `CV2-WARM
  // StartNativeSession` re-runs the FrameSinkManager create sequence), in which
  // case ui::Compositor has our issue STASHED in pending_begin_frame_args_ and
  // will replay+ack it on rebind — WAITING fixes it for free; OR a genuine
  // mid-stream freeze (the 2026-06-16 capture-start Show() Display reconfigure
  // that silently drops the pending callback), which needs the re-issue kick.
  //
  // Re-issuing into the UNBOUND case is the double-issue that crashes the GPU
  // process on viz's `Check failed: !has_created_frame_sink_manager_`
  // (viz_main_impl.cc:342) — measured live at ~1/3 of fresh warm-restore
  // sessions (guest serial 019f3477-fe40 vs working control 019f3477-46c7,
  // 2026-07-06): the fatal watchdog took the re-issue branch because the old
  // `!first_ack_received_` latch was already true from pre-snapshot acks, so
  // the boot-window guard did not apply to the warm-restore unbound window.
  //
  // TWO layered guards:
  //
  // (1) PRE-FIRST-ACK (original cold-boot guard, kept as an INDEFINITE wait).
  //     Before the first-ever ack, a missed ack can ONLY be the controller not
  //     yet bound (there is no prior ack, so no "was-acking-then-froze"
  //     mid-stream freeze is even possible). ui::Compositor holds our first
  //     issue stashed for replay-on-bind. Re-issuing is never right here — the
  //     rebind will replay + ack whenever the GPU channel finishes initializing
  //     (routinely >1s, occasionally several seconds on a cold microVM). So we
  //     wait INDEFINITELY (re-arm every cycle, never re-issue) until that first
  //     ack. This preserves f9d79b1's exact behavior for the cold-boot case and
  //     must NOT be weakened to a bounded wait (a bounded wait would re-issue
  //     into a still-unbound controller on a slow cold boot = the crash).
  //
  // (2) POST-FIRST-ACK (new 2026-07-06 warm-restore/freeze handling). After
  //     acks have flowed, a missed ack is AMBIGUOUS: a transient re-unbind
  //     (warm-restore GPU-channel re-establishment) OR a genuine mid-stream
  //     freeze. We disambiguate by waiting exactly one cycle: a re-unbind
  //     self-resolves (rebind → stashed replay → ack → counter resets), a
  //     freeze does not. So re-issue only on the SECOND consecutive fire.
  ++watchdog_fires_without_ack_;
  const bool pre_first_ack = !first_ack_received_;
  const bool within_wait_window =
      watchdog_fires_without_ack_ < kWatchdogFiresBeforeReissue;
  if (pre_first_ack || within_wait_window) {
    LOG(WARNING) << "CbBeginFrameDriver: stall watchdog fired (no BeginFrame "
                    "ack in "
                 << kStallWatchdogTimeout.InMilliseconds()
                 << "ms; consecutive-fires-without-ack="
                 << watchdog_fires_without_ack_
                 << ", first_ack_received=" << first_ack_received_
                 << ") — controller "
                 << (pre_first_ack ? "not yet bound (cold-boot window)"
                                   : "may be re-establishing (warm-restore "
                                     "GPU-channel window)")
                 << "; NOT re-issuing (a stashed frame replays on bind; "
                    "re-issuing here would double-issue and crash the GPU on "
                    "!has_created_frame_sink_manager_), re-arming watchdog"
                 << (pre_first_ack ? " (waiting for first bind)"
                                   : " to wait one cycle");
    stall_watchdog_timer_.Start(
        FROM_HERE, kStallWatchdogTimeout,
        base::BindOnce(&CbBeginFrameDriver::OnStallWatchdog,
                       weak_factory_.GetWeakPtr()));
    return;
  }
  // No ack for kStallWatchdogTimeout => the pending frame callback was dropped
  // and the ack-chain froze (the 2026-06-16 capture-start Show() stall). The
  // pending re-arm timer (if any) is moot because the issue it would chain from
  // never acked; cancel it and re-issue directly to restart the loop. The 1s
  // gap makes this re-issue safe vs viz's DCHECK(!pending_frame_callback_): the
  // Display has finished/cleared that frame long ago.
  //
  // Bump the epoch FIRST so the abandoned frame's late ack (if it ever arrives)
  // is dropped by OnBeginFrameAck rather than re-arming a second issue.
  ++issue_epoch_;
  next_frame_timer_.Stop();
  // Reset the consecutive-fire counter: this re-issue starts a fresh attempt,
  // so a subsequent unbound window (after the re-issue but before its ack) must
  // again get a full wait-one-cycle grace rather than re-issuing immediately.
  // The IssueOneBeginFrame below arms the watchdog anew; if its ack never comes
  // the counter climbs from 0 again and we wait before the next re-issue.
  watchdog_fires_without_ack_ = 0;
  LOG(WARNING) << "CbBeginFrameDriver: stall watchdog fired (no BeginFrame ack "
                  "in "
               << (kStallWatchdogTimeout.InMilliseconds() *
                   kWatchdogFiresBeforeReissue)
               << "ms across " << kWatchdogFiresBeforeReissue
               << " consecutive fires) — controller was bound and acking but a "
                  "pending callback was genuinely dropped mid-stream; "
                  "re-issuing to restart the ack-chain";
  IssueOneBeginFrame();
}

void CbBeginFrameDriver::EmitDiagnostic() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // --- our own loop liveness (necessary but NOT sufficient — the part-1
  //     driver showed healthy issue/ack here while producing 0 fps) ---
  const uint64_t issued = issued_since_report_;
  const uint64_t acked = acked_since_report_;
  issued_since_report_ = 0;
  acked_since_report_ = 0;
  const double issued_fps = issued / kDiagnosticInterval.InSecondsF();

  // --- captured-renderer attachment + visibility (the subscription
  //     precondition: an idle/hidden renderer will not observe our source) ---
  std::string renderer_state = "resolver=unavailable";
  if (diag_resolver_) {
    content::WebContents* wc = diag_resolver_->GetActiveWebContents();
    if (!wc) {
      renderer_state = "no-active-capture (no WebContents being captured)";
    } else {
      std::string fsid_str = "fsid=?";
      std::string vis_str = "rwhv=none";
      if (content::RenderWidgetHostView* rwhv =
              wc->GetRenderWidgetHostView()) {
        vis_str = rwhv->IsShowing() ? "rwhv=SHOWING" : "rwhv=HIDDEN";
        if (content::RenderWidgetHost* rwh = rwhv->GetRenderWidgetHost()) {
          fsid_str = "fsid=" + rwh->GetFrameSinkId().ToString();
        }
      }
      // Visibility::VISIBLE is the precondition for the renderer's cc::Scheduler
      // to keep raising client_needs_begin_frame_ (rAF runs only on a visible
      // doc). HIDDEN here would explain 0 production regardless of our ticks.
      renderer_state =
          "captured " + fsid_str + " " + vis_str + " wc_visibility=" +
          base::NumberToString(static_cast<int>(wc->GetVisibility()));
    }
  }

  // --- GROUND TRUTH: is the captured renderer actually producing frames under
  //     our ticks? frames_received is the capturer's count of OnFrameCaptured
  //     deliveries from viz. issued>>0 with frames_delta≈0 == BeginFrame dying
  //     at the renderer observer-subscription gate (the 2026-06-16 bug). ---
  std::string capture_state = "capturer=unavailable";
  bool capturer_present = false;
  uint64_t captured_delta = 0;
  if (diag_capturer_) {
    capturer_present = true;
    const uint64_t now_received = diag_capturer_->GetStats().frames_received;
    captured_delta = (now_received >= last_reported_frames_received_)
                         ? (now_received - last_reported_frames_received_)
                         : 0;
    last_reported_frames_received_ = now_received;
    const double capture_fps = captured_delta / kDiagnosticInterval.InSecondsF();
    capture_state = "frames_received +" + base::NumberToString(captured_delta) +
                    " (" + base::NumberToString(capture_fps) +
                    " fps captured), total=" + base::NumberToString(now_received);
  }

  // Single greppable line. The VERDICT field makes the failure mode explicit so
  // a reader (or a log-scraping harness) needn't cross-reference the wire. It
  // keys off the per-window CAPTURED delta (ground truth), not the total, so it
  // reflects current behaviour rather than history.
  const char* verdict;
  if (!capturer_present) {
    verdict = "UNKNOWN(no-capturer-handle)";
  } else if (issued == 0) {
    verdict = "DRIVER-STALLED(issued=0)";  // our own loop is dead
  } else if (captured_delta == 0) {
    // issued>>0 but nothing captured this window: BeginFrame is dying at the
    // renderer's observer-subscription gate (renderer idle / hidden / static,
    // so client_needs_begin_frame_=false and the support never subscribed to
    // our source). This is the 2026-06-16 failure, now self-evident in-log.
    verdict = "RENDERER-STARVED(ticks-not-reaching-renderer-or-renderer-idle)";
  } else if (issued >= 4 * captured_delta) {
    // We issue ~30/s but capture is materially below that: the renderer is
    // subscribing only intermittently (the ~30s-burst shape), not sustaining.
    verdict = "BURSTING(capture<<issued: renderer subscribing intermittently)";
  } else {
    verdict = "PRODUCING";
  }

  LOG(INFO) << "CbBeginFrameDriver[diag]: issued=" << issued << " ("
            << issued_fps << " fps) acked=" << acked << " | " << renderer_state
            << " | " << capture_state << " | VERDICT=" << verdict;
}

}  // namespace cloud_browser

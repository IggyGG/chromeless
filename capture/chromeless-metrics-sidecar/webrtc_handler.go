// Wave 2 A4: WebRTC pod-level observability emit.
//
// streamer.js fires fire-and-forget POSTs to /webrtc-event whenever a
// WebRTC lifecycle event occurs (session_created, dc.opened, ice.failed,
// session_closed, …). Each request is a single small JSON envelope:
//
//   { "event": "chromeless.webrtc.dc.opened",
//     "attrs": { "label": "cursor",
//                "session_id": "...",
//                "duration_ms": 1234,
//                "handshake_ms": 280 } }
//
// We translate the event name into a Prometheus counter increment (and,
// for `session_closed` / `dc.opened`, a histogram observation) using the
// metrics declared in
// /workspace/chemistry/elements/tools/chromeless/.triform/observability.yaml
// — names match the YAML exactly (with dots → underscores per Prometheus
// convention), so SigNoz dashboards keyed on the YAML schema bind to
// both the physics-side and chromeless-side emitters with one rule.
//
// All counters are dimensioned by `element_id` per the YAML; on top of
// that, `webrtc_dc_opened_count` is dimensioned by `label`
// (input|stats|cursor|clipboard|file-upload). The `element_id` value is
// taken from $CHROMELESS_ELEMENT_ID at process start; when unset (dev
// compose, ad-hoc smoke runs) it falls back to `_unknown`.
//
// OTLP-export gap (A4 closeout):
//   The sidecar currently has an OTLP *trace* exporter (tracing.go) but
//   no OTLP *metrics* exporter — Prometheus scrape is the only metrics
//   path today. That covers the D4-YAML metrics named here, but
//   Triform's event bus also wants the dot-named events
//   (`chromeless.webrtc.*`) at the OTLP-logs/events layer for activity
//   feed enrichment. Wiring an OTLP-logs exporter in this sidecar is a
//   future task; A4 caps at "events are produced and counters are
//   incrementing in /metrics".

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
)

// ---------------------------------------------------------------------------
// metrics — names mirror chemistry/elements/tools/chromeless/.triform/observability.yaml
//
// The YAML declares `chromeless_*` metric names (the chromeless element
// prefix is implicit there); we expose them here under the same names
// so a SigNoz / Prometheus query against the YAML doc matches the wire
// shape from this sidecar.
// ---------------------------------------------------------------------------

var (
	// element_id labelled, simple counters.
	mWebRTCSessionCreated = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_session_count",
		Help:        "WebRTC sessions initialised by the Pattern C broker (incremented on session_created).",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	// Counterpart to mWebRTCSessionCreated. With both, the D4-Dash
	// "Active sessions = created − closed" panel can be built directly
	// from /metrics — without this counter, dashboards have to derive
	// "closed" from histogram series cardinality, which double-counts
	// across pod restarts and is brittle. Same dimensionality as
	// mWebRTCSessionCreated (element_id only — no `label` because the
	// session is the unit, not the channel).
	mWebRTCSessionClosedCount = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_session_closed_count",
		Help:        "WebRTC sessions terminated (incremented on session_closed). Pair with chromeless_webrtc_session_count to chart active sessions.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCICEConnected = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_ice_connected_count",
		Help:        "ICE state transitioned to connected/completed.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCICEFailed = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_ice_failed_count",
		Help:        "ICE state transitioned to failed/disconnected past the recovery window.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCDCOpened = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_dc_opened_count",
		Help:        "DataChannel opened, dimensioned by channel label (input|stats|cursor|clipboard|file-upload).",
		ConstLabels: regionLabels(),
	}, []string{"element_id", "label"})
	mWebRTCReplayHit = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_replay_hit_count",
		Help:        "SDP replay buffer or ICE FIFO served a buffered envelope to a late-joining peer (broker-side; chromeless-side never increments — physics emits this).",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCReplayMiss = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_replay_miss_count",
		Help:        "Late-joining peer arrived but the broker buffer was already evicted (broker-side; physics emits this).",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})

	// Histograms — buckets come straight from the YAML.
	mWebRTCSessionDurationMs = promauto.NewHistogramVec(prometheus.HistogramOpts{
		Name:        "chromeless_webrtc_session_duration_ms",
		Help:        "Session lifetime from session_created to session_closed.",
		ConstLabels: regionLabels(),
		Buckets:     []float64{1000, 5000, 30000, 60000, 300000, 900000, 1800000, 3600000},
	}, []string{"element_id"})
	mWebRTCSignalingHandshakeMs = promauto.NewHistogramVec(prometheus.HistogramOpts{
		Name:        "chromeless_webrtc_signaling_handshake_ms",
		Help:        "User-perceived signaling latency from session_created to first dc.opened.",
		ConstLabels: regionLabels(),
		Buckets:     []float64{50, 100, 250, 500, 1000, 2500, 5000, 10000},
	}, []string{"element_id"})

	// In-process counters for events that don't have a YAML counter but
	// are still useful for sidecar-side debugging — surfaced under the
	// same `chromeless_webrtc_*` namespace so dashboards stay consistent.
	mWebRTCDCCursorCoalesced = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_dc_cursor_coalesced_count",
		Help:        "Cursor emitter dropped a stale pendingPayload because a newer one arrived before the queueMicrotask flush.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCDCClipboardUnsupportedMime = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_dc_clipboard_unsupported_mime_count",
		Help:        "Inbound clipboard envelope dropped because its MIME wasn't text/plain or text/html.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCDCClipboardStaleSeq = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_dc_clipboard_stale_seq_count",
		Help:        "Inbound clipboard envelope dropped because seq <= lastSeenInboundSeq.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCDCFileUploadOrphanTimeout = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_dc_file_upload_orphan_timeout_count",
		Help:        "File-upload buffer for an upload_id discarded because no chunk arrived within FILE_UPLOAD_ORPHAN_MS.",
		ConstLabels: regionLabels(),
	}, []string{"element_id"})
	mWebRTCDCFileUploadCompleted = promauto.NewCounterVec(prometheus.CounterOpts{
		Name:        "chromeless_webrtc_dc_file_upload_completed_count",
		Help:        "File-upload reassembly completed; result is one of ok|no-input|err.",
		ConstLabels: regionLabels(),
	}, []string{"element_id", "result"})
)

// ---------------------------------------------------------------------------
// element_id resolution
// ---------------------------------------------------------------------------

// elementIDLabel returns the value to stamp on the `element_id` series
// label. Read once at process start (ConstLabels can't change after
// promauto.New*); a stale env var doesn't matter because the sidecar is
// short-lived (one Chromium pod) and restarts pick up the new value.
//
// We don't use a ConstLabel because the YAML schema makes `element_id` a
// dynamic dimension (in case multiple chromeless tenants ever share one
// pod, although today it's 1:1). The runtime cost is one allocation
// per Inc(), which is negligible compared to the wire round-trip.
func elementIDLabel() string {
	if v := os.Getenv("CHROMELESS_ELEMENT_ID"); v != "" {
		return v
	}
	return "_unknown"
}

// ---------------------------------------------------------------------------
// /webrtc-event handler
// ---------------------------------------------------------------------------

// webrtcEventEnvelope is the on-wire shape POSTed by streamer.js.
//
// We deliberately kept this off the v1.1 stats envelope (the one in
// stats_handler.go) so the two endpoints stay independent — adding new
// event types here doesn't risk de-stabilising the stats pipeline that
// dashboards already depend on.
type webrtcEventEnvelope struct {
	Event string                 `json:"event"`
	Attrs map[string]interface{} `json:"attrs,omitempty"`
}

// attrString fetches a string attribute from the envelope.
func (e *webrtcEventEnvelope) attrString(key string) string {
	if e.Attrs == nil {
		return ""
	}
	if v, ok := e.Attrs[key].(string); ok {
		return v
	}
	return ""
}

// attrFloat64 fetches a numeric attribute (JSON numbers decode to
// float64 in Go's default decoder).
func (e *webrtcEventEnvelope) attrFloat64(key string) (float64, bool) {
	if e.Attrs == nil {
		return 0, false
	}
	switch n := e.Attrs[key].(type) {
	case float64:
		return n, true
	case json.Number:
		if f, err := n.Float64(); err == nil {
			return f, true
		}
	}
	return 0, false
}

// applyWebRTCEvent translates one envelope into Prometheus updates.
// Returns nil on success (including unknown event names — we accept
// future-event compatibility silently rather than 400-ing a deploy that
// shipped a newer streamer.js); errors only fire on malformed input.
func applyWebRTCEvent(env *webrtcEventEnvelope, log *slog.Logger) error {
	if env == nil {
		return errors.New("nil envelope")
	}
	if env.Event == "" {
		return errors.New("envelope missing event name")
	}

	elementID := elementIDLabel()

	switch env.Event {
	case "chromeless.webrtc.session_created":
		mWebRTCSessionCreated.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.session_closed":
		// Always bump the closed-count counter so the D4-Dash
		// "Active sessions = created − closed" panel can read both
		// series straight off /metrics. The duration histogram only
		// observes when duration_ms is present (a session_closed
		// without a duration is still a closed session — drop the
		// observation, keep the counter).
		mWebRTCSessionClosedCount.WithLabelValues(elementID).Inc()
		if d, ok := env.attrFloat64("duration_ms"); ok && d >= 0 {
			mWebRTCSessionDurationMs.WithLabelValues(elementID).Observe(d)
		}

	case "chromeless.webrtc.ice.connected":
		mWebRTCICEConnected.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.ice.failed":
		mWebRTCICEFailed.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.dc.opened":
		label := env.attrString("label")
		if label == "" {
			label = "_unknown"
		}
		mWebRTCDCOpened.WithLabelValues(elementID, label).Inc()
		// Optional handshake latency observation: only valid on the
		// FIRST dc.opened of a session (per D4-YAML), but the streamer
		// is responsible for filtering — the sidecar accepts whatever
		// arrives and trusts the emitter's discipline.
		if h, ok := env.attrFloat64("handshake_ms"); ok && h >= 0 {
			mWebRTCSignalingHandshakeMs.WithLabelValues(elementID).Observe(h)
		}

	case "chromeless.webrtc.replay.hit":
		// Broker-side concern (physics emits this); accept it on
		// principle so a future physics-side instrumentation pass can
		// route through any sidecar. Today the streamer never emits this.
		mWebRTCReplayHit.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.replay.miss":
		mWebRTCReplayMiss.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.dc.cursor.coalesced":
		mWebRTCDCCursorCoalesced.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.dc.clipboard.unsupported_mime":
		mWebRTCDCClipboardUnsupportedMime.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.dc.clipboard.stale_seq":
		mWebRTCDCClipboardStaleSeq.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.dc.file_upload.orphan_timeout":
		mWebRTCDCFileUploadOrphanTimeout.WithLabelValues(elementID).Inc()

	case "chromeless.webrtc.dc.file_upload.completed":
		result := env.attrString("result")
		if result == "" {
			result = "_unknown"
		}
		mWebRTCDCFileUploadCompleted.WithLabelValues(elementID, result).Inc()

	default:
		// Unknown event name: log once at debug, accept the request.
		// This keeps the wire forward-compatible — a streamer.js that
		// emits a new event name doesn't fail deployment if the sidecar
		// hasn't been updated yet.
		log.Debug("webrtc-event: unknown event name (accepted, no metric updated)",
			slog.String("event", env.Event),
		)
	}

	return nil
}

// webrtcEventHandler returns the http.HandlerFunc to register at
// /webrtc-event. Mirrors the `/stats-update` handler shape but doesn't
// need per-session state — every event is independent.
func webrtcEventHandler(log *slog.Logger) http.HandlerFunc {
	const maxBody = 16 * 1024 // 16 KiB; one event envelope is < 1 KiB.
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		body, err := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
		if err != nil {
			http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
			return
		}
		if len(body) > maxBody {
			http.Error(w, "body too large", http.StatusRequestEntityTooLarge)
			return
		}
		var env webrtcEventEnvelope
		if err := json.Unmarshal(body, &env); err != nil {
			http.Error(w, "decode: "+err.Error(), http.StatusBadRequest)
			return
		}
		if err := applyWebRTCEvent(&env, log); err != nil {
			http.Error(w, fmt.Sprintf("apply: %v", err), http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}
}

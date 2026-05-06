// Wave 2 A4 — webrtc-event handler unit tests.
//
// Mirrors stats_handler_test.go's shape: HTTP-level negative cases
// (bad method / malformed JSON / oversize body) plus per-event
// counter-increment assertions using the testutil package's
// CollectAndCount.

package main

import (
	"bytes"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/testutil"
	dto "github.com/prometheus/client_model/go"
)

// histSampleCount returns the cumulative sample_count of a single
// histogram series (i.e. how many times Observe was called on the
// matching label set). testutil.CollectAndCount counts distinct
// series, not observations — for that we have to read the dto.Metric
// proto directly. Used to assert observation discipline (e.g. "the
// handshake histogram observed exactly once across N dc.opened
// events" — which is what the streamer's firstDCOpenedSeen latch
// guarantees).
func histSampleCount(t *testing.T, h prometheus.Observer) uint64 {
	t.Helper()
	c, ok := h.(prometheus.Collector)
	if !ok {
		t.Fatalf("histSampleCount: %T does not implement prometheus.Collector", h)
	}
	ch := make(chan prometheus.Metric, 8)
	go func() {
		c.Collect(ch)
		close(ch)
	}()
	var total uint64
	for m := range ch {
		var pb dto.Metric
		if err := m.Write(&pb); err != nil {
			t.Fatalf("histSampleCount: write: %v", err)
		}
		if pb.Histogram == nil {
			t.Fatalf("histSampleCount: %T is not a histogram", h)
		}
		total += pb.Histogram.GetSampleCount()
	}
	return total
}

// resetWebRTCMetrics zeroes every chromeless_webrtc_* series so a test
// can assert on a clean baseline.
func resetWebRTCMetrics() {
	mWebRTCSessionCreated.Reset()
	mWebRTCSessionClosedCount.Reset()
	mWebRTCICEConnected.Reset()
	mWebRTCICEFailed.Reset()
	mWebRTCDCOpened.Reset()
	mWebRTCReplayHit.Reset()
	mWebRTCReplayMiss.Reset()
	mWebRTCSessionDurationMs.Reset()
	mWebRTCSignalingHandshakeMs.Reset()
	mWebRTCDCCursorCoalesced.Reset()
	mWebRTCDCClipboardUnsupportedMime.Reset()
	mWebRTCDCClipboardStaleSeq.Reset()
	mWebRTCDCFileUploadOrphanTimeout.Reset()
	mWebRTCDCFileUploadCompleted.Reset()
}

func quietLoggerWebRTC() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestWebRTCEventHandler_RejectsNonPOST(t *testing.T) {
	h := webrtcEventHandler(quietLoggerWebRTC())
	r := httptest.NewRequest(http.MethodGet, "/webrtc-event", nil)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("got status %d, want 405", w.Code)
	}
}

func TestWebRTCEventHandler_RejectsMalformedJSON(t *testing.T) {
	h := webrtcEventHandler(quietLoggerWebRTC())
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", bytes.NewBufferString("not json"))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("got status %d, want 400", w.Code)
	}
}

func TestWebRTCEventHandler_MissingEventName(t *testing.T) {
	h := webrtcEventHandler(quietLoggerWebRTC())
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(`{}`))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("got status %d, want 400 (missing event)", w.Code)
	}
}

func TestWebRTCEventHandler_SessionCreated(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "el-test")
	h := webrtcEventHandler(quietLoggerWebRTC())
	body := `{"event":"chromeless.webrtc.session_created","attrs":{"session_id":"s1"}}`
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("got status %d, want 204", w.Code)
	}
	if got := testutil.ToFloat64(mWebRTCSessionCreated.WithLabelValues("el-test")); got != 1 {
		t.Fatalf("session_count = %v, want 1", got)
	}
}

func TestWebRTCEventHandler_DCOpenedWithLabel(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "el-test")
	h := webrtcEventHandler(quietLoggerWebRTC())

	// Mirror the streamer's firstDCOpenedSeen latch: handshake_ms is
	// attached to the FIRST dc.opened event of the session only.
	// Subsequent dc.opened events for the same session_id carry the
	// label but no handshake_ms, so the handshake histogram should
	// observe exactly once across N=3 events. If a future regression
	// attaches handshake_ms to multiple dc.opened events, this test
	// fails fast — the previous CollectAndCount > 0 assertion would
	// have silently passed for any N >= 1.
	type dcEvent struct {
		label   string
		withHM  bool
	}
	events := []dcEvent{
		{label: "cursor", withHM: true},   // first → carries handshake_ms
		{label: "clipboard", withHM: false},
		{label: "file-upload", withHM: false},
	}
	for _, ev := range events {
		body := `{"event":"chromeless.webrtc.dc.opened","attrs":{"label":"` + ev.label + `","session_id":"s1"`
		if ev.withHM {
			body += `,"handshake_ms":250`
		}
		body += `}}`
		r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
		w := httptest.NewRecorder()
		h(w, r)
		if w.Code != http.StatusNoContent {
			t.Fatalf("label=%s: got status %d", ev.label, w.Code)
		}
	}
	if got := testutil.ToFloat64(mWebRTCDCOpened.WithLabelValues("el-test", "cursor")); got != 1 {
		t.Fatalf("dc_opened{label=cursor} = %v, want 1", got)
	}
	if got := testutil.ToFloat64(mWebRTCDCOpened.WithLabelValues("el-test", "clipboard")); got != 1 {
		t.Fatalf("dc_opened{label=clipboard} = %v, want 1", got)
	}
	if got := testutil.ToFloat64(mWebRTCDCOpened.WithLabelValues("el-test", "file-upload")); got != 1 {
		t.Fatalf("dc_opened{label=file-upload} = %v, want 1", got)
	}
	// Exactly one handshake observation, regardless of how many
	// dc.opened events fired — the streamer is responsible for the
	// "first dc.opened only" discipline (per YAML §51-55) and the
	// sidecar test models that contract.
	if got := histSampleCount(t, mWebRTCSignalingHandshakeMs.WithLabelValues("el-test")); got != 1 {
		t.Fatalf("handshake histogram sample_count = %d, want 1 (first dc.opened only per session)", got)
	}
}

func TestWebRTCEventHandler_SessionClosedDuration(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "el-test")
	h := webrtcEventHandler(quietLoggerWebRTC())
	body := `{"event":"chromeless.webrtc.session_closed","attrs":{"duration_ms":12345}}`
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("got status %d, want 204", w.Code)
	}
	// FU #34: session_closed bumps both the closed-count counter and
	// observes the duration histogram. The counter is what the
	// "active sessions = created − closed" panel reads.
	if got := testutil.ToFloat64(mWebRTCSessionClosedCount.WithLabelValues("el-test")); got != 1 {
		t.Fatalf("session_closed_count = %v, want 1", got)
	}
	if got := histSampleCount(t, mWebRTCSessionDurationMs.WithLabelValues("el-test")); got != 1 {
		t.Fatalf("session_duration histogram sample_count = %d, want 1", got)
	}
}

// FU #34: session_closed must increment the closed-count counter even
// when duration_ms is absent — a closed session is closed regardless
// of whether the emitter knew its duration. The histogram is the
// thing that gets dropped silently in that case, not the counter.
func TestWebRTCEventHandler_SessionClosedNoDurationStillCountsCounter(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "el-test")
	h := webrtcEventHandler(quietLoggerWebRTC())
	body := `{"event":"chromeless.webrtc.session_closed","attrs":{"session_id":"s1"}}`
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("got status %d, want 204", w.Code)
	}
	if got := testutil.ToFloat64(mWebRTCSessionClosedCount.WithLabelValues("el-test")); got != 1 {
		t.Fatalf("session_closed_count without duration_ms = %v, want 1", got)
	}
	if got := histSampleCount(t, mWebRTCSessionDurationMs.WithLabelValues("el-test")); got != 0 {
		t.Fatalf("session_duration histogram observed without duration_ms = %d, want 0", got)
	}
}

func TestWebRTCEventHandler_ICEFailed(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "el-test")
	h := webrtcEventHandler(quietLoggerWebRTC())
	body := `{"event":"chromeless.webrtc.ice.failed","attrs":{"reason":"timeout"}}`
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("got status %d, want 204", w.Code)
	}
	if got := testutil.ToFloat64(mWebRTCICEFailed.WithLabelValues("el-test")); got != 1 {
		t.Fatalf("ice_failed_count = %v, want 1", got)
	}
}

func TestWebRTCEventHandler_FileUploadCompletedResult(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "el-test")
	h := webrtcEventHandler(quietLoggerWebRTC())
	for _, result := range []string{"ok", "no-input", "err"} {
		body := `{"event":"chromeless.webrtc.dc.file_upload.completed","attrs":{"result":"` + result + `"}}`
		r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
		w := httptest.NewRecorder()
		h(w, r)
		if w.Code != http.StatusNoContent {
			t.Fatalf("result=%s: got status %d", result, w.Code)
		}
	}
	for _, result := range []string{"ok", "no-input", "err"} {
		if got := testutil.ToFloat64(mWebRTCDCFileUploadCompleted.WithLabelValues("el-test", result)); got != 1 {
			t.Fatalf("file_upload_completed{result=%s} = %v, want 1", result, got)
		}
	}
}

func TestWebRTCEventHandler_UnknownEventAccepted(t *testing.T) {
	resetWebRTCMetrics()
	h := webrtcEventHandler(quietLoggerWebRTC())
	body := `{"event":"chromeless.webrtc.future.thing.we.dont.know.yet"}`
	r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("unknown event got status %d, want 204 (forward-compat)", w.Code)
	}
}

func TestWebRTCEventHandler_ElementIDFromEnv(t *testing.T) {
	resetWebRTCMetrics()
	t.Setenv("CHROMELESS_ELEMENT_ID", "")
	if got := elementIDLabel(); got != "_unknown" {
		t.Fatalf("elementIDLabel() with empty env = %q, want _unknown", got)
	}
	t.Setenv("CHROMELESS_ELEMENT_ID", "abc-123")
	if got := elementIDLabel(); got != "abc-123" {
		t.Fatalf("elementIDLabel() = %q, want abc-123", got)
	}
}

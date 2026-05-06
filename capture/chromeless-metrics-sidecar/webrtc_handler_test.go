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

	"github.com/prometheus/client_golang/prometheus/testutil"
)

// resetWebRTCMetrics zeroes every chromeless_webrtc_* series so a test
// can assert on a clean baseline.
func resetWebRTCMetrics() {
	mWebRTCSessionCreated.Reset()
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
	for _, label := range []string{"cursor", "clipboard", "file-upload"} {
		body := `{"event":"chromeless.webrtc.dc.opened","attrs":{"label":"` + label + `","handshake_ms":250}}`
		r := httptest.NewRequest(http.MethodPost, "/webrtc-event", strings.NewReader(body))
		w := httptest.NewRecorder()
		h(w, r)
		if w.Code != http.StatusNoContent {
			t.Fatalf("label=%s: got status %d", label, w.Code)
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
	// Three handshake observations should land in the histogram.
	if got := testutil.CollectAndCount(mWebRTCSignalingHandshakeMs); got == 0 {
		t.Fatalf("handshake histogram has no series, want >= 1")
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
	if got := testutil.CollectAndCount(mWebRTCSessionDurationMs); got == 0 {
		t.Fatalf("session_duration histogram has no series, want >= 1")
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

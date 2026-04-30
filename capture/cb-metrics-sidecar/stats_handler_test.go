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
)

// quietLogger returns a logger that discards output; we only check
// behaviour through metrics + HTTP responses.
func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestStatsUpdateHandler_RejectsNonPOST(t *testing.T) {
	h, _ := statsUpdateHandler(quietLogger())
	r := httptest.NewRequest(http.MethodGet, "/stats-update", nil)
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("got status %d, want 405", w.Code)
	}
}

func TestStatsUpdateHandler_RejectsMalformedJSON(t *testing.T) {
	h, _ := statsUpdateHandler(quietLogger())
	r := httptest.NewRequest(http.MethodPost, "/stats-update", bytes.NewBufferString("not json"))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("got status %d, want 400", w.Code)
	}
}

func TestStatsUpdateHandler_RejectsUnsupportedVersion(t *testing.T) {
	h, _ := statsUpdateHandler(quietLogger())
	body := `{"v":2,"t":1,"sample":{"v":2,"t":1,"inbound":[],"outbound":[],"remoteInbound":[],"candidatePair":null}}`
	r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("got status %d, want 400", w.Code)
	}
	if !strings.Contains(w.Body.String(), "version") {
		t.Fatalf("error did not mention version: %q", w.Body.String())
	}
}

func TestStatsUpdateHandler_RejectsEmpty(t *testing.T) {
	h, _ := statsUpdateHandler(quietLogger())
	body := `{"v":1,"t":1}` // no sample, no event
	r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("got status %d, want 400", w.Code)
	}
}

func TestStatsUpdateHandler_AcceptsCodecFallbackEvent(t *testing.T) {
	// T54 events ride this same channel; they have no sample but a
	// non-empty event field. The handler must accept and not error.
	h, _ := statsUpdateHandler(quietLogger())
	body := `{"v":1,"t":1,"event":"codec_fallback","data":{"preferred":["VP9"],"negotiated":"H264"}}`
	r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("got status %d, want 204; body=%q", w.Code, w.Body.String())
	}
}

func TestStatsUpdateHandler_HappyPath_GaugesSet(t *testing.T) {
	// Reset the singleton metrics this test asserts on.
	mClientInboundFPS.Set(0)
	mClientPairRTT.Set(0)
	mClientRemoteInboundLossFraction.Set(0)

	h, _ := statsUpdateHandler(quietLogger())

	// First sample: prime byte/frame counters; gauges that depend on
	// deltas (bitrate) won't update yet, but FPS/RTT/loss should.
	first := `{"v":1,"t":1000,"sample":{
		"v":1,"t":1000,
		"inbound":[{"trackId":"v","kind":"video","bytesReceived":0,"packetsReceived":0,"packetsLost":0,"jitter":0,"framesPerSecond":30,"framesDropped":0,"framesReceived":0,"totalDecodeTime":0}],
		"outbound":[],
		"remoteInbound":[{"trackId":"v","kind":"video","roundTripTime":0.05,"packetsLost":2,"fractionLost":0.0125}],
		"candidatePair":{"currentRoundTripTime":0.062,"availableOutgoingBitrate":null,"availableIncomingBitrate":null,"localCandidateType":"host","remoteCandidateType":"srflx"}
	}}`
	r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(first))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("first POST: status %d, body %q", w.Code, w.Body.String())
	}
	// fps, rtt, loss should be set; bitrate stays at 0 until 2nd sample.
	if got := testutil.ToFloat64(mClientInboundFPS); got != 30 {
		t.Errorf("fps after first: %v want 30", got)
	}
	if got := testutil.ToFloat64(mClientPairRTT); got != 62 {
		t.Errorf("rtt after first: %v want 62", got)
	}
	if got := testutil.ToFloat64(mClientRemoteInboundLossFraction); got != 0.0125 {
		t.Errorf("loss fraction after first: %v want 0.0125", got)
	}

	// Second sample 1s later: 125_000 more bytes received → 1_000_000 bps.
	second := `{"v":1,"t":2000,"sample":{
		"v":1,"t":2000,
		"inbound":[{"trackId":"v","kind":"video","bytesReceived":125000,"packetsReceived":100,"packetsLost":2,"jitter":0,"framesPerSecond":29,"framesDropped":3,"framesReceived":29,"totalDecodeTime":0}],
		"outbound":[],
		"remoteInbound":[{"trackId":"v","kind":"video","roundTripTime":0.05,"packetsLost":2,"fractionLost":0.005}],
		"candidatePair":{"currentRoundTripTime":0.05,"availableOutgoingBitrate":null,"availableIncomingBitrate":null,"localCandidateType":"host","remoteCandidateType":"srflx"}
	}}`
	r = httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(second))
	w = httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("second POST: status %d, body %q", w.Code, w.Body.String())
	}
	if got := testutil.ToFloat64(mClientInboundBitrate); got != 1_000_000 {
		t.Errorf("bitrate after second: %v want 1000000", got)
	}
	if got := testutil.ToFloat64(mClientInboundFPS); got != 29 {
		t.Errorf("fps after second: %v want 29", got)
	}
	if got := testutil.ToFloat64(mClientPairRTT); got != 50 {
		t.Errorf("rtt after second: %v want 50", got)
	}
	if got := testutil.ToFloat64(mClientRemoteInboundLossFraction); got != 0.005 {
		t.Errorf("loss after second: %v want 0.005", got)
	}
	// 3 dropped frames cumulative; counter should reflect the delta.
	if got := testutil.ToFloat64(mClientInboundFramesDropped); got < 3 {
		t.Errorf("frames_dropped: %v want ≥ 3", got)
	}
}

func TestStatsUpdateHandler_RejectsHugeBody(t *testing.T) {
	h, _ := statsUpdateHandler(quietLogger())
	huge := bytes.Repeat([]byte("A"), 300*1024)
	r := httptest.NewRequest(http.MethodPost, "/stats-update", bytes.NewReader(huge))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("got status %d, want 413", w.Code)
	}
}

// statsfixture: silence unused-import warning for prometheus when other
// test additions remove the assertion that uses it.
var _ = prometheus.DefaultRegisterer

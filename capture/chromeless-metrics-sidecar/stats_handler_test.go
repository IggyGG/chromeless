package main

import (
	"bytes"
	"fmt"
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

// resetClientMetrics zeroes every cb_client_* series. Tests that assert
// on specific values must call this first.
func resetClientMetrics() {
	mClientInboundBitrate.Reset()
	mClientInboundFPS.Reset()
	mClientInboundFramesDropped.Reset()
	mClientPairRTT.Reset()
	mClientRemoteInboundLossFraction.Reset()
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
	resetClientMetrics()
	h, _ := statsUpdateHandler(quietLogger())

	// First sample: prime byte/frame counters; gauges that depend on
	// deltas (bitrate) won't update yet, but FPS/RTT/loss should.
	first := `{"v":1,"t":1000,"session_id":"dev","tenant_id":"tenant-A","sample":{
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
	// fps, rtt, loss should be set under the {tenant=tenant-A, session=dev} label.
	if got := testutil.ToFloat64(mClientInboundFPS.WithLabelValues("tenant-A", "dev")); got != 30 {
		t.Errorf("fps after first: %v want 30", got)
	}
	if got := testutil.ToFloat64(mClientPairRTT.WithLabelValues("tenant-A", "dev")); got != 62 {
		t.Errorf("rtt after first: %v want 62", got)
	}
	if got := testutil.ToFloat64(mClientRemoteInboundLossFraction.WithLabelValues("tenant-A", "dev")); got != 0.0125 {
		t.Errorf("loss fraction after first: %v want 0.0125", got)
	}

	// Second sample 1s later: 125_000 more bytes received → 1_000_000 bps.
	second := `{"v":1,"t":2000,"session_id":"dev","tenant_id":"tenant-A","sample":{
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
	if got := testutil.ToFloat64(mClientInboundBitrate.WithLabelValues("tenant-A", "dev")); got != 1_000_000 {
		t.Errorf("bitrate after second: %v want 1000000", got)
	}
	if got := testutil.ToFloat64(mClientInboundFPS.WithLabelValues("tenant-A", "dev")); got != 29 {
		t.Errorf("fps after second: %v want 29", got)
	}
	if got := testutil.ToFloat64(mClientPairRTT.WithLabelValues("tenant-A", "dev")); got != 50 {
		t.Errorf("rtt after second: %v want 50", got)
	}
	if got := testutil.ToFloat64(mClientRemoteInboundLossFraction.WithLabelValues("tenant-A", "dev")); got != 0.005 {
		t.Errorf("loss after second: %v want 0.005", got)
	}
	// 3 dropped frames cumulative; counter should reflect the delta.
	if got := testutil.ToFloat64(mClientInboundFramesDropped.WithLabelValues("tenant-A", "dev")); got < 3 {
		t.Errorf("frames_dropped: %v want ≥ 3", got)
	}
}

func TestStatsUpdateHandler_LabelsAnonymousFallback(t *testing.T) {
	resetClientMetrics()
	h, _ := statsUpdateHandler(quietLogger())
	// Pre-T82 client: no session_id / tenant_id fields. Sidecar should
	// bucket as "_anonymous" / "_anonymous".
	body := `{"v":1,"t":1000,"sample":{
		"v":1,"t":1000,
		"inbound":[{"trackId":"v","kind":"video","bytesReceived":0,"packetsReceived":0,"packetsLost":0,"jitter":0,"framesPerSecond":24,"framesDropped":0,"framesReceived":0,"totalDecodeTime":0}],
		"outbound":[],"remoteInbound":[],"candidatePair":null}}`
	r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("status %d", w.Code)
	}
	if got := testutil.ToFloat64(mClientInboundFPS.WithLabelValues("_anonymous", "_anonymous")); got != 24 {
		t.Errorf("fps under _anonymous labels: got %v want 24", got)
	}
}

func TestStatsUpdateHandler_PerSessionStateIsolation(t *testing.T) {
	// Two sessions emit samples interleaved; the bitrate computation
	// for session A must NOT use session B's prevBytesReceived.
	resetClientMetrics()
	h, _ := statsUpdateHandler(quietLogger())

	mkBody := func(sid string, t int64, bytes uint64) string {
		return `{"v":1,"t":` + fmt.Sprint(t) + `,"session_id":"` + sid + `","tenant_id":"tenant-A","sample":{` +
			`"v":1,"t":` + fmt.Sprint(t) + `,` +
			`"inbound":[{"trackId":"v","kind":"video","bytesReceived":` + fmt.Sprint(bytes) +
			`,"packetsReceived":0,"packetsLost":0,"jitter":0,"framesPerSecond":30,"framesDropped":0,"framesReceived":0,"totalDecodeTime":0}],` +
			`"outbound":[],"remoteInbound":[],"candidatePair":null}}`
	}
	post := func(body string) {
		r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
		w := httptest.NewRecorder()
		h(w, r)
		if w.Code != http.StatusNoContent {
			t.Fatalf("status %d body %q", w.Code, w.Body.String())
		}
	}
	// A starts at 0 bytes, B starts at 10_000 bytes. If state isolation
	// is broken, A's second sample (125_000 bytes) would compute the
	// delta against B's last value (10_000) instead of A's own 0.
	post(mkBody("A", 1000, 0))
	post(mkBody("B", 1000, 10_000))
	post(mkBody("A", 2000, 125_000)) // 125kB / 1s = 1_000_000 bps if state is per-session
	post(mkBody("B", 2000, 60_000))  // 50kB / 1s = 400_000 bps

	if got := testutil.ToFloat64(mClientInboundBitrate.WithLabelValues("tenant-A", "A")); got != 1_000_000 {
		t.Errorf("session A bitrate: got %v want 1_000_000 (state leak from B?)", got)
	}
	if got := testutil.ToFloat64(mClientInboundBitrate.WithLabelValues("tenant-A", "B")); got != 400_000 {
		t.Errorf("session B bitrate: got %v want 400_000", got)
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

// ---- T82 cardinality-cap unit tests ----

func TestLabelBucket_AnonymousNeverConsumesSlot(t *testing.T) {
	b := newLabelBucket()
	for i := 0; i < clientLabelCap; i++ {
		if got := b.label(""); got != clientAnonymousLabel {
			t.Fatalf("empty: got %q", got)
		}
		if got := b.label(clientAnonymousLabel); got != clientAnonymousLabel {
			t.Fatalf("explicit anonymous: got %q", got)
		}
	}
	for i := 0; i < clientLabelCap; i++ {
		if got := b.label(fmt.Sprintf("v%d", i)); got == clientOverflowLabel {
			t.Fatalf("anonymous illegally consumed slot at i=%d", i)
		}
	}
}

func TestLabelBucket_OverflowAfterCap(t *testing.T) {
	b := newLabelBucket()
	for i := 0; i < clientLabelCap; i++ {
		b.label(fmt.Sprintf("seen-%d", i))
	}
	// Known still maps to itself.
	if got := b.label("seen-0"); got != "seen-0" {
		t.Fatalf("known under cap got remapped: %q", got)
	}
	// Unseen overflows.
	if got := b.label("brand-new"); got != clientOverflowLabel {
		t.Fatalf("unseen-over-cap: got %q", got)
	}
	// Repeated overflow lookups don't pollute the seen map.
	for i := 0; i < 10; i++ {
		if got := b.label(fmt.Sprintf("over-%d", i)); got != clientOverflowLabel {
			t.Fatalf("over-%d: got %q", i, got)
		}
	}
}

func TestStatsUpdateHandler_CardinalityCap(t *testing.T) {
	resetClientMetrics()
	h, router := statsUpdateHandler(quietLogger())
	// Fill the session bucket to capacity.
	for i := 0; i < clientLabelCap; i++ {
		body := `{"v":1,"t":1,"session_id":"sess-` + fmt.Sprint(i) + `","tenant_id":"t","sample":{` +
			`"v":1,"t":1,"inbound":[],"outbound":[],"remoteInbound":[],"candidatePair":null}}`
		r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
		w := httptest.NewRecorder()
		h(w, r)
	}
	// 101st distinct session should bucket into _other.
	body := `{"v":1,"t":2,"session_id":"sess-overflow","tenant_id":"t","sample":{` +
		`"v":1,"t":2,"inbound":[{"trackId":"v","kind":"video","bytesReceived":0,"packetsReceived":0,"packetsLost":0,"jitter":0,"framesPerSecond":42,"framesDropped":0,"framesReceived":0,"totalDecodeTime":0}],` +
		`"outbound":[],"remoteInbound":[],"candidatePair":null}}`
	r := httptest.NewRequest(http.MethodPost, "/stats-update", strings.NewReader(body))
	w := httptest.NewRecorder()
	h(w, r)
	if w.Code != http.StatusNoContent {
		t.Fatalf("status %d body %q", w.Code, w.Body.String())
	}
	if got := testutil.ToFloat64(mClientInboundFPS.WithLabelValues("t", "_other")); got != 42 {
		t.Errorf("overflowed session not bucketed as _other: got %v", got)
	}
	// Side check: the router's session bucket reports the overflow.
	if router.sessionBucket.label("sess-overflow") != clientOverflowLabel {
		t.Errorf("router didn't classify sess-overflow as _other")
	}
}

// statsfixture: silence unused-import warning when other test additions
// remove the assertion that uses it.
var _ = prometheus.DefaultRegisterer

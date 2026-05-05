// T72 stats forwarding loop: simulate the streamer page POSTing a
// StatsSample envelope to the chromeless-metrics-sidecar and assert that the
// `cb_client_*` gauges + counter on /metrics reflect the values.
//
// We build the sidecar binary on demand (its module is separate from
// signaling/) and launch it on a random port; this is the same
// subprocess pattern as signaling_roundtrip_test.go.

package integration_test

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

var (
	sidecarBinaryOnce sync.Once
	sidecarBinaryPath string
	sidecarBinaryErr  error
)

// buildSidecarOnce compiles the sidecar binary on the first call;
// subsequent callers reuse the path. We build into a sibling tmp dir
// of the signaling binary so the test cleanup model stays uniform.
func buildSidecarOnce(t *testing.T) string {
	t.Helper()
	sidecarBinaryOnce.Do(func() {
		dir, err := os.MkdirTemp("", "chromeless-metrics-sidecar-")
		if err != nil {
			sidecarBinaryErr = err
			return
		}
		out := filepath.Join(dir, "chromeless-metrics-sidecar")
		build := exec.Command("go", "build", "-o", out, ".")
		build.Dir = filepath.FromSlash("../../capture/chromeless-metrics-sidecar")
		build.Stdout = os.Stderr
		build.Stderr = os.Stderr
		if err := build.Run(); err != nil {
			sidecarBinaryErr = fmt.Errorf("go build chromeless-metrics-sidecar: %w", err)
			return
		}
		sidecarBinaryPath = out
	})
	if sidecarBinaryErr != nil {
		t.Skipf("chromeless-metrics-sidecar build failed: %v", sidecarBinaryErr)
	}
	return sidecarBinaryPath
}

// startSidecar launches the sidecar on a free port pointing /proc at a
// fresh tempdir (so the proc reader finds nothing — we don't care
// about chromium CPU stats here, only /stats-update).
func startSidecar(t *testing.T) (port int) {
	t.Helper()
	bin := buildSidecarOnce(t)

	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("free port: %v", err)
	}
	port = l.Addr().(*net.TCPAddr).Port
	_ = l.Close()

	procDir := t.TempDir() // empty proc → no chromium-comm matches; cpu/rss stay at 0

	var stdout, stderr bytes.Buffer
	cmd := exec.Command(bin,
		"-listen", fmt.Sprintf("127.0.0.1:%d", port),
		"-proc-root", procDir,
		"-poll-interval", "1s",
		"-log-level", "warn",
	)
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start sidecar: %v", err)
	}

	// Wait for /healthz.
	deadline := time.Now().Add(5 * time.Second)
	healthURL := fmt.Sprintf("http://127.0.0.1:%d/healthz", port)
	for {
		resp, err := http.Get(healthURL)
		if err == nil {
			_ = resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				break
			}
		}
		if time.Now().After(deadline) {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
			t.Logf("--- sidecar stdout ---\n%s", stdout.String())
			t.Logf("--- sidecar stderr ---\n%s", stderr.String())
			t.Fatalf("sidecar healthz never came up")
		}
		time.Sleep(25 * time.Millisecond)
	}

	t.Cleanup(func() {
		_ = cmd.Process.Signal(syscall.SIGTERM)
		done := make(chan struct{})
		go func() { _ = cmd.Wait(); close(done) }()
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			_ = cmd.Process.Kill()
			<-done
		}
		if t.Failed() || os.Getenv("SIGNALING_TEST_LOGS") == "1" {
			t.Logf("--- sidecar stdout ---\n%s", stdout.String())
			t.Logf("--- sidecar stderr ---\n%s", stderr.String())
		}
	})
	return port
}

// scrapeMetric reads /metrics and returns the float value of a metric
// matching `metricExpr` — accepts either a bare metric name or a
// metric with a label-set selector (e.g.,
// `cb_client_inbound_video_fps{tenant_id="t",session_id="s"}`). The
// labels in the expr must appear in the same order as Prometheus
// emits them on the wire (alphabetical for *Vec types).
func scrapeMetric(t *testing.T, port int, metricExpr string) float64 {
	t.Helper()
	resp, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/metrics", port))
	if err != nil {
		t.Fatalf("scrape: %v", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body: %v", err)
	}
	re := regexp.MustCompile(fmt.Sprintf(`(?m)^%s (\S+)`, regexp.QuoteMeta(metricExpr)))
	m := re.FindSubmatch(body)
	if m == nil {
		t.Fatalf("metric %q not found in /metrics:\n%s", metricExpr, string(body))
	}
	v, err := strconv.ParseFloat(string(m[1]), 64)
	if err != nil {
		t.Fatalf("parse %q value %q: %v", metricExpr, m[1], err)
	}
	return v
}

func postStats(t *testing.T, port int, body string) int {
	t.Helper()
	resp, err := http.Post(
		fmt.Sprintf("http://127.0.0.1:%d/stats-update", port),
		"application/json",
		strings.NewReader(body),
	)
	if err != nil {
		t.Fatalf("POST /stats-update: %v", err)
	}
	defer resp.Body.Close()
	return resp.StatusCode
}

// TestStatsLoop: drive a fake "streamer relay" that POSTs two
// StatsSample envelopes 1s apart, then assert /metrics reflects
// FPS, RTT, loss-fraction, and a non-zero bitrate (delta-derived).
func TestStatsLoop(t *testing.T) {
	port := startSidecar(t)

	// T82 v1.1 envelope — session_id + tenant_id are top-level fields.
	first := `{"v":1,"t":1000,"session_id":"sess-1","tenant_id":"tenant-A","sample":{
		"v":1,"t":1000,
		"inbound":[{"trackId":"v","kind":"video","bytesReceived":0,"packetsReceived":0,"packetsLost":0,"jitter":0,"framesPerSecond":30,"framesDropped":0,"framesReceived":0,"totalDecodeTime":0}],
		"outbound":[],
		"remoteInbound":[{"trackId":"v","kind":"video","roundTripTime":0.05,"packetsLost":2,"fractionLost":0.01}],
		"candidatePair":{"currentRoundTripTime":0.062,"availableOutgoingBitrate":null,"availableIncomingBitrate":null,"localCandidateType":"host","remoteCandidateType":"srflx"}
	}}`
	if code := postStats(t, port, first); code != http.StatusNoContent {
		t.Fatalf("first POST status %d, want 204", code)
	}

	// Second sample at t=2000 with +250_000 bytes → 2_000_000 bps.
	second := `{"v":1,"t":2000,"session_id":"sess-1","tenant_id":"tenant-A","sample":{
		"v":1,"t":2000,
		"inbound":[{"trackId":"v","kind":"video","bytesReceived":250000,"packetsReceived":200,"packetsLost":4,"jitter":0,"framesPerSecond":29.5,"framesDropped":2,"framesReceived":59,"totalDecodeTime":0}],
		"outbound":[],
		"remoteInbound":[{"trackId":"v","kind":"video","roundTripTime":0.05,"packetsLost":4,"fractionLost":0.0125}],
		"candidatePair":{"currentRoundTripTime":0.05,"availableOutgoingBitrate":null,"availableIncomingBitrate":null,"localCandidateType":"host","remoteCandidateType":"srflx"}
	}}`
	if code := postStats(t, port, second); code != http.StatusNoContent {
		t.Fatalf("second POST status %d, want 204", code)
	}

	// Assertions: prom emits labels alphabetically, so the order is
	// {session_id, tenant_id}.
	const labels = `{session_id="sess-1",tenant_id="tenant-A"}`
	if got := scrapeMetric(t, port, "cb_client_inbound_video_fps"+labels); got != 29.5 {
		t.Errorf("cb_client_inbound_video_fps: got %v, want 29.5", got)
	}
	if got := scrapeMetric(t, port, "cb_client_pair_rtt_ms"+labels); got != 50 {
		t.Errorf("cb_client_pair_rtt_ms: got %v, want 50", got)
	}
	if got := scrapeMetric(t, port, "cb_client_remote_inbound_packet_loss_fraction"+labels); got != 0.0125 {
		t.Errorf("cb_client_remote_inbound_packet_loss_fraction: got %v, want 0.0125", got)
	}
	if got := scrapeMetric(t, port, "cb_client_inbound_video_bitrate_bps"+labels); got != 2_000_000 {
		t.Errorf("cb_client_inbound_video_bitrate_bps: got %v, want 2000000", got)
	}
	if got := scrapeMetric(t, port, "cb_client_inbound_video_frames_dropped_total"+labels); got < 2 {
		t.Errorf("cb_client_inbound_video_frames_dropped_total: got %v, want ≥ 2", got)
	}
}

// TestStatsLoop_RejectsMalformed: the sidecar must 400 on bad JSON.
func TestStatsLoop_RejectsMalformed(t *testing.T) {
	port := startSidecar(t)
	if code := postStats(t, port, "{not-json"); code != http.StatusBadRequest {
		t.Fatalf("malformed POST: got %d, want 400", code)
	}
}

// TestStatsLoop_AcceptsCodecFallbackEvent: T54 codec_fallback events
// ride the same channel; the sidecar must accept them as no-op.
func TestStatsLoop_AcceptsCodecFallbackEvent(t *testing.T) {
	port := startSidecar(t)
	body := `{"v":1,"t":1000,"event":"codec_fallback","data":{"preferred":["VP9"],"negotiated":"H264"}}`
	if code := postStats(t, port, body); code != http.StatusNoContent {
		t.Fatalf("codec_fallback POST: got %d, want 204", code)
	}
}

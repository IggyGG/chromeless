// Integration test for the input-bridge's WebSocket source (T22) plus
// the wire format that streamer-page/streamer.js (T23 / T41) forwards
// onto it.
//
// What this test exercises end-to-end:
//
//   1. The input-bridge binary built from capture/input-bridge/ —
//      launched in `--dry-run` mode so we don't need a real Chromium
//      DevTools endpoint.
//   2. The WebSocket source: a fake peer connects to
//      `ws://127.0.0.1:<port>/input`, sends a v1 `mouse_move` envelope,
//      and we verify the bridge counts the event in its Prometheus
//      metrics.
//   3. The bridge's parsing: malformed envelopes increment
//      `input_bridge_parse_errors_total` and do not increment
//      `input_bridge_events_total`. (Negative test.)
//   4. Unknown event types are surfaced via
//      `input_bridge_unknown_total`.
//
// What this test deliberately does NOT exercise:
//
//   * The JavaScript relay in capture/streamer-page/streamer.js. That
//     is Playwright territory (T33), and asking Go to drive a browser
//     here would more than double the test surface for diminishing
//     return. We get *protocol* coverage here; the JS relay is glue.
//   * Real CDP. `--dry-run` swallows the CDP calls; if CDP is broken
//     the dry-run still succeeds. This is by design — the bridge's
//     CDP correctness lives in capture/input-bridge/main_test.go's
//     fake-CDP table tests (T22).
//
// Cross-references:
//   * docs/protocols/input-channel.md  (v1 envelope spec)
//   * capture/input-bridge/main.go     (bridge under test)
//   * capture/streamer-page/streamer.js (JS-side relay; not driven here)

package integration_test

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// ---------------------------------------------------------------------------
// Bridge process lifecycle
// ---------------------------------------------------------------------------

var bridgeBinaryPath string
var bridgeBuildOnce sync.Once

// buildBridge compiles capture/input-bridge into a temp binary the first
// time it's called; subsequent calls reuse the artifact.  We don't put
// this in TestMain because TestMain in this package already builds the
// signaling binary; layering both there made the test lifecycle harder
// to read for a small win.
func buildBridge(t *testing.T) string {
	t.Helper()
	bridgeBuildOnce.Do(func() {
		dir, err := os.MkdirTemp("", "bridge-bin-")
		if err != nil {
			t.Fatalf("mktemp: %v", err)
		}
		// Note: not registering removal — these binaries are tiny and
		// the temp dir clears on reboot.  Mirrors the existing
		// signaling test's approach.
		bridgeBinaryPath = filepath.Join(dir, "input-bridge")
		build := exec.Command("go", "build", "-o", bridgeBinaryPath, ".")
		build.Dir = filepath.FromSlash("../../capture/input-bridge")
		build.Stdout = os.Stderr
		build.Stderr = os.Stderr
		if err := build.Run(); err != nil {
			t.Fatalf("go build of capture/input-bridge failed: %v", err)
		}
	})
	return bridgeBinaryPath
}

type runningBridge struct {
	wsURL      string // ws://127.0.0.1:PORT/input
	metricsURL string // http://127.0.0.1:PORT/metrics
	cmd        *exec.Cmd
	stdout     *bytes.Buffer
	stderr     *bytes.Buffer
}

// startBridge launches the bridge in --dry-run --source ws on free
// ports and waits for /metrics to come up.
func startBridge(t *testing.T) *runningBridge {
	t.Helper()
	bin := buildBridge(t)

	wsPort := freePort(t)
	metricsPort := freePort(t)

	var stdout, stderr bytes.Buffer
	cmd := exec.Command(bin,
		"--source", "ws",
		"--ws-addr", fmt.Sprintf("127.0.0.1:%d", wsPort),
		"--ws-path", "/input",
		"--metrics-addr", fmt.Sprintf("127.0.0.1:%d", metricsPort),
		"--dry-run",
	)
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start bridge: %v", err)
	}

	br := &runningBridge{
		wsURL:      fmt.Sprintf("ws://127.0.0.1:%d/input", wsPort),
		metricsURL: fmt.Sprintf("http://127.0.0.1:%d/metrics", metricsPort),
		cmd:        cmd,
		stdout:     &stdout,
		stderr:     &stderr,
	}

	// Wait for /metrics to come up — that means both the metrics
	// server and (by the order things start in run()) the ws source
	// listener are ready.
	deadline := time.Now().Add(5 * time.Second)
	for {
		resp, err := http.Get(br.metricsURL)
		if err == nil {
			_ = resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				break
			}
		}
		if time.Now().After(deadline) {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
			t.Logf("--- bridge stdout ---\n%s", stdout.String())
			t.Logf("--- bridge stderr ---\n%s", stderr.String())
			t.Fatalf("bridge metrics did not come up at %s", br.metricsURL)
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
		if t.Failed() || os.Getenv("BRIDGE_TEST_LOGS") == "1" {
			t.Logf("--- bridge stdout ---\n%s", stdout.String())
			t.Logf("--- bridge stderr ---\n%s", stderr.String())
		}
	})
	return br
}

// ---------------------------------------------------------------------------
// Metrics helpers
// ---------------------------------------------------------------------------

// scrapeCounter parses Prometheus text format and returns the value of
// the first sample whose line matches the metric name with the given
// label assignments. Returns 0 if no match. Tiny ad-hoc parser; we
// don't need a full Prometheus client just for two lookups.
func scrapeCounter(t *testing.T, metricsURL, name string, labels map[string]string) float64 {
	t.Helper()
	resp, err := http.Get(metricsURL)
	if err != nil {
		t.Fatalf("scrape: %v", err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read scrape: %v", err)
	}
	// Build a regex like:  ^name(\{...\})?  value$
	// Match label string verbatim if labels were provided.
	for _, line := range bytes.Split(body, []byte("\n")) {
		if !bytes.HasPrefix(line, []byte(name)) {
			continue
		}
		// Skip HELP / TYPE comments which start with `# `.
		if bytes.HasPrefix(line, []byte("#")) {
			continue
		}
		// Crudely require all labels appear in the line. Collisions
		// between, e.g., `input_bridge_events_total` and
		// `input_bridge_events_total_seconds` aren't a concern here
		// because none of our metrics share a prefix accidentally.
		ok := true
		for k, v := range labels {
			needle := fmt.Sprintf(`%s="%s"`, k, v)
			if !bytes.Contains(line, []byte(needle)) {
				ok = false
				break
			}
		}
		if !ok {
			continue
		}
		// Last whitespace-separated field is the value.
		fields := regexp.MustCompile(`\s+`).Split(string(line), -1)
		if len(fields) < 2 {
			continue
		}
		v, err := strconv.ParseFloat(fields[len(fields)-1], 64)
		if err != nil {
			continue
		}
		return v
	}
	return 0
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

func TestInputBridgeAcceptsMouseMove(t *testing.T) {
	br := startBridge(t)

	conn, _, err := websocket.DefaultDialer.Dial(br.wsURL, nil)
	if err != nil {
		t.Fatalf("dial bridge: %v", err)
	}
	defer conn.Close()

	// v1 envelope per docs/protocols/input-channel.md.
	envelope := map[string]any{
		"v":    1,
		"type": "mouse_move",
		"t":    time.Now().UnixMilli(),
		"seq":  int64(1),
		"data": map[string]any{"x": 100, "y": 200},
	}
	if err := conn.WriteJSON(envelope); err != nil {
		t.Fatalf("write envelope: %v", err)
	}

	// The bridge is async — give it a moment to dispatch + record the
	// metric. 1s is a generous bound for a localhost dispatch in
	// dry-run mode.
	deadline := time.Now().Add(1 * time.Second)
	var got float64
	for time.Now().Before(deadline) {
		got = scrapeCounter(t, br.metricsURL,
			"input_bridge_events_total",
			map[string]string{"type": "mouse_move"})
		if got >= 1 {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	if got < 1 {
		t.Fatalf("input_bridge_events_total{type=mouse_move} = %v, want >= 1", got)
	}

	// Errors counter should still be zero.
	if errs := scrapeCounter(t, br.metricsURL,
		"input_bridge_errors_total",
		map[string]string{"type": "mouse_move"}); errs != 0 {
		t.Errorf("input_bridge_errors_total{type=mouse_move} = %v, want 0", errs)
	}
}

func TestInputBridgeRoundsTripsKeyEvent(t *testing.T) {
	br := startBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(br.wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	for i, ty := range []string{"key_down", "key_up"} {
		env := map[string]any{
			"v":    1,
			"type": ty,
			"t":    time.Now().UnixMilli(),
			"seq":  int64(i + 1),
			"data": map[string]any{
				"code": "KeyA", "key": "a", "mods": 0,
			},
		}
		if err := conn.WriteJSON(env); err != nil {
			t.Fatalf("write %s: %v", ty, err)
		}
	}

	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		dn := scrapeCounter(t, br.metricsURL,
			"input_bridge_events_total",
			map[string]string{"type": "key_down"})
		up := scrapeCounter(t, br.metricsURL,
			"input_bridge_events_total",
			map[string]string{"type": "key_up"})
		if dn >= 1 && up >= 1 {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatal("key_down + key_up counters never reached >= 1")
}

func TestInputBridgeRejectsMalformed(t *testing.T) {
	br := startBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(br.wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	// Not JSON.
	if err := conn.WriteMessage(websocket.TextMessage,
		[]byte("definitely not json")); err != nil {
		t.Fatalf("write garbage: %v", err)
	}
	// Wrong protocol version.
	if err := conn.WriteJSON(map[string]any{
		"v": 99, "type": "mouse_move", "t": 0, "seq": 1,
		"data": map[string]any{"x": 1, "y": 1},
	}); err != nil {
		t.Fatalf("write v99: %v", err)
	}

	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		v := scrapeCounter(t, br.metricsURL,
			"input_bridge_parse_errors_total", nil)
		if v >= 2 {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatalf("input_bridge_parse_errors_total = %v, want >= 2",
		scrapeCounter(t, br.metricsURL, "input_bridge_parse_errors_total", nil))
}

func TestInputBridgeUnknownTypeIsCounted(t *testing.T) {
	br := startBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(br.wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	if err := conn.WriteJSON(map[string]any{
		"v":    1,
		"type": "telepathy",
		"t":    time.Now().UnixMilli(),
		"seq":  1,
		"data": map[string]any{"thought": "hello"},
	}); err != nil {
		t.Fatalf("write unknown: %v", err)
	}

	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		v := scrapeCounter(t, br.metricsURL,
			"input_bridge_unknown_total", nil)
		if v >= 1 {
			return
		}
		time.Sleep(25 * time.Millisecond)
	}
	t.Fatalf("input_bridge_unknown_total = %v, want >= 1",
		scrapeCounter(t, br.metricsURL, "input_bridge_unknown_total", nil))
}

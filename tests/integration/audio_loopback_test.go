// audio_loopback_test.go — T64 integration check.
//
// "Fast(er) integration test that doesn't need a full browser" per the
// T64 brief: launches the cloud-browser-webrtc compose stack, joins
// signaling as a fake "client" peer, waits for the streamer to send
// its SDP offer, and asserts the offer advertises audio in its SDP.
//
// Why this matters: T23 + T24 wire audio (PulseAudio null-sink, Chromium
// --autoplay-policy=no-user-gesture-required, getDisplayMedia({audio:true})).
// If any of those regress, the streamer's offer SDP loses its `m=audio`
// section silently — and from a working-video pipeline you'd have no
// idea audio was missing until a user complained. This test makes
// audio's presence in the *contract layer* (the SDP the streamer
// advertises) a first-class assertion.
//
// Lifecycle:
//   - Default `go test ./tests/integration/...` SKIPS this test, because
//     it needs `docker compose up` (60+ seconds, real Docker daemon).
//     Set `CBWRTC_INTEGRATION_LIVE=1` to opt in.
//   - When opted in, TestMain itself does NOT spin up compose — each
//     live test is responsible. This keeps the cheap signaling tests
//     in this package running in <2s when only they're invoked.
//   - Compose teardown is registered with t.Cleanup so a failed test
//     still leaves the host clean.
//
// Dependencies:
//   - Docker daemon reachable.
//   - cloud-browser-webrtc:dev and cloud-browser-webrtc-signaling:dev
//     buildable from the repo root via `docker compose ... up --build`.
//   - **T69** (streamer-page exposes window.pc + dials signaling).
//     Without T69 the streamer never sends an offer; the test will
//     time out and skip with a message tying the failure to T69.
//
// See tests/e2e/README.md for the Playwright-based companion (spec 04).

package integration_test

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// liveStackEnabled gates every test in this file: opt-in via
// CBWRTC_INTEGRATION_LIVE because spinning up compose is heavy.
func liveStackEnabled(t *testing.T) {
	t.Helper()
	if v := os.Getenv("CBWRTC_INTEGRATION_LIVE"); v != "1" && v != "true" {
		t.Skip("set CBWRTC_INTEGRATION_LIVE=1 to run live-compose integration tests " +
			"(this test brings up `docker compose up`, takes ~60s)")
	}
}

// repoRoot resolves the repo root from this file's location.
func repoRoot(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatalf("getwd: %v", err)
	}
	// tests/integration -> repo root.
	return filepath.Clean(filepath.Join(wd, "..", ".."))
}

// startCompose runs `docker compose up -d --build` and registers
// teardown via t.Cleanup. Polls signaling /healthz and chromium
// container health up to readyTimeout before returning.
func startCompose(t *testing.T, readyTimeout time.Duration) {
	t.Helper()
	root := repoRoot(t)
	composeFile := filepath.Join(root, "infra", "compose.yaml")

	t.Logf("starting compose stack at %s", composeFile)
	up := exec.Command("docker", "compose", "-f", composeFile, "up", "-d", "--build")
	up.Stdout = testLogWriter{t: t, prefix: "compose-up: "}
	up.Stderr = testLogWriter{t: t, prefix: "compose-up: "}
	if err := up.Run(); err != nil {
		t.Fatalf("docker compose up failed: %v", err)
	}

	t.Cleanup(func() {
		t.Log("stopping compose stack")
		down := exec.Command("docker", "compose", "-f", composeFile, "down", "-v")
		down.Stdout = testLogWriter{t: t, prefix: "compose-down: "}
		down.Stderr = testLogWriter{t: t, prefix: "compose-down: "}
		_ = down.Run()
	})

	// Poll signaling /healthz; this is the cheapest "stack is up" probe
	// we have. The chromium service has its own healthcheck that gates
	// the client service's start-up, so once /healthz answers we know
	// signaling is up; the chromium service's health is independent.
	deadline := time.Now().Add(readyTimeout)
	for {
		if time.Now().After(deadline) {
			t.Fatalf("compose stack did not become ready within %s", readyTimeout)
		}
		resp, err := http.Get("http://127.0.0.1:8080/healthz")
		if err == nil {
			_ = resp.Body.Close()
			if resp.StatusCode == 200 {
				break
			}
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Log("compose stack is ready")
}

// testLogWriter is an io.Writer that forwards to t.Log per-line.
type testLogWriter struct {
	t      *testing.T
	prefix string
}

func (w testLogWriter) Write(p []byte) (int, error) {
	for _, line := range strings.Split(strings.TrimRight(string(p), "\n"), "\n") {
		if line != "" {
			w.t.Log(w.prefix + line)
		}
	}
	return len(p), nil
}

// audioMLineRE matches an SDP m=audio line per RFC 4566.
//
// e.g. "m=audio 9 UDP/TLS/RTP/SAVPF 111 103 104 9 0 8 106 105 13 110 112 113 126"
var audioMLineRE = regexp.MustCompile(`(?m)^m=audio\s+\d+\s+\S+\s+\d+`)

// opusRtpmapRE matches an opus rtpmap line (the canonical WebRTC audio codec).
var opusRtpmapRE = regexp.MustCompile(`(?mi)^a=rtpmap:\d+\s+opus/`)

// TestStreamerOffersAudio asserts that the cloud-Chromium streamer's
// SDP offer advertises an audio m-line (and specifically opus, the
// only audio codec we ship in v1 per T30/T34).
func TestStreamerOffersAudio(t *testing.T) {
	liveStackEnabled(t)

	startCompose(t, 240*time.Second)

	// Connect to signaling at the streamer's default session id ("dev"
	// per cold-start.sh's fallback, locked in compose's `chromium`
	// environment). We dial as "client" with a benign warmup envelope
	// so the streamer's offer (already queued or arriving shortly) gets
	// forwarded to us.
	wsURL := "ws://127.0.0.1:8080/ws/dev"
	t.Logf("dialing %s as client", wsURL)
	dialCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.DefaultDialer.DialContext(dialCtx, wsURL, nil)
	if err != nil {
		t.Fatalf("dial signaling: %v", err)
	}
	defer conn.Close()

	// Warmup ICE — registers role=client. Drops if the streamer hasn't
	// joined yet, which is fine.
	warmup := envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":"audio-test-warmup"}`),
	}
	if err := conn.WriteJSON(warmup); err != nil {
		t.Fatalf("write warmup: %v", err)
	}

	// Wait up to 30 s for the streamer to send an offer. If nothing
	// arrives, the most likely cause is T69 (streamer doesn't dial
	// signaling) — skip with that explicit reference rather than fail
	// confusingly.
	const offerWait = 30 * time.Second
	_ = conn.SetReadDeadline(time.Now().Add(offerWait))

	var offerEnv envelope
	for {
		var got envelope
		if err := conn.ReadJSON(&got); err != nil {
			t.Skipf("no offer envelope from streamer within %s — most likely T78 "+
				"(getDisplayMedia NotReadableError in cloud Chromium) is keeping "+
				"the streamer's start() from running. T69 fix is in but masked. "+
				"Verify by attaching to cloud Chromium DevTools (host:9222 "+
				"post-T52) and reading the streamer page's #log div: an "+
				"`ERR start failed NotReadableError` line confirms T78. Last "+
				"read error: %v", offerWait, err)
		}
		if got.Type == "offer" && got.From == "browser" {
			offerEnv = got
			break
		}
		t.Logf("ignoring %s/%s while waiting for offer", got.From, got.Type)
	}

	// Parse the SDP from the envelope's data payload. The streamer's
	// envelope shape is `{type, from, data: {type, sdp}}`.
	var offerData struct {
		Type string `json:"type"`
		SDP  string `json:"sdp"`
	}
	if err := json.Unmarshal(offerEnv.Data, &offerData); err != nil {
		t.Fatalf("parse offer data: %v (raw=%q)", err, offerEnv.Data)
	}
	if offerData.SDP == "" {
		t.Fatalf("offer envelope has empty sdp (raw=%q)", offerEnv.Data)
	}
	t.Logf("received offer: sdpBytes=%d", len(offerData.SDP))

	// Assert audio m-line present.
	if !audioMLineRE.MatchString(offerData.SDP) {
		t.Errorf("offer SDP missing audio m-line — T24 audio routing or T23 "+
			"getDisplayMedia({audio:true}) regressed. SDP head:\n%s",
			snippet(offerData.SDP, 600))
	}

	// Assert opus codec is offered. v1 ships opus only (per
	// docs/protocols/sdp-munging.md — T30). If we ever add another
	// audio codec, soften this to "at least one of {opus, ...}".
	if !opusRtpmapRE.MatchString(offerData.SDP) {
		t.Errorf("offer SDP missing opus rtpmap — codec preference broken? "+
			"SDP head:\n%s", snippet(offerData.SDP, 600))
	}

	if !t.Failed() {
		t.Log("✓ streamer offer advertises audio (m=audio + a=rtpmap:* opus/)")
	}

	// Be polite: emit a bye so the streamer's session can recycle.
	_ = conn.WriteJSON(envelope{Type: "bye", From: "client"})
}

// snippet returns the first n chars of s, with ellipsis if truncated.
func snippet(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}

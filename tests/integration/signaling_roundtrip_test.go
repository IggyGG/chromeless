// Package integration exercises the v0 signaling server (T13) end-to-end.
//
// We build the signaling binary once in TestMain and launch it as a
// subprocess on a free port for each test. This is the cheapest faithful
// integration check: the protocol contract under test is the wire
// behaviour of the actual artifact, not a re-implementation.
//
// Contract under test (per signaling/server.go):
//   - Path: /ws/{session_id}
//   - First frame establishes peer role via Envelope.From in {"client","browser"}
//   - Forwarded message types: offer, answer, ice, bye
//   - Session holds at most one peer per role; duplicates get
//     ClosePolicyViolation (1008)
//   - "bye" is forwarded; the sender's connection STAYS open (a bye ends the
//     session, not the socket — the re-armable worker offers again on it)
//   - A peer joining an empty session is sent a `peer_absent` advisory
//     (readEnvelope skips it; see there)
//   - Session is dropped from the hub once both peers have left
package integration_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// envelope mirrors signaling/server.go's wire format.
type envelope struct {
	Type string          `json:"type"`
	From string          `json:"from"`
	Data json.RawMessage `json:"data,omitempty"`
}

// ----- TestMain: build the signaling binary once. -----

var binaryPath string

func TestMain(m *testing.M) {
	tmpDir, err := os.MkdirTemp("", "signaling-bin-")
	if err != nil {
		fmt.Fprintf(os.Stderr, "mktemp: %v\n", err)
		os.Exit(2)
	}
	defer os.RemoveAll(tmpDir)

	binaryPath = filepath.Join(tmpDir, "signaling")
	// The signaling/ directory is its own Go module, so we have to build
	// from inside that module — we can't reach across module boundaries
	// from this test module's view.
	build := exec.Command("go", "build", "-o", binaryPath, ".")
	build.Dir = filepath.FromSlash("../../signaling")
	build.Stdout = os.Stderr
	build.Stderr = os.Stderr
	if err := build.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "go build of signaling/ failed: %v\n", err)
		os.Exit(2)
	}

	os.Exit(m.Run())
}

// ----- helpers -----

// freePort grabs an available TCP port from the kernel and immediately
// closes the listener. There's a tiny race window before the signaling
// server claims it; in practice 0% flake locally.
func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("free port: %v", err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	_ = l.Close()
	return port
}

// startServer launches the signaling binary on a free port and waits for
// /healthz to come up. Server logs are captured and printed via t.Logf
// only if the test fails (or SIGNALING_TEST_LOGS=1 is set), so passing
// runs stay quiet. Cleanup is registered with t.Cleanup.
func startServer(t *testing.T) int {
	t.Helper()
	port := freePort(t)

	var stdout, stderr bytes.Buffer
	cmd := exec.Command(binaryPath)
	cmd.Env = append(os.Environ(), fmt.Sprintf("SIGNALING_PORT=%d", port))
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	if err := cmd.Start(); err != nil {
		t.Fatalf("start signaling: %v", err)
	}

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
			t.Logf("--- signaling stdout ---\n%s", stdout.String())
			t.Logf("--- signaling stderr ---\n%s", stderr.String())
			t.Fatalf("server failed to come up at %s", healthURL)
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
			t.Logf("--- signaling stdout ---\n%s", stdout.String())
			t.Logf("--- signaling stderr ---\n%s", stderr.String())
		}
	})

	return port
}

// dialPeer connects as `role` for `sessionID` and writes the registration
// envelope. The first frame both establishes the peer role and (per T13)
// is forwarded to the other peer if one is already registered.
func dialPeer(t *testing.T, port int, sessionID string, reg envelope) *websocket.Conn {
	t.Helper()
	url := fmt.Sprintf("ws://127.0.0.1:%d/ws/%s", port, sessionID)
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		t.Fatalf("dial as %s: %v", reg.From, err)
	}
	if err := conn.WriteJSON(reg); err != nil {
		_ = conn.Close()
		t.Fatalf("write reg as %s: %v", reg.From, err)
	}
	return conn
}

// readEnvelope reads one envelope with a deadline.
//
// `peer_absent` advisories are skipped. The broker sends one to a peer that
// joins an EMPTY session (signaling/server.go: it tells a lone client that no
// worker is there yet, so a misconfigured stack fails loudly instead of
// "waiting for offer" forever), so every test that connects the client first
// gets it as its first frame. It carries no session state and is not part of
// the offer/answer/ice/bye contract these tests pin. When it landed, five
// tests in this module went red with `want offer/browser, got peer_absent` —
// and stayed red, because ci.yml excludes tests/integration from its go-test
// loop (`make verify` is the only thing that runs it). A test that wants to
// see the advisory reads the raw frame itself.
func readEnvelope(t *testing.T, conn *websocket.Conn, timeout time.Duration) envelope {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		_ = conn.SetReadDeadline(deadline)
		var got envelope
		if err := conn.ReadJSON(&got); err != nil {
			t.Fatalf("read envelope: %v", err)
		}
		if got.Type == "peer_absent" {
			continue
		}
		return got
	}
}

func writeEnvelope(t *testing.T, conn *websocket.Conn, env envelope) {
	t.Helper()
	if err := conn.WriteJSON(env); err != nil {
		t.Fatalf("write %s/%s: %v", env.From, env.Type, err)
	}
}

// expectClose reads from conn and asserts the server closed it with `code`.
func expectClose(t *testing.T, conn *websocket.Conn, code int, timeout time.Duration) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(timeout))
	_, _, err := conn.ReadMessage()
	if err == nil {
		t.Fatalf("expected close %d, got message", code)
	}
	if !websocket.IsCloseError(err, code) {
		t.Fatalf("expected close %d, got %v", code, err)
	}
}

// expectClosed reads from conn and asserts the connection has been closed
// (any close code or read failure satisfies it). Use when the protocol
// allows the server to close abruptly without a specific code.
func expectClosed(t *testing.T, conn *websocket.Conn, timeout time.Duration) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(timeout))
	if _, _, err := conn.ReadMessage(); err == nil {
		t.Fatal("expected connection closed, got message")
	}
}

// drain reads exactly n envelopes from conn, giving each individual
// read up to perFrame and the whole batch up to total. Used for ICE
// trickle assertions where TCP+WebSocket guarantees per-direction
// ordering but a saturated CI host can stall any one frame.
//
// Returns the envelopes in the order received.
func drain(t *testing.T, conn *websocket.Conn, n int, perFrame, total time.Duration) []envelope {
	t.Helper()
	hardStop := time.Now().Add(total)
	out := make([]envelope, 0, n)
	for len(out) < n {
		// Whichever is sooner: per-frame deadline or the batch deadline.
		dl := time.Now().Add(perFrame)
		if hardStop.Before(dl) {
			dl = hardStop
		}
		_ = conn.SetReadDeadline(dl)
		var got envelope
		if err := conn.ReadJSON(&got); err != nil {
			t.Fatalf("drain @ %d/%d: %v", len(out), n, err)
		}
		out = append(out, got)
	}
	return out
}

// ----- tests -----

// TestSignalingRoundtrip exercises the full SDP + ICE forwarding contract
// in both directions on a single session.
func TestSignalingRoundtrip(t *testing.T) {
	port := startServer(t)

	sessID := "rt-roundtrip"

	// Client joins first with a benign warmup frame. No browser yet, so
	// the server drops the warmup; the client is registered.
	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	defer cli.Close()

	// Browser joins second. Its registration frame *is* the SDP offer;
	// it should be forwarded to the now-registered client.
	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-1"}`),
	})
	defer brw.Close()

	// 1. SDP offer reaches the client.
	got := readEnvelope(t, cli, 2*time.Second)
	if got.Type != "offer" || got.From != "browser" {
		t.Fatalf("offer envelope: want offer/browser, got %+v", got)
	}
	if !bytes.Contains(got.Data, []byte(`"OFFER-1"`)) {
		t.Fatalf("offer payload: %s", got.Data)
	}

	// 2. SDP answer reaches the browser.
	writeEnvelope(t, cli, envelope{
		Type: "answer", From: "client",
		Data: json.RawMessage(`{"sdp":"ANSWER-1"}`),
	})
	got = readEnvelope(t, brw, 2*time.Second)
	if got.Type != "answer" || got.From != "client" {
		t.Fatalf("answer envelope: want answer/client, got %+v", got)
	}
	if !bytes.Contains(got.Data, []byte(`"ANSWER-1"`)) {
		t.Fatalf("answer payload: %s", got.Data)
	}

	// 3. Three ICE candidates browser→client, asserting per-direction
	//    order. TCP+WebSocket preserves order on a single sender, so
	//    the answer the cli already wrote can never interleave with
	//    these. Original test (pre-T51) used a 2s deadline per frame;
	//    we drain all 3 with a single batch budget. See README.md
	//    note on T51.
	for i := 1; i <= 3; i++ {
		writeEnvelope(t, brw, envelope{
			Type: "ice", From: "browser",
			Data: json.RawMessage(fmt.Sprintf(`{"candidate":"b-%d"}`, i)),
		})
	}
	got3 := drain(t, cli, 3, 4*time.Second, 8*time.Second)
	for i, e := range got3 {
		if e.Type != "ice" || e.From != "browser" {
			t.Fatalf("ice b-* envelope at slot %d: %+v", i, e)
		}
		want := fmt.Sprintf(`"b-%d"`, i+1)
		if !bytes.Contains(e.Data, []byte(want)) {
			t.Fatalf("ice ordering b→c: want %s at slot %d, got %s", want, i, e.Data)
		}
	}

	// 4. Three ICE candidates client→browser, in order.
	for i := 1; i <= 3; i++ {
		writeEnvelope(t, cli, envelope{
			Type: "ice", From: "client",
			Data: json.RawMessage(fmt.Sprintf(`{"candidate":"c-%d"}`, i)),
		})
	}
	got3 = drain(t, brw, 3, 4*time.Second, 8*time.Second)
	for i, e := range got3 {
		if e.Type != "ice" || e.From != "client" {
			t.Fatalf("ice c-* envelope at slot %d: %+v", i, e)
		}
		want := fmt.Sprintf(`"c-%d"`, i+1)
		if !bytes.Contains(e.Data, []byte(want)) {
			t.Fatalf("ice ordering c→b: want %s at slot %d, got %s", want, i, e.Data)
		}
	}
}

// TestDuplicateRoleRejected asserts that a third connection trying to
// register as an already-occupied role gets ClosePolicyViolation (1008).
func TestDuplicateRoleRejected(t *testing.T) {
	port := startServer(t)

	sessID := "rt-dup"

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	defer cli.Close()

	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER"}`),
	})
	defer brw.Close()

	// Drain the offer that the browser's registration sent over.
	_ = readEnvelope(t, cli, 2*time.Second)

	// Third connection tries to claim "client" — should be rejected.
	dup := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "client",
		Data: json.RawMessage(`{"sdp":"DUP"}`),
	})
	defer dup.Close()

	expectClose(t, dup, websocket.ClosePolicyViolation, 2*time.Second)

	// And the legitimate client did NOT receive the duplicate's frame.
	_ = cli.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
	if _, _, err := cli.ReadMessage(); err == nil {
		t.Fatal("legitimate client received a stray frame from rejected duplicate")
	}
}

// TestByePropagationAndTeardown asserts that "bye" is forwarded to the
// other peer, that the sender's connection STAYS open and can carry the next
// offer (a bye ends the session, not the socket — the re-armable worker sends
// a bye for a dead session and offers again on the same socket; the broker
// closing that socket recycled the guest, live 2026-09-07), and that after
// both peers close their own sockets the session_id can be reused cleanly.
func TestByePropagationAndTeardown(t *testing.T) {
	port := startServer(t)

	sessID := "rt-bye"

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER"}`),
	})

	// Drain the offer.
	_ = readEnvelope(t, cli, 2*time.Second)

	// Browser says bye.
	writeEnvelope(t, brw, envelope{Type: "bye", From: "browser"})

	// Client receives bye.
	got := readEnvelope(t, cli, 2*time.Second)
	if got.Type != "bye" || got.From != "browser" {
		t.Fatalf("bye envelope: want bye/browser, got %+v", got)
	}

	// The bye-sender's connection is still live in BOTH directions: its next
	// offer, on the same socket, reaches the still-connected client. With the
	// old readPump `return` the server had stopped reading this socket, so
	// the write below "succeeded" into a closing connection and the client
	// never received OFFER-1b.
	writeEnvelope(t, brw, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-1b"}`),
	})
	got = readEnvelope(t, cli, 2*time.Second)
	if got.Type != "offer" || !bytes.Contains(got.Data, []byte(`"OFFER-1b"`)) {
		t.Fatalf("offer after the sender's own bye: want offer OFFER-1b, got %+v", got)
	}
	// A leaving peer closes its own socket.
	_ = brw.Close()

	// Disconnect the client too. Now both peers have left; the session
	// must be eligible for cleanup.
	_ = cli.Close()

	// Allow the server's read pumps to wind down + dropIfEmpty to fire.
	time.Sleep(100 * time.Millisecond)

	// Reusing the same session_id must work cleanly — no stale role
	// registrations, no leftover state.
	cli2 := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	defer cli2.Close()
	brw2 := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-2"}`),
	})
	defer brw2.Close()

	got = readEnvelope(t, cli2, 2*time.Second)
	if got.Type != "offer" || !bytes.Contains(got.Data, []byte(`"OFFER-2"`)) {
		t.Fatalf("post-teardown offer: %+v", got)
	}
}

// TestSessionIsolation confirms that two simultaneous sessions on the same
// server do not leak messages between each other.
func TestSessionIsolation(t *testing.T) {
	port := startServer(t)

	type pair struct {
		cli, brw *websocket.Conn
	}

	mkPair := func(sess string, marker string) pair {
		cli := dialPeer(t, port, sess, envelope{
			Type: "ice", From: "client",
			Data: nil,
		})
		brw := dialPeer(t, port, sess, envelope{
			Type: "offer", From: "browser",
			Data: json.RawMessage(fmt.Sprintf(`{"sdp":"%s"}`, marker)),
		})
		// Drain the offer.
		got := readEnvelope(t, cli, 2*time.Second)
		if !bytes.Contains(got.Data, []byte(marker)) {
			t.Fatalf("session %s: warmup offer payload mismatch: %s", sess, got.Data)
		}
		return pair{cli: cli, brw: brw}
	}

	a := mkPair("rt-iso-A", "OFFER-A")
	defer a.cli.Close()
	defer a.brw.Close()

	b := mkPair("rt-iso-B", "OFFER-B")
	defer b.cli.Close()
	defer b.brw.Close()

	// Have each session do an answer round-trip in parallel; assert the
	// payload received on each browser matches its own session marker.
	var wg sync.WaitGroup
	check := func(p pair, marker string, name string) {
		defer wg.Done()
		writeEnvelope(t, p.cli, envelope{
			Type: "answer", From: "client",
			Data: json.RawMessage(fmt.Sprintf(`{"sdp":"%s"}`, marker)),
		})
		got := readEnvelope(t, p.brw, 2*time.Second)
		if got.Type != "answer" {
			t.Errorf("session %s: bad type: %+v", name, got)
			return
		}
		if !bytes.Contains(got.Data, []byte(marker)) {
			t.Errorf("session %s: cross-talk! payload %s, expected %s", name, got.Data, marker)
		}
	}
	wg.Add(2)
	go check(a, "ANSWER-A", "A")
	go check(b, "ANSWER-B", "B")
	wg.Wait()

	// Final cross-check: B's browser must NOT have seen A's marker, and
	// vice versa. Above check confirms received payload contains the
	// expected marker; here we additionally ensure no extra frame is
	// queued on either side (i.e. no stray cross-talk).
	for name, conn := range map[string]*websocket.Conn{"A.cli": a.cli, "A.brw": a.brw, "B.cli": b.cli, "B.brw": b.brw} {
		_ = conn.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
		if _, _, err := conn.ReadMessage(); err == nil {
			t.Errorf("%s: unexpected stray frame after isolation check", name)
		}
	}
}

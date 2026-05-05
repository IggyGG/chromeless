// T67 cross-tenant isolation: two tenants both running session "demo"
// must NOT cross-talk. Reuses the binary built in TestMain; spawns a
// fresh server with the dev issuer activated so we have a working
// auth path to drive tenant claims through.

package integration_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// startServerWithDevIssuer launches the signaling binary with
// CHROMELESS_DEV_ISSUER=1, which activates the in-process /issue-token
// endpoint and enables auth verification using a freshly-generated
// Ed25519 keypair.
func startServerWithDevIssuer(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("free port: %v", err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	_ = l.Close()

	var stdout, stderr bytes.Buffer
	cmd := exec.Command(binaryPath)
	// Strip any inherited CHROMELESS_AUTH_PUBKEY so the dev issuer doesn't
	// refuse to start.
	env := []string{}
	for _, kv := range os.Environ() {
		if strings.HasPrefix(kv, "CHROMELESS_AUTH_PUBKEY=") {
			continue
		}
		env = append(env, kv)
	}
	env = append(env, fmt.Sprintf("SIGNALING_PORT=%d", port), "CHROMELESS_DEV_ISSUER=1")
	cmd.Env = env
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start signaling: %v", err)
	}

	healthURL := fmt.Sprintf("http://127.0.0.1:%d/healthz", port)
	deadline := time.Now().Add(5 * time.Second)
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
			t.Logf("--- stdout ---\n%s", stdout.String())
			t.Logf("--- stderr ---\n%s", stderr.String())
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
			t.Logf("--- stdout ---\n%s", stdout.String())
			t.Logf("--- stderr ---\n%s", stderr.String())
		}
	})

	return port
}

// issueToken hits /issue-token on the dev server and returns the
// minted JWT.
func issueToken(t *testing.T, port int, tenant, sessionID, role string) string {
	t.Helper()
	q := url.Values{}
	q.Set("role", role)
	q.Set("session_id", sessionID)
	q.Set("tenant", tenant)
	u := fmt.Sprintf("http://127.0.0.1:%d/issue-token?%s", port, q.Encode())
	resp, err := http.Get(u)
	if err != nil {
		t.Fatalf("issue-token GET: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		t.Fatalf("issue-token status %d: %s", resp.StatusCode, body)
	}
	var out struct {
		Token string `json:"token"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		t.Fatalf("decode issue-token: %v", err)
	}
	if out.Token == "" {
		t.Fatal("empty token")
	}
	return out.Token
}

// dialAuthed connects with a token in the query string and writes the
// registration envelope.
func dialAuthed(t *testing.T, port int, sessionID, token string, reg envelope) *websocket.Conn {
	t.Helper()
	u := fmt.Sprintf("ws://127.0.0.1:%d/ws/%s?token=%s", port, sessionID, url.QueryEscape(token))
	conn, _, err := websocket.DefaultDialer.Dial(u, nil)
	if err != nil {
		t.Fatalf("dial as %s: %v", reg.From, err)
	}
	if err := conn.WriteJSON(reg); err != nil {
		_ = conn.Close()
		t.Fatalf("write reg as %s: %v", reg.From, err)
	}
	return conn
}

// TestCrossTenantIsolation: two tenants both running session_id "demo"
// must not see each other's frames.
func TestCrossTenantIsolation(t *testing.T) {
	port := startServerWithDevIssuer(t)
	const sessID = "demo"

	tokA := func(role string) string { return issueToken(t, port, "tenant-A", sessID, role) }
	tokB := func(role string) string { return issueToken(t, port, "tenant-B", sessID, role) }

	// Set up tenant-A pair.
	aCli := dialAuthed(t, port, sessID, tokA("client"), envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	defer aCli.Close()
	aBrw := dialAuthed(t, port, sessID, tokA("browser"), envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-A"}`),
	})
	defer aBrw.Close()

	// Tenant-A's client must receive A's offer.
	got := readEnvelope(t, aCli, 2*time.Second)
	if got.Type != "offer" || !bytes.Contains(got.Data, []byte(`"OFFER-A"`)) {
		t.Fatalf("A.cli expected OFFER-A, got %+v", got)
	}

	// Now bring up tenant-B with the SAME session_id.
	bCli := dialAuthed(t, port, sessID, tokB("client"), envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	defer bCli.Close()
	bBrw := dialAuthed(t, port, sessID, tokB("browser"), envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-B"}`),
	})
	defer bBrw.Close()

	got = readEnvelope(t, bCli, 2*time.Second)
	if got.Type != "offer" || !bytes.Contains(got.Data, []byte(`"OFFER-B"`)) {
		t.Fatalf("B.cli expected OFFER-B, got %+v", got)
	}

	// Now run a parallel answer round-trip on each tenant and assert
	// each browser only sees its own answer.
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		writeEnvelope(t, aCli, envelope{Type: "answer", From: "client",
			Data: json.RawMessage(`{"sdp":"ANSWER-A"}`)})
		got := readEnvelope(t, aBrw, 2*time.Second)
		if got.Type != "answer" || !bytes.Contains(got.Data, []byte(`"ANSWER-A"`)) {
			t.Errorf("A.brw expected ANSWER-A, got %+v", got)
		}
	}()
	go func() {
		defer wg.Done()
		writeEnvelope(t, bCli, envelope{Type: "answer", From: "client",
			Data: json.RawMessage(`{"sdp":"ANSWER-B"}`)})
		got := readEnvelope(t, bBrw, 2*time.Second)
		if got.Type != "answer" || !bytes.Contains(got.Data, []byte(`"ANSWER-B"`)) {
			t.Errorf("B.brw expected ANSWER-B, got %+v", got)
		}
	}()
	wg.Wait()

	// Cross-talk check: no peer should have any extra frame queued.
	for name, conn := range map[string]*websocket.Conn{
		"A.cli": aCli, "A.brw": aBrw, "B.cli": bCli, "B.brw": bBrw,
	} {
		_ = conn.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
		if _, raw, err := conn.ReadMessage(); err == nil {
			t.Errorf("%s: unexpected stray frame: %s", name, raw)
		}
	}
}

// TestSameTenantSameSessionStillWorks: nominally redundant with
// TestSignalingRoundtrip but explicitly drives the auth path so we
// catch any tenant-keying regression that wouldn't show up in the
// auth-disabled suite.
func TestSameTenantSameSessionStillWorks(t *testing.T) {
	port := startServerWithDevIssuer(t)
	const sessID = "shared"

	tok := func(role string) string { return issueToken(t, port, "tenant-A", sessID, role) }

	cli := dialAuthed(t, port, sessID, tok("client"), envelope{
		Type: "ice", From: "client",
		Data: nil,
	})
	defer cli.Close()
	brw := dialAuthed(t, port, sessID, tok("browser"), envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER"}`),
	})
	defer brw.Close()

	got := readEnvelope(t, cli, 2*time.Second)
	if got.Type != "offer" || !bytes.Contains(got.Data, []byte(`"OFFER"`)) {
		t.Fatalf("expected forwarded offer, got %+v", got)
	}
}

// TestAuthDisabledFallbackUsesAnonymousTenant: with no auth, all
// connections share the anonymous tenant — meaning two callers using
// the same session_id behave like the pre-T67 single-namespace world
// (a third caller of the same role gets ClosePolicyViolation).
func TestAuthDisabledFallbackUsesAnonymousTenant(t *testing.T) {
	port := startServer(t) // no dev issuer → CHROMELESS_AUTH_PUBKEY unset → auth disabled
	const sessID = "fallback"

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

	got := readEnvelope(t, cli, 2*time.Second)
	if got.Type != "offer" {
		t.Fatalf("expected forwarded offer in anonymous tenant, got %+v", got)
	}

	// A third caller claiming "client" should still be rejected because
	// the anonymous tenant collapses everything into one namespace.
	dup := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "client",
		Data: json.RawMessage(`{"sdp":"DUP"}`),
	})
	defer dup.Close()
	expectClose(t, dup, websocket.ClosePolicyViolation, 2*time.Second)
}

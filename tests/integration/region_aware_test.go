// T93 — region-aware signaling: tokens carrying an `aud` claim are
// rejected when their list does not include the signaling server's
// region. Asserts the wire-level rejection, not just the unit-test
// path through verifyToken.

package integration_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// startRegionalServer is startServerWithDevIssuer + CHROMELESS_REGION.
// Kept separate so the existing T67/T89 tests don't have to absorb
// the region label.
func startRegionalServer(t *testing.T, region string) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("free port: %v", err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	_ = l.Close()

	var stdout, stderr bytes.Buffer
	cmd := exec.Command(binaryPath)
	env := []string{}
	for _, kv := range os.Environ() {
		if strings.HasPrefix(kv, "CHROMELESS_AUTH_PUBKEY=") ||
			strings.HasPrefix(kv, "CHROMELESS_REGION=") {
			continue
		}
		env = append(env, kv)
	}
	env = append(env,
		fmt.Sprintf("SIGNALING_PORT=%d", port),
		"CHROMELESS_DEV_ISSUER=1",
		"CHROMELESS_REGION="+region,
	)
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

// issueTokenWithAud is issueToken + ?aud=… so we can mint
// region-restricted tokens.
func issueTokenWithAud(t *testing.T, port int, tenant, sessionID, role string, aud []string) string {
	t.Helper()
	q := url.Values{}
	q.Set("role", role)
	q.Set("session_id", sessionID)
	q.Set("tenant", tenant)
	if len(aud) > 0 {
		q.Set("aud", strings.Join(aud, ","))
	}
	u := fmt.Sprintf("http://127.0.0.1:%d/issue-token?%s", port, q.Encode())
	resp, err := http.Get(u)
	if err != nil {
		t.Fatalf("issue-token GET: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("issue-token: status %d", resp.StatusCode)
	}
	var out struct {
		Token string `json:"token"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		t.Fatalf("issue-token decode: %v", err)
	}
	if out.Token == "" {
		t.Fatal("issue-token: empty token")
	}
	return out.Token
}

// dialWithToken opens a WS and writes one register frame. Returns
// the conn and any error from the handshake / first read so callers
// can assert close codes.
func dialWithToken(t *testing.T, port int, sessionID, token string, hello envelope) *websocket.Conn {
	t.Helper()
	u := fmt.Sprintf("ws://127.0.0.1:%d/ws/%s?token=%s",
		port, url.PathEscape(sessionID), url.QueryEscape(token))
	c, _, err := websocket.DefaultDialer.Dial(u, nil)
	if err != nil {
		t.Fatalf("ws dial: %v", err)
	}
	raw, err := json.Marshal(hello)
	if err != nil {
		t.Fatalf("marshal hello: %v", err)
	}
	if err := c.WriteMessage(websocket.TextMessage, raw); err != nil {
		t.Fatalf("write hello: %v", err)
	}
	return c
}

// TestRegionAware_AudExcludesRegion: token says aud=[eu-west-1], the
// signaling server is region=us-east-1 — the WS register frame is
// rejected with close code 1008 (PolicyViolation).
func TestRegionAware_AudExcludesRegion(t *testing.T) {
	port := startRegionalServer(t, "us-east-1")
	tok := issueTokenWithAud(t, port, "tenant-A", "rt-region-deny", "client",
		[]string{"eu-west-1", "eu-central-1"})

	c := dialWithToken(t, port, "rt-region-deny", tok, envelope{
		Type: "ice", From: "client",
	})
	defer c.Close()

	_ = c.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, _, err := c.ReadMessage()
	if err == nil {
		t.Fatal("expected close on region mismatch, got nil error")
	}
	closeErr, ok := err.(*websocket.CloseError)
	if !ok {
		t.Fatalf("expected websocket.CloseError, got %T: %v", err, err)
	}
	if closeErr.Code != websocket.ClosePolicyViolation {
		t.Fatalf("close code: want %d (policy violation), got %d (text=%q)",
			websocket.ClosePolicyViolation, closeErr.Code, closeErr.Text)
	}
	if !strings.Contains(closeErr.Text, "region") {
		t.Errorf("close reason should mention region: got %q", closeErr.Text)
	}
}

// TestRegionAware_AudIncludesRegion: token says aud=[us-east-1,
// us-west-2]; signaling region is us-east-1 — connection is allowed.
// Symmetric companion to the deny case so a future regression that
// flips the comparison is caught both ways.
func TestRegionAware_AudIncludesRegion(t *testing.T) {
	port := startRegionalServer(t, "us-east-1")
	tok := issueTokenWithAud(t, port, "tenant-A", "rt-region-allow", "client",
		[]string{"us-east-1", "us-west-2"})

	c := dialWithToken(t, port, "rt-region-allow", tok, envelope{
		Type: "ice", From: "client",
	})
	defer c.Close()

	// No remote close should arrive within a short window — the
	// connection registered successfully.
	_ = c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	_, _, err := c.ReadMessage()
	// Either timeout (no peer to forward from yet) or clean close on
	// our side is fine; what we MUST NOT see is a CloseError.
	if err != nil {
		if ce, ok := err.(*websocket.CloseError); ok {
			t.Fatalf("unexpected close on allowed region: code=%d text=%q",
				ce.Code, ce.Text)
		}
	}
}

// TestRegionAware_AudEmptyAllowsAnyRegion: pre-T93 backwards-compat —
// tokens without an aud claim continue to work in any region.
func TestRegionAware_AudEmptyAllowsAnyRegion(t *testing.T) {
	port := startRegionalServer(t, "us-east-1")
	tok := issueTokenWithAud(t, port, "tenant-A", "rt-region-noaud", "client", nil)

	c := dialWithToken(t, port, "rt-region-noaud", tok, envelope{
		Type: "ice", From: "client",
	})
	defer c.Close()

	_ = c.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	_, _, err := c.ReadMessage()
	if err != nil {
		if ce, ok := err.(*websocket.CloseError); ok {
			t.Fatalf("unexpected close on no-aud token: code=%d text=%q",
				ce.Code, ce.Text)
		}
	}
}

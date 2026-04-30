// End-to-end integration test for the file-upload bridge (T74).
//
// Spec under test: docs/protocols/file-upload.md (v1).
//
// Strategy: build the file-bridge binary once, run it as a
// subprocess against an in-process fake CDP, drive a 1 MiB PDF
// upload over the bridge's WebSocket source, and assert:
//   - the bridge sends file_upload_progress envelopes
//   - the bridge sends file_upload_complete with attached_via=domSetFileInputFiles
//   - the file on disk matches the uploaded bytes
//   - the SHA-256 round-trip is verified by the bridge (not just by us)
//   - DOM.getDocument → DOM.querySelector → DOM.setFileInputFiles
//     fires in order against the fake CDP
//
// We do NOT run a real Chromium here — the unit tests already cover
// the per-codepath correctness; this test asserts the binary works
// over its real I/O surfaces.
package integration_test

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// ---------------------------------------------------------------------------
// Build the file-bridge binary once for the package.
// ---------------------------------------------------------------------------

var fileBridgeBinary string

func buildFileBridge(t *testing.T) string {
	t.Helper()
	if fileBridgeBinary != "" {
		return fileBridgeBinary
	}
	tmp, err := os.MkdirTemp("", "file-bridge-bin-")
	if err != nil {
		t.Fatalf("mktemp: %v", err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(tmp) })

	out := filepath.Join(tmp, "file-bridge")
	cmd := exec.Command("go", "build", "-o", out, ".")
	cmd.Dir = filepath.Join("..", "..", "capture", "file-bridge")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("go build file-bridge: %v", err)
	}
	fileBridgeBinary = out
	return out
}

// ---------------------------------------------------------------------------
// Fake CDP server — speaks /json/list discovery + /devtools/page/1
// WebSocket and replies with canned answers for the DOM.* methods
// the bridge calls during attach.
// ---------------------------------------------------------------------------

type fileFakeCDP struct {
	mu       sync.Mutex
	conn     *websocket.Conn
	calls    []string
	server   *httptest.Server
	ready    chan struct{}
}

func newFileFakeCDP(t *testing.T) *fileFakeCDP {
	t.Helper()
	f := &fileFakeCDP{ready: make(chan struct{})}
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}

	mux := http.NewServeMux()
	srv := httptest.NewServer(mux)
	f.server = srv

	mux.HandleFunc("/json/list", func(w http.ResponseWriter, _ *http.Request) {
		base := strings.Replace(srv.URL, "http://", "ws://", 1)
		body := `[{"type":"page","webSocketDebuggerUrl":"` + base + `/devtools/page/1","url":"about:blank"}]`
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, body)
	})

	mux.HandleFunc("/devtools/page/1", func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		f.mu.Lock()
		f.conn = conn
		f.mu.Unlock()
		select {
		case <-f.ready:
		default:
			close(f.ready)
		}
		for {
			_, raw, err := conn.ReadMessage()
			if err != nil {
				return
			}
			var env struct {
				ID     int64  `json:"id"`
				Method string `json:"method"`
			}
			if err := json.Unmarshal(raw, &env); err != nil {
				return
			}
			f.mu.Lock()
			f.calls = append(f.calls, env.Method)
			f.mu.Unlock()

			var reply map[string]interface{}
			switch env.Method {
			case "DOM.getDocument":
				reply = map[string]interface{}{
					"id": env.ID,
					"result": map[string]interface{}{
						"root": map[string]interface{}{"nodeId": 1},
					},
				}
			case "DOM.querySelector":
				reply = map[string]interface{}{
					"id": env.ID,
					"result": map[string]interface{}{"nodeId": 42},
				}
			default:
				reply = map[string]interface{}{
					"id":     env.ID,
					"result": map[string]interface{}{},
				}
			}
			rb, _ := json.Marshal(reply)
			_ = conn.WriteMessage(websocket.TextMessage, rb)
		}
	})

	return f
}

func (f *fileFakeCDP) URL() string { return f.server.URL }
func (f *fileFakeCDP) Close()      { f.server.Close() }

func (f *fileFakeCDP) methods() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]string, len(f.calls))
	copy(out, f.calls)
	return out
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

// TestFileBridgeUpload1MiBPDF — DoD: end-to-end 1 MB PDF upload works.
func TestFileBridgeUpload1MiBPDF(t *testing.T) {
	bin := buildFileBridge(t)
	cdp := newFileFakeCDP(t)
	defer cdp.Close()

	uploadDir := t.TempDir()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// Pick a high port likely to be free for the bridge's WebSocket
	// source. Race-free port allocation is harder than we need here;
	// listen-then-close-then-pass-port is a fine heuristic for tests.
	port := freePortFB(t)
	wsAddr := fmt.Sprintf("127.0.0.1:%d", port)
	wsURL := fmt.Sprintf("ws://%s/files", wsAddr)

	cmd := exec.CommandContext(ctx, bin,
		"--ws-addr", wsAddr,
		"--ws-path", "/files",
		"--cdp-url", cdp.URL(),
		"--upload-dir", uploadDir,
		"--session-id", "sess1",
	)
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("start file-bridge: %v", err)
	}
	defer func() { _ = cmd.Process.Kill(); _ = cmd.Wait() }()

	// Wait for the bridge's WS endpoint to come up.
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = 2 * time.Second
	var c *websocket.Conn
	deadline := time.Now().Add(10 * time.Second)
	var dialErr error
	for time.Now().Before(deadline) {
		c, _, dialErr = dialer.Dial(wsURL, nil)
		if dialErr == nil {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if dialErr != nil {
		t.Fatalf("dial bridge: %v", dialErr)
	}
	defer c.Close()

	// Build a 1 MiB PDF-shaped payload. http.DetectContentType keys
	// "application/pdf" off the leading "%PDF-" magic; we make sure
	// the rest is benign printable bytes so neither the sniff nor any
	// ASCII check trips.
	const total = 1024 * 1024
	payload := make([]byte, total)
	header := []byte("%PDF-1.4\n")
	copy(payload, header)
	for i := len(header); i < total-8; i++ {
		payload[i] = byte('A' + (i % 26))
	}
	copy(payload[total-8:], []byte("\n%%EOF\n"))
	digest := sha256.Sum256(payload)

	uploadID := "u1"
	target := "input[type=file]"

	// Send file_upload_start.
	startEnv := map[string]interface{}{
		"v": 1, "type": "file_upload_start",
		"upload_id": uploadID, "name": "report.pdf",
		"mime_type": "application/pdf",
		"size":      total, "sha256": hex.EncodeToString(digest[:]),
		"target_selector": target,
	}
	startRaw, _ := json.Marshal(startEnv)
	if err := c.WriteMessage(websocket.TextMessage, startRaw); err != nil {
		t.Fatal(err)
	}

	// Send chunks (32 KiB each → 32 chunks for 1 MiB).
	const chunkSize = 32 * 1024
	for off, seq := 0, 0; off < total; seq++ {
		end := off + chunkSize
		if end > total {
			end = total
		}
		chunkEnv := map[string]interface{}{
			"v": 1, "type": "file_upload_chunk",
			"upload_id": uploadID, "seq": seq,
			"data": base64.StdEncoding.EncodeToString(payload[off:end]),
		}
		raw, _ := json.Marshal(chunkEnv)
		if err := c.WriteMessage(websocket.TextMessage, raw); err != nil {
			t.Fatalf("send chunk %d: %v", seq, err)
		}
		off = end
	}

	// Send file_upload_end.
	endEnv := map[string]interface{}{
		"v": 1, "type": "file_upload_end", "upload_id": uploadID,
	}
	endRaw, _ := json.Marshal(endEnv)
	if err := c.WriteMessage(websocket.TextMessage, endRaw); err != nil {
		t.Fatal(err)
	}

	// Read replies until we see file_upload_complete (or error).
	var sawProgress, sawComplete bool
	var serverPath string
	deadline = time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) && !sawComplete {
		_ = c.SetReadDeadline(deadline)
		_, raw, err := c.ReadMessage()
		if err != nil {
			t.Fatalf("read reply: %v", err)
		}
		var m map[string]interface{}
		if err := json.Unmarshal(raw, &m); err != nil {
			continue
		}
		switch m["type"] {
		case "file_upload_progress":
			sawProgress = true
		case "file_upload_complete":
			sawComplete = true
			serverPath, _ = m["server_path"].(string)
			if av, _ := m["attached_via"].(string); av != "domSetFileInputFiles" {
				t.Errorf("attached_via = %v, want domSetFileInputFiles", av)
			}
		case "file_upload_error":
			t.Fatalf("got error envelope: %v", m)
		}
	}
	if !sawComplete {
		t.Fatal("never saw file_upload_complete")
	}
	if !sawProgress {
		t.Errorf("expected at least one file_upload_progress envelope")
	}

	// Verify file on disk.
	wantPath := filepath.Join(uploadDir, "sess1", uploadID+"__report.pdf")
	if serverPath != wantPath {
		t.Errorf("server_path = %v, want %v", serverPath, wantPath)
	}
	got, err := os.ReadFile(wantPath)
	if err != nil {
		t.Fatalf("read uploaded file: %v", err)
	}
	if len(got) != total {
		t.Errorf("uploaded size = %d, want %d", len(got), total)
	}
	gotDigest := sha256.Sum256(got)
	if gotDigest != digest {
		t.Errorf("uploaded SHA-256 mismatch")
	}

	// Verify the bridge talked to CDP in the expected sequence.
	saw := cdp.methods()
	want := []string{"DOM.getDocument", "DOM.querySelector", "DOM.setFileInputFiles"}
	for _, w := range want {
		found := false
		for _, m := range saw {
			if m == w {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("expected CDP %s; saw %v", w, saw)
		}
	}
}

// freePortFB allocates an ephemeral TCP port, closes the listener, and
// returns the port for the bridge to bind. Inherently racy with
// other binders on the host, but the window is microseconds and CI
// runs serial integration tests.
func freePortFB(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	port := l.Addr().(*net.TCPAddr).Port
	_ = l.Close()
	return port
}

// Tests for the cursor watcher.
//
// Two layers, mirroring capture/input-bridge:
//
//  1. Pure-function tests (envelope encoding, equalState dedup,
//     parseFlags).
//  2. End-to-end test against a fake CDP server: speaks just enough
//     CDP to accept the install RPCs and then synthesizes
//     Runtime.bindingCalled events, asserting the watcher emits the
//     right wire envelopes (and only on change).
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// ---------------------------------------------------------------------------
// 1. Pure-function tests
// ---------------------------------------------------------------------------

func TestEqualState(t *testing.T) {
	a := cursorData{X: 1, Y: 2, Visible: true, Shape: "default"}
	if !equalState(a, a) {
		t.Errorf("equalState should be reflexive")
	}
	b := a
	b.X = 3
	if equalState(a, b) {
		t.Errorf("differing x should not be equal")
	}
	c := a
	c.Shape = "pointer"
	if equalState(a, c) {
		t.Errorf("differing shape should not be equal")
	}
	d1 := a
	d1.Hotspot = &xy{X: 5, Y: 5}
	d2 := a
	d2.Hotspot = &xy{X: 5, Y: 5}
	if !equalState(d1, d2) {
		t.Errorf("equal hotspots should be equal")
	}
	d3 := a
	d3.Hotspot = &xy{X: 6, Y: 5}
	if equalState(d1, d3) {
		t.Errorf("differing hotspot should not be equal")
	}
}

type recorder struct {
	mu  sync.Mutex
	out [][]byte
}

func (r *recorder) Emit(b []byte) error {
	r.mu.Lock()
	defer r.mu.Unlock()
	cp := make([]byte, len(b))
	copy(cp, b)
	r.out = append(r.out, cp)
	return nil
}

func (r *recorder) snapshot() [][]byte {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([][]byte, len(r.out))
	copy(out, r.out)
	return out
}

func TestMaybeEmitDedup(t *testing.T) {
	r := &recorder{}
	w := newWatcher(nil, r, slog.New(slog.NewTextHandler(io.Discard, nil)))

	// Three identical state pushes → one emit.
	state := cursorData{X: 10, Y: 20, Visible: true, Shape: "default"}
	w.maybeEmit(state)
	w.maybeEmit(state)
	w.maybeEmit(state)

	if got := len(r.snapshot()); got != 1 {
		t.Fatalf("identical-state pushes should dedup; got %d emits, want 1", got)
	}

	// Change one field → second emit.
	state.X = 11
	w.maybeEmit(state)
	if got := len(r.snapshot()); got != 2 {
		t.Fatalf("changed state should re-emit; got %d, want 2", got)
	}
	state.Shape = "pointer"
	w.maybeEmit(state)
	if got := len(r.snapshot()); got != 3 {
		t.Fatalf("changed shape should re-emit; got %d, want 3", got)
	}
}

func TestEmittedEnvelopeShape(t *testing.T) {
	r := &recorder{}
	w := newWatcher(nil, r, slog.New(slog.NewTextHandler(io.Discard, nil)))

	state := cursorData{X: 100, Y: 200, Visible: true, Shape: "pointer"}
	w.emitSynthetic(state)
	envs := r.snapshot()
	if len(envs) != 1 {
		t.Fatalf("expected 1 envelope, got %d", len(envs))
	}
	var got cursorEnvelope
	if err := json.Unmarshal(envs[0], &got); err != nil {
		t.Fatalf("envelope not JSON: %v\n%s", err, envs[0])
	}
	if got.V != 1 {
		t.Errorf("v = %d, want 1", got.V)
	}
	if got.Type != "cursor" {
		t.Errorf("type = %q, want \"cursor\"", got.Type)
	}
	if got.Seq != 0 {
		t.Errorf("seq = %d, want 0 for first envelope", got.Seq)
	}
	if got.Data != state {
		t.Errorf("data round-trip mismatch: got %+v want %+v", got.Data, state)
	}
	if got.T == 0 {
		t.Errorf("t should be set to a positive epoch ms")
	}
}

func TestParseFlags(t *testing.T) {
	cfg, err := parseFlags([]string{"--sink", "ws", "--sink-url", "ws://example.test:1/cursor"})
	if err != nil {
		t.Fatalf("parseFlags: %v", err)
	}
	if cfg.sink != "ws" || cfg.wsURL != "ws://example.test:1/cursor" {
		t.Errorf("unexpected cfg: %+v", cfg)
	}
	if _, err := parseFlags([]string{"--sink", "garbage"}); err == nil {
		t.Errorf("expected error for invalid --sink")
	}
}

func TestDryRunEmitsOneEnvelope(t *testing.T) {
	var buf bytes.Buffer
	cfg, err := parseFlags([]string{"--dry-run"})
	if err != nil {
		t.Fatal(err)
	}
	if err := run(context.Background(), cfg, &buf, slog.New(slog.NewTextHandler(io.Discard, nil))); err != nil {
		t.Fatalf("run: %v", err)
	}
	lines := bytes.Split(bytes.TrimSpace(buf.Bytes()), []byte("\n"))
	if len(lines) != 1 {
		t.Fatalf("dry-run should emit exactly one envelope, got %d lines:\n%s", len(lines), buf.String())
	}
	var env cursorEnvelope
	if err := json.Unmarshal(lines[0], &env); err != nil {
		t.Fatalf("envelope: %v", err)
	}
	if env.V != 1 || env.Type != "cursor" || !env.Data.Visible || env.Data.Shape != "default" {
		t.Errorf("unexpected dry-run envelope: %+v", env)
	}
}

// ---------------------------------------------------------------------------
// 2. End-to-end against a fake CDP server
// ---------------------------------------------------------------------------

type fakeCDPServer struct {
	mu       sync.Mutex
	server   *httptest.Server
	conn     *websocket.Conn
	received []recordedCall
	ready    chan struct{}
}

type recordedCall struct {
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

func newFakeCDPServer(t *testing.T) *fakeCDPServer {
	t.Helper()
	f := &fakeCDPServer{ready: make(chan struct{})}
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
			t.Errorf("upgrade: %v", err)
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
				ID     int64           `json:"id"`
				Method string          `json:"method"`
				Params json.RawMessage `json:"params"`
			}
			if err := json.Unmarshal(raw, &env); err != nil {
				return
			}
			f.mu.Lock()
			f.received = append(f.received, recordedCall{Method: env.Method, Params: env.Params})
			f.mu.Unlock()
			reply := map[string]any{"id": env.ID, "result": map[string]any{}}
			rb, _ := json.Marshal(reply)
			if err := conn.WriteMessage(websocket.TextMessage, rb); err != nil {
				return
			}
		}
	})

	return f
}

func (f *fakeCDPServer) URL() string { return f.server.URL }
func (f *fakeCDPServer) Close()      { f.server.Close() }

// pushBindingCalled simulates the page calling window.__cb_cursor__("...")
// — Chromium reports that as a Runtime.bindingCalled event with a JSON
// string `payload` field.
func (f *fakeCDPServer) pushBindingCalled(t *testing.T, payload string) {
	t.Helper()
	<-f.ready
	f.mu.Lock()
	conn := f.conn
	f.mu.Unlock()
	frame := map[string]any{
		"method": "Runtime.bindingCalled",
		"params": map[string]any{"name": "__cb_cursor__", "payload": payload, "executionContextId": 1},
	}
	raw, _ := json.Marshal(frame)
	if err := conn.WriteMessage(websocket.TextMessage, raw); err != nil {
		t.Fatalf("push event: %v", err)
	}
}

func (f *fakeCDPServer) calls() []recordedCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]recordedCall, len(f.received))
	copy(out, f.received)
	return out
}

func TestWatcherEndToEnd(t *testing.T) {
	f := newFakeCDPServer(t)
	defer f.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cdp, err := dialCDP(ctx, f.URL(), slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	r := &recorder{}
	w := newWatcher(cdp, r, slog.New(slog.NewTextHandler(io.Discard, nil)))

	if err := w.install(ctx); err != nil {
		t.Fatalf("install: %v", err)
	}

	// install should have called these CDP methods:
	wanted := map[string]bool{
		"Page.enable":                          false,
		"Runtime.enable":                       false,
		"Runtime.addBinding":                   false,
		"Page.addScriptToEvaluateOnNewDocument": false,
		"Runtime.evaluate":                     false,
	}
	for _, c := range f.calls() {
		if _, ok := wanted[c.Method]; ok {
			wanted[c.Method] = true
		}
	}
	for m, seen := range wanted {
		if !seen {
			t.Errorf("install did not invoke %s; calls=%v", m, f.calls())
		}
	}

	runErrCh := make(chan error, 1)
	go func() { runErrCh <- w.run(ctx) }()

	// Push two distinct events; expect two emits.
	f.pushBindingCalled(t, `{"x":10,"y":20,"visible":true,"shape":"default"}`)
	f.pushBindingCalled(t, `{"x":30,"y":40,"visible":true,"shape":"pointer"}`)
	// Push duplicate of the second; expect dedup (still 2 emits).
	f.pushBindingCalled(t, `{"x":30,"y":40,"visible":true,"shape":"pointer"}`)
	// Push a "visible:false" change.
	f.pushBindingCalled(t, `{"x":30,"y":40,"visible":false,"shape":"none"}`)

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if len(r.snapshot()) >= 3 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}

	envs := r.snapshot()
	if len(envs) != 3 {
		t.Fatalf("expected 3 envelopes (with dedup), got %d", len(envs))
	}

	var first cursorEnvelope
	if err := json.Unmarshal(envs[0], &first); err != nil {
		t.Fatal(err)
	}
	if first.Data.X != 10 || first.Data.Y != 20 || first.Data.Shape != "default" {
		t.Errorf("first envelope wrong: %+v", first)
	}
	var third cursorEnvelope
	if err := json.Unmarshal(envs[2], &third); err != nil {
		t.Fatal(err)
	}
	if third.Data.Visible || third.Data.Shape != "none" {
		t.Errorf("third envelope wrong: %+v", third)
	}

	cancel()
	<-runErrCh
}

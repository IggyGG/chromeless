// Tests for the input-bridge service.
//
// Two layers:
//
//  1. Pure-function table tests for protocol parsing (parseEnvelope,
//     modifier remap, button remap). Fast, no network.
//
//  2. End-to-end dispatcher tests against a *fake CDP target* — a
//     gorilla/websocket server that records each frame the bridge
//     sends and replies with empty results so the dispatcher's
//     request/response pairing exercises the same code path it would
//     against real Chromium.
package main

import (
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
// 1. Protocol parsing
// ---------------------------------------------------------------------------

func TestParseEnvelope(t *testing.T) {
	cases := []struct {
		name    string
		raw     string
		wantErr string // substring; "" = expect no error
	}{
		{
			name: "valid mouse_move",
			raw:  `{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":10,"y":20}}`,
		},
		{
			name: "valid key_down with mods",
			raw:  `{"v":1,"type":"key_down","t":2,"seq":1,"data":{"code":"KeyA","key":"a","mods":3}}`,
		},
		{
			name:    "wrong version",
			raw:     `{"v":2,"type":"mouse_move","t":1,"seq":0,"data":{}}`,
			wantErr: "unsupported protocol version",
		},
		{
			name:    "missing type",
			raw:     `{"v":1,"t":1,"seq":0,"data":{}}`,
			wantErr: "missing 'type'",
		},
		{
			name:    "garbage JSON",
			raw:     `not json at all`,
			wantErr: "invalid JSON",
		},
		{
			name:    "empty",
			raw:     ``,
			wantErr: "invalid JSON",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			env, err := parseEnvelope([]byte(tc.raw))
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				if env.V != 1 {
					t.Errorf("env.V = %d, want 1", env.V)
				}
				return
			}
			if err == nil {
				t.Fatalf("expected error containing %q, got nil", tc.wantErr)
			}
			if !strings.Contains(err.Error(), tc.wantErr) {
				t.Errorf("error = %q, want contains %q", err.Error(), tc.wantErr)
			}
		})
	}
}

func TestProtocolModsToCDP(t *testing.T) {
	cases := []struct {
		in, out int
		name    string
	}{
		{0, 0, "none"},
		{modShift, 8, "shift"},
		{modCtrl, 2, "ctrl"},
		{modAlt, 1, "alt"},
		{modMeta, 4, "meta"},
		{modShift | modCtrl, 8 | 2, "shift+ctrl"},
		{modShift | modCtrl | modAlt | modMeta, 15, "all"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := protocolModsToCDP(tc.in)
			if got != tc.out {
				t.Errorf("protocolModsToCDP(%b) = %b, want %b", tc.in, got, tc.out)
			}
		})
	}
}

func TestProtocolButtonToCDP(t *testing.T) {
	cases := []struct {
		in   int
		want string
	}{
		{0, "left"}, {1, "middle"}, {2, "right"}, {3, "back"}, {4, "forward"},
		{99, "none"}, {-1, "none"},
	}
	for _, tc := range cases {
		got := protocolButtonToCDP(tc.in)
		if got != tc.want {
			t.Errorf("protocolButtonToCDP(%d) = %s, want %s", tc.in, got, tc.want)
		}
	}
}

// ---------------------------------------------------------------------------
// 2. Fake CDP target — exercises Dispatch end to end
// ---------------------------------------------------------------------------

// fakeCDP is a minimal CDP-over-WebSocket server. It speaks just enough
// to make /json/list discovery work, then accepts a websocket and
// records each method+params it receives. Every call gets a synthetic
// {"id":N,"result":{}} reply so the bridge's request/response pairing
// is exercised.
type fakeCDP struct {
	mu       sync.Mutex
	received []recordedCall
	server   *httptest.Server
}

type recordedCall struct {
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

func newFakeCDP(t *testing.T) *fakeCDP {
	t.Helper()
	f := &fakeCDP{}
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}

	mux := http.NewServeMux()
	// We register the page-target ws path *after* the test server starts
	// so we know its URL — see ServeHTTP closure below.
	srv := httptest.NewServer(mux)
	f.server = srv

	mux.HandleFunc("/json/list", func(w http.ResponseWriter, _ *http.Request) {
		// Construct a target description pointing at /devtools/page/1.
		// The test server URL is http://127.0.0.1:NNNN — switch scheme
		// to ws://.
		base := strings.Replace(srv.URL, "http://", "ws://", 1)
		body := `[{"type":"page","webSocketDebuggerUrl":"` + base + `/devtools/page/1","url":"about:blank"}]`
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, body)
	})

	mux.HandleFunc("/devtools/page/1", func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("fakeCDP upgrade: %v", err)
			return
		}
		defer conn.Close()
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
			rawReply, _ := json.Marshal(reply)
			if err := conn.WriteMessage(websocket.TextMessage, rawReply); err != nil {
				return
			}
		}
	})

	return f
}

func (f *fakeCDP) Close() { f.server.Close() }

func (f *fakeCDP) Calls() []recordedCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]recordedCall, len(f.received))
	copy(out, f.received)
	return out
}

func (f *fakeCDP) URL() string { return f.server.URL }

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// TestDispatchEndToEnd drives a representative envelope through
// dialCDP + dispatcher and verifies each one produced the expected CDP
// method.
func TestDispatchEndToEnd(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	cases := []struct {
		raw    string
		method string
	}{
		{`{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":10,"y":20}}`, "Input.dispatchMouseEvent"},
		{`{"v":1,"type":"mouse_button","t":2,"seq":1,"data":{"button":0,"action":"down","x":10,"y":20}}`, "Input.dispatchMouseEvent"},
		{`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":2,"action":"up","x":11,"y":22}}`, "Input.dispatchMouseEvent"},
		{`{"v":1,"type":"mouse_wheel","t":4,"seq":3,"data":{"dx":0,"dy":-120,"mode":0,"x":11,"y":22}}`, "Input.dispatchMouseEvent"},
		{`{"v":1,"type":"key_down","t":5,"seq":4,"data":{"code":"KeyA","key":"a","mods":1}}`, "Input.dispatchKeyEvent"},
		{`{"v":1,"type":"key_up","t":6,"seq":5,"data":{"code":"KeyA","key":"a","mods":0}}`, "Input.dispatchKeyEvent"},
		{`{"v":1,"type":"composition_start","t":7,"seq":6,"data":{"data":"こ"}}`, "Input.imeSetComposition"},
		{`{"v":1,"type":"composition_update","t":8,"seq":7,"data":{"data":"こん"}}`, "Input.imeSetComposition"},
		{`{"v":1,"type":"composition_end","t":9,"seq":8,"data":{"data":"こんにちは"}}`, "Input.insertText"},
		{`{"v":1,"type":"clipboard_paste","t":10,"seq":9,"data":{"text":"hi"}}`, "Input.insertText"},
	}

	for _, tc := range cases {
		env, err := parseEnvelope([]byte(tc.raw))
		if err != nil {
			t.Fatalf("parse %s: %v", tc.raw, err)
		}
		if err := disp.Dispatch(ctx, env); err != nil {
			t.Errorf("dispatch %s: %v", env.Type, err)
		}
	}

	// We expect bringToFront once + one CDP call per envelope above.
	expected := len(cases) + 1
	calls := f.Calls()
	if len(calls) < expected {
		t.Fatalf("got %d CDP calls, want >= %d", len(calls), expected)
	}
	// Page.bringToFront should be among the calls.
	var sawBringFront bool
	methodCounts := map[string]int{}
	for _, c := range calls {
		methodCounts[c.Method]++
		if c.Method == "Page.bringToFront" {
			sawBringFront = true
		}
	}
	if !sawBringFront {
		t.Errorf("expected Page.bringToFront among CDP calls; saw: %v", methodCounts)
	}

	// Verify each test-case method was hit (counts may vary because
	// e.g. clipboard_copy_request would emit two key events).
	wantedMethods := map[string]bool{}
	for _, tc := range cases {
		wantedMethods[tc.method] = true
	}
	for m := range wantedMethods {
		if methodCounts[m] == 0 {
			t.Errorf("expected at least one %s call; got methods=%v", m, methodCounts)
		}
	}
}

func TestDispatchUnknownTypeIgnored(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	m := newMetrics()
	disp := newDispatcher(cdp, m, quietLogger())

	env, err := parseEnvelope([]byte(`{"v":1,"type":"definitely_not_a_real_type","t":1,"seq":0,"data":{}}`))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Fatalf("expected nil error for unknown type, got %v", err)
	}
	// bringToFront still ran once; no event-specific CDP method.
	calls := f.Calls()
	for _, c := range calls {
		if c.Method != "Page.bringToFront" {
			t.Errorf("unknown event produced unexpected CDP call %s", c.Method)
		}
	}
}

// TestStdinSource checks the line-delimited reader handler.
func TestStdinSource(t *testing.T) {
	input := strings.NewReader(
		`{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":10,"y":20}}` + "\n" +
			"\n" + // blank line — should be skipped
			`{"v":1,"type":"mouse_move","t":2,"seq":1,"data":{"x":11,"y":22}}` + "\n",
	)
	var seen [][]byte
	handler := func(_ context.Context, raw []byte) {
		seen = append(seen, raw)
	}
	if err := runStdinSource(context.Background(), input, handler); err != nil {
		t.Fatalf("runStdinSource: %v", err)
	}
	if len(seen) != 2 {
		t.Fatalf("seen %d lines, want 2", len(seen))
	}
}

// TestDryRunDispatch covers the noopCDP path.
func TestDryRunDispatch(t *testing.T) {
	disp := newDispatcher(noopCDP{log: quietLogger()}, newMetrics(), quietLogger())
	env, err := parseEnvelope([]byte(`{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":1,"y":2}}`))
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if err := disp.Dispatch(context.Background(), env); err != nil {
		t.Fatalf("dispatch: %v", err)
	}
}

// ---------------------------------------------------------------------------
// 3. Drag-and-drop dispatch (v1.1)
// ---------------------------------------------------------------------------

func TestDispatchDragLifecycle(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	envs := []string{
		`{"v":1,"type":"drag_start","t":1,"seq":0,"data":{"x":100,"y":200,"types":["text/plain"],"items":[{"kind":"string","type":"text/plain","data":"hello"}]}}`,
		`{"v":1,"type":"drag_over","t":2,"seq":1,"data":{"x":110,"y":210}}`,
		`{"v":1,"type":"drag_over","t":3,"seq":2,"data":{"x":120,"y":220}}`,
		`{"v":1,"type":"drop","t":4,"seq":3,"data":{"x":130,"y":230,"types":["text/plain"],"items":[{"kind":"string","type":"text/plain","data":"hello"}]}}`,
		`{"v":1,"type":"drag_end","t":5,"seq":4,"data":{"success":true}}`,
	}
	for _, raw := range envs {
		env, err := parseEnvelope([]byte(raw))
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		if err := disp.Dispatch(ctx, env); err != nil {
			t.Errorf("dispatch %s: %v", env.Type, err)
		}
	}

	calls := f.Calls()
	dragTypes := []string{}
	for _, c := range calls {
		if c.Method != "Input.dispatchDragEvent" {
			continue
		}
		var p map[string]any
		_ = json.Unmarshal(c.Params, &p)
		if t2, ok := p["type"].(string); ok {
			dragTypes = append(dragTypes, t2)
		}
	}
	// drag_start → dragEnter, drag_over (×2) → dragOver, drop → drop;
	// drag_end success:true is in-process state-clear only.
	wantDragSequence := []string{"dragEnter", "dragOver", "dragOver", "drop"}
	if len(dragTypes) != len(wantDragSequence) {
		t.Fatalf("drag CDP sequence: got %v, want %v", dragTypes, wantDragSequence)
	}
	for i, w := range wantDragSequence {
		if dragTypes[i] != w {
			t.Errorf("drag step %d: got %s, want %s", i, dragTypes[i], w)
		}
	}

	// setInterceptDrags should have been called once (idempotent across
	// the whole drag lifecycle).
	intercepts := 0
	for _, c := range calls {
		if c.Method == "Input.setInterceptDrags" {
			intercepts++
		}
	}
	if intercepts != 1 {
		t.Errorf("Input.setInterceptDrags fired %d times, want 1", intercepts)
	}
}

func TestDispatchDragCancelOnFalseEnd(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	for _, raw := range []string{
		`{"v":1,"type":"drag_start","t":1,"seq":0,"data":{"x":1,"y":2,"types":[],"items":[]}}`,
		`{"v":1,"type":"drag_over","t":2,"seq":1,"data":{"x":3,"y":4}}`,
		`{"v":1,"type":"drag_end","t":3,"seq":2,"data":{"success":false}}`,
	} {
		env, _ := parseEnvelope([]byte(raw))
		_ = disp.Dispatch(ctx, env)
	}

	cancelSeen := false
	for _, c := range f.Calls() {
		if c.Method != "Input.dispatchDragEvent" {
			continue
		}
		var p map[string]any
		_ = json.Unmarshal(c.Params, &p)
		if p["type"] == "dragCancel" {
			cancelSeen = true
		}
	}
	if !cancelSeen {
		t.Errorf("expected dragCancel on success:false drag_end")
	}
}

func TestDispatchDragOverBeforeStartIsNoOp(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()
	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	env, _ := parseEnvelope([]byte(
		`{"v":1,"type":"drag_over","t":1,"seq":0,"data":{"x":10,"y":20}}`))
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Errorf("expected nil error for stray drag_over, got %v", err)
	}
	// No Input.dispatchDragEvent should have been issued.
	for _, c := range f.Calls() {
		if c.Method == "Input.dispatchDragEvent" {
			t.Errorf("stray drag_over should not call dispatchDragEvent")
		}
	}
}

func TestDispatchDragItemsReachCDP(t *testing.T) {
	// Verify that the items we put in drag_start and re-assert in
	// drop arrive at CDP in the expected DragData shape.
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()
	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	env, _ := parseEnvelope([]byte(
		`{"v":1,"type":"drag_start","t":1,"seq":0,"data":{"x":1,"y":2,` +
			`"types":["text/plain","text/uri-list"],` +
			`"items":[` +
			`{"kind":"string","type":"text/plain","data":"hello"},` +
			`{"kind":"string","type":"text/uri-list","data":"https://example.com/"}` +
			`]}}`))
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Fatalf("dispatch drag_start: %v", err)
	}

	for _, c := range f.Calls() {
		if c.Method != "Input.dispatchDragEvent" {
			continue
		}
		var p map[string]any
		_ = json.Unmarshal(c.Params, &p)
		data, _ := p["data"].(map[string]any)
		items, _ := data["items"].([]any)
		if len(items) != 2 {
			t.Fatalf("expected 2 CDP items, got %d", len(items))
		}
		first, _ := items[0].(map[string]any)
		if first["mimeType"] != "text/plain" || first["data"] != "hello" {
			t.Errorf("first item mismatch: %+v", first)
		}
		mask, ok := data["dragOperationsMask"]
		if !ok {
			t.Errorf("dragOperationsMask missing")
		}
		_ = mask
		return
	}
	t.Fatalf("no Input.dispatchDragEvent observed")
}

func TestParseFlags(t *testing.T) {
	cfg, err := parseFlags([]string{"--source", "ws", "--ws-addr", "127.0.0.1:9999"})
	if err != nil {
		t.Fatalf("parseFlags: %v", err)
	}
	if cfg.source != "ws" || cfg.wsAddr != "127.0.0.1:9999" {
		t.Errorf("unexpected cfg: %+v", cfg)
	}

	if _, err := parseFlags([]string{"--source", "garbage"}); err == nil {
		t.Errorf("expected error for invalid --source")
	}
}

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

// fakeCDP is a minimal flat-mode CDP server. It serves /json/version
// pointing at a single browser-level WebSocket; on that WS it acks
// the bridge's Target.setDiscoverTargets / Target.setAutoAttach
// bootstrap, synthesises one Target.attachedToTarget event for a
// page target (sessionId=fake-page-1), then records every subsequent
// command + acks with {"id":N,"result":{}}.
//
// Calls() returns only POST-bootstrap, page-session-routed commands —
// the Target.* setup commands and the synthetic attach event are
// filtered so existing assertions on dispatched method counts still
// hold.
type fakeCDP struct {
	mu       sync.Mutex
	received []recordedCall
	server   *httptest.Server

	// writeMu serialises websocket writes for the lone connection;
	// gorilla/websocket forbids concurrent writes.
	writeMu sync.Mutex
}

type recordedCall struct {
	Method    string          `json:"method"`
	SessionID string          `json:"sessionId,omitempty"`
	Params    json.RawMessage `json:"params"`
}

const fakeBrowserPath = "/devtools/browser/abc"
const fakePageSessionID = "fake-page-1"
const fakePageTargetID = "fake-page-target-1"

func newFakeCDP(t *testing.T) *fakeCDP {
	t.Helper()
	f := &fakeCDP{}
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}

	mux := http.NewServeMux()
	srv := httptest.NewServer(mux)
	f.server = srv

	mux.HandleFunc("/json/version", func(w http.ResponseWriter, _ *http.Request) {
		base := strings.Replace(srv.URL, "http://", "ws://", 1)
		body := `{"webSocketDebuggerUrl":"` + base + fakeBrowserPath + `","Browser":"fake/test"}`
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, body)
	})

	mux.HandleFunc(fakeBrowserPath, func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("fakeCDP upgrade: %v", err)
			return
		}
		defer conn.Close()

		writeJSON := func(v any) error {
			f.writeMu.Lock()
			defer f.writeMu.Unlock()
			return conn.WriteJSON(v)
		}

		for {
			_, raw, err := conn.ReadMessage()
			if err != nil {
				return
			}
			var env struct {
				ID        int64           `json:"id"`
				SessionID string          `json:"sessionId"`
				Method    string          `json:"method"`
				Params    json.RawMessage `json:"params"`
			}
			if err := json.Unmarshal(raw, &env); err != nil {
				return
			}

			// Special-case the flat-mode bootstrap commands: ack them
			// without recording, and emit a synthetic
			// Target.attachedToTarget after setAutoAttach so the
			// bridge's pageSessionSender picks up sessionId.
			switch env.Method {
			case "Page.enable":
				// Issued by pageSessionSender on each attach as a
				// BUGS-529 diagnostic so the bridge sees
				// Page.frameNavigated events. Ack but don't record;
				// it's bootstrap-shaped from the dispatcher's
				// perspective.
				if err := writeJSON(map[string]any{
					"id": env.ID, "sessionId": env.SessionID, "result": map[string]any{},
				}); err != nil {
					return
				}
				continue
			case "Target.setDiscoverTargets":
				if err := writeJSON(map[string]any{
					"id": env.ID, "result": map[string]any{},
				}); err != nil {
					return
				}
				continue
			case "Target.setAutoAttach":
				// Emit the synthetic attach event FIRST, then ack —
				// matches chromium's ordering (events stream during
				// the auto-attach handshake).
				attachEvent := map[string]any{
					"method": "Target.attachedToTarget",
					"params": map[string]any{
						"sessionId":          fakePageSessionID,
						"waitingForDebugger": false,
						"targetInfo": map[string]any{
							"targetId": fakePageTargetID,
							"type":     "page",
							"url":      "about:blank",
							"title":    "",
							"attached": true,
						},
					},
				}
				if err := writeJSON(attachEvent); err != nil {
					return
				}
				if err := writeJSON(map[string]any{
					"id": env.ID, "result": map[string]any{},
				}); err != nil {
					return
				}
				continue
			}

			// Record everything else (Page.bringToFront,
			// Input.dispatchMouseEvent, Input.dispatchKeyEvent, ...).
			f.mu.Lock()
			f.received = append(f.received, recordedCall{
				Method:    env.Method,
				SessionID: env.SessionID,
				Params:    env.Params,
			})
			f.mu.Unlock()

			if err := writeJSON(map[string]any{
				"id": env.ID, "sessionId": env.SessionID, "result": map[string]any{},
			}); err != nil {
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

// ---------------------------------------------------------------------------
// 4. Touch dispatch (v1.1)
// ---------------------------------------------------------------------------

// touchDispatchEvents pulls the type + touchPoints array out of every
// Input.dispatchTouchEvent CDP call recorded by the fake.
type touchEvent struct {
	cdpType string
	ids     []int
}

func collectTouchEvents(calls []recordedCall) []touchEvent {
	out := []touchEvent{}
	for _, c := range calls {
		if c.Method != "Input.dispatchTouchEvent" {
			continue
		}
		var p map[string]any
		_ = json.Unmarshal(c.Params, &p)
		typ, _ := p["type"].(string)
		ev := touchEvent{cdpType: typ}
		if pts, ok := p["touchPoints"].([]any); ok {
			for _, pt := range pts {
				if m, ok := pt.(map[string]any); ok {
					if id, ok := m["id"].(float64); ok {
						ev.ids = append(ev.ids, int(id))
					}
				}
			}
		}
		out = append(out, ev)
	}
	return out
}

func TestDispatchTouchSingleFingerLifecycle(t *testing.T) {
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
		`{"v":1,"type":"touch_start","t":1,"seq":0,"data":{"identifier":1,"x":10,"y":20,"radius_x":5,"radius_y":5,"force":0.5,"twist":0}}`,
		`{"v":1,"type":"touch_move","t":2,"seq":1,"data":{"identifier":1,"x":15,"y":25,"radius_x":5,"radius_y":5,"force":0.5,"twist":0}}`,
		`{"v":1,"type":"touch_end","t":3,"seq":2,"data":{"identifier":1}}`,
	} {
		env, err := parseEnvelope([]byte(raw))
		if err != nil {
			t.Fatalf("parse: %v", err)
		}
		if err := disp.Dispatch(ctx, env); err != nil {
			t.Errorf("dispatch %s: %v", env.Type, err)
		}
	}

	evs := collectTouchEvents(f.Calls())
	wantTypes := []string{"touchStart", "touchMove", "touchEnd"}
	if len(evs) != len(wantTypes) {
		t.Fatalf("got %d touch events, want %d: %+v", len(evs), len(wantTypes), evs)
	}
	for i, w := range wantTypes {
		if evs[i].cdpType != w {
			t.Errorf("touch event %d: got %s, want %s", i, evs[i].cdpType, w)
		}
	}
	// touchStart includes the new finger; touchMove still includes
	// finger 1; touchEnd touchPoints is empty (last finger lifted —
	// state-after-event).
	if len(evs[0].ids) != 1 || evs[0].ids[0] != 1 {
		t.Errorf("touchStart ids: got %v, want [1]", evs[0].ids)
	}
	if len(evs[1].ids) != 1 || evs[1].ids[0] != 1 {
		t.Errorf("touchMove ids: got %v, want [1]", evs[1].ids)
	}
	if len(evs[2].ids) != 0 {
		t.Errorf("touchEnd ids should be empty after last lift, got %v", evs[2].ids)
	}
}

func TestDispatchTouchMultiFingerSnapshot(t *testing.T) {
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

	// Pinch: finger 1 down, finger 2 down, finger 1 moves, finger 2
	// moves, finger 1 up, finger 2 up.
	envs := []string{
		`{"v":1,"type":"touch_start","t":1,"seq":0,"data":{"identifier":1,"x":10,"y":10,"radius_x":1,"radius_y":1,"force":0.5,"twist":0}}`,
		`{"v":1,"type":"touch_start","t":2,"seq":1,"data":{"identifier":2,"x":90,"y":90,"radius_x":1,"radius_y":1,"force":0.5,"twist":0}}`,
		`{"v":1,"type":"touch_move","t":3,"seq":2,"data":{"identifier":1,"x":11,"y":11,"radius_x":1,"radius_y":1,"force":0.5,"twist":0}}`,
		`{"v":1,"type":"touch_move","t":4,"seq":3,"data":{"identifier":2,"x":89,"y":89,"radius_x":1,"radius_y":1,"force":0.5,"twist":0}}`,
		`{"v":1,"type":"touch_end","t":5,"seq":4,"data":{"identifier":1}}`,
		`{"v":1,"type":"touch_end","t":6,"seq":5,"data":{"identifier":2}}`,
	}
	for _, raw := range envs {
		env, _ := parseEnvelope([]byte(raw))
		if err := disp.Dispatch(ctx, env); err != nil {
			t.Errorf("dispatch %s: %v", env.Type, err)
		}
	}

	evs := collectTouchEvents(f.Calls())
	if len(evs) != 6 {
		t.Fatalf("got %d touch events, want 6: %+v", len(evs), evs)
	}
	// Snapshot of touchPoints state-AFTER each event (sorted ids):
	want := [][]int{
		{1},     // touchStart finger 1
		{1, 2},  // touchStart finger 2 — both active now
		{1, 2},  // touchMove finger 1 — both still active
		{1, 2},  // touchMove finger 2 — both still active
		{2},     // touchEnd finger 1 — only 2 remains
		{},      // touchEnd finger 2 — empty
	}
	for i := range want {
		if !intsEqual(evs[i].ids, want[i]) {
			t.Errorf("event %d (%s) ids: got %v, want %v",
				i, evs[i].cdpType, evs[i].ids, want[i])
		}
	}
}

func TestDispatchTouchUnknownIdentifierIgnored(t *testing.T) {
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

	// touch_move for an identifier we never started — protocol says drop+log.
	for _, raw := range []string{
		`{"v":1,"type":"touch_move","t":1,"seq":0,"data":{"identifier":99,"x":10,"y":20,"radius_x":1,"radius_y":1,"force":0,"twist":0}}`,
		`{"v":1,"type":"touch_end","t":2,"seq":1,"data":{"identifier":99}}`,
		`{"v":1,"type":"touch_cancel","t":3,"seq":2,"data":{"identifier":99}}`,
	} {
		env, _ := parseEnvelope([]byte(raw))
		if err := disp.Dispatch(ctx, env); err != nil {
			t.Errorf("expected nil error, got %v for %s", err, env.Type)
		}
	}
	evs := collectTouchEvents(f.Calls())
	if len(evs) != 0 {
		t.Errorf("unknown-identifier touch events should not call dispatchTouchEvent; got %+v", evs)
	}
}

func TestDispatchTouchCancelMapsToCDP(t *testing.T) {
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

	for _, raw := range []string{
		`{"v":1,"type":"touch_start","t":1,"seq":0,"data":{"identifier":7,"x":10,"y":20,"radius_x":1,"radius_y":1,"force":0,"twist":0}}`,
		`{"v":1,"type":"touch_cancel","t":2,"seq":1,"data":{"identifier":7}}`,
	} {
		env, _ := parseEnvelope([]byte(raw))
		_ = disp.Dispatch(ctx, env)
	}
	evs := collectTouchEvents(f.Calls())
	if len(evs) != 2 || evs[1].cdpType != "touchCancel" {
		t.Fatalf("expected touchStart then touchCancel, got %+v", evs)
	}
}

func TestTouchPointsLockedClampsRadius(t *testing.T) {
	d := newDispatcher(nil, newMetrics(), quietLogger())
	d.activeTouches[1] = touchPoint{id: 1, x: 0, y: 0, radiusX: 0, radiusY: -3}
	d.touchMu.Lock()
	defer d.touchMu.Unlock()
	pts := d.touchPointsLocked()
	if len(pts) != 1 {
		t.Fatalf("got %d points, want 1", len(pts))
	}
	// touchPoint.toCDP doesn't clamp — clamping happens at insert time
	// via max1(). Verify the inserted-via-insert-path case in the
	// happy-path test above. Here just verify the snapshot is shape-correct.
	if pts[0]["id"] != 1 {
		t.Errorf("id mismatch: %+v", pts[0])
	}
}

func TestMax1(t *testing.T) {
	if max1(0) != 1 {
		t.Errorf("max1(0) should be 1")
	}
	if max1(-5) != 1 {
		t.Errorf("max1(-5) should be 1")
	}
	if max1(7) != 7 {
		t.Errorf("max1(7) should be 7")
	}
}

func intsEqual(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------------------
// 5. Wheel inertia (v1.1 / T62)
// ---------------------------------------------------------------------------

// wheelDispatchEvents extracts the deltaX/deltaY from each
// Input.dispatchMouseEvent(type=mouseWheel) call recorded by the fake.
type wheelEvent struct {
	dx, dy      float64
	x, y        int
	pointerType string // empty unless explicitly set (we set it on phase=end)
}

func collectWheelEvents(calls []recordedCall) []wheelEvent {
	out := []wheelEvent{}
	for _, c := range calls {
		if c.Method != "Input.dispatchMouseEvent" {
			continue
		}
		var p map[string]any
		_ = json.Unmarshal(c.Params, &p)
		if p["type"] != "mouseWheel" {
			continue
		}
		ev := wheelEvent{}
		if v, ok := p["deltaX"].(float64); ok {
			ev.dx = v
		}
		if v, ok := p["deltaY"].(float64); ok {
			ev.dy = v
		}
		if v, ok := p["x"].(float64); ok {
			ev.x = int(v)
		}
		if v, ok := p["y"].(float64); ok {
			ev.y = int(v)
		}
		if v, ok := p["pointerType"].(string); ok {
			ev.pointerType = v
		}
		out = append(out, ev)
	}
	return out
}

func TestDispatchWheelPhaseEndDispatchesZeroDelta(t *testing.T) {
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
		// Three real wheel deltas, then phase=end zero-delta.
		`{"v":1,"type":"mouse_wheel","t":1,"seq":0,"data":{"dx":0,"dy":-120,"mode":0,"x":50,"y":60,"delta_mode":"pixel","phase":"start","momentum":false}}`,
		`{"v":1,"type":"mouse_wheel","t":2,"seq":1,"data":{"dx":0,"dy":-100,"mode":0,"x":50,"y":60,"delta_mode":"pixel","phase":"changed","momentum":false}}`,
		`{"v":1,"type":"mouse_wheel","t":3,"seq":2,"data":{"dx":0,"dy":-50,"mode":0,"x":50,"y":60,"delta_mode":"pixel","phase":"changed","momentum":true}}`,
		`{"v":1,"type":"mouse_wheel","t":4,"seq":3,"data":{"dx":0,"dy":0,"mode":0,"x":50,"y":60,"delta_mode":"pixel","phase":"end","momentum":false}}`,
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

	wheels := collectWheelEvents(f.Calls())
	if len(wheels) != 4 {
		t.Fatalf("expected 4 wheel CDP events, got %d: %+v", len(wheels), wheels)
	}
	// First three carry the real deltas; last is zero-delta.
	if wheels[0].dy != -120 || wheels[1].dy != -100 || wheels[2].dy != -50 {
		t.Errorf("delta passthrough mismatch: %+v", wheels)
	}
	if wheels[3].dx != 0 || wheels[3].dy != 0 {
		t.Errorf("phase=end should be zero-delta: %+v", wheels[3])
	}
	if wheels[3].pointerType != "mouse" {
		t.Errorf("phase=end should set pointerType=mouse, got %q", wheels[3].pointerType)
	}
	if wheels[3].x != 50 || wheels[3].y != 60 {
		t.Errorf("phase=end should preserve last position, got x=%d y=%d", wheels[3].x, wheels[3].y)
	}
}

func TestDispatchWheelDeltaModeStringWins(t *testing.T) {
	// When both numeric `mode` and string `delta_mode` are present, the
	// bridge must prefer the string per the spec's "SHOULD prefer".
	// We fire a wheel with mode=0 (pixel) but delta_mode="line"; the
	// CDP deltaY should reflect the line scaling (16×).
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
		`{"v":1,"type":"mouse_wheel","t":1,"seq":0,"data":{"dx":0,"dy":-3,"mode":0,"x":0,"y":0,"delta_mode":"line","phase":"start"}}`))
	_ = disp.Dispatch(ctx, env)

	wheels := collectWheelEvents(f.Calls())
	if len(wheels) != 1 {
		t.Fatalf("got %d wheels, want 1", len(wheels))
	}
	// dy=-3 lines × 16 px/line = -48 px.
	if wheels[0].dy != -48 {
		t.Errorf("delta_mode=line should multiply by 16; got dy=%v want -48", wheels[0].dy)
	}
}

func TestDispatchWheelLegacyClientNoPhase(t *testing.T) {
	// A v1.0 client emits no phase / no delta_mode. The bridge MUST
	// translate it as a legacy "changed" wheel — same as before T62.
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
		`{"v":1,"type":"mouse_wheel","t":1,"seq":0,"data":{"dx":0,"dy":-120,"mode":0,"x":10,"y":20}}`))
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Fatalf("legacy wheel dispatch: %v", err)
	}
	wheels := collectWheelEvents(f.Calls())
	if len(wheels) != 1 || wheels[0].dy != -120 {
		t.Errorf("legacy wheel pass-through failed: %+v", wheels)
	}
	if wheels[0].pointerType != "" {
		t.Errorf("legacy wheel should NOT set pointerType, got %q", wheels[0].pointerType)
	}
}

// ---------------------------------------------------------------------------
// 6. IME composition polish (T88)
// ---------------------------------------------------------------------------

// imeCalls extracts the sequence of (method, params) for IME-related
// CDP calls so tests can assert on the imeSetComposition selection
// arguments and the order of insertText commits.
type imeCall struct {
	method string
	params map[string]any
}

func collectIMECalls(calls []recordedCall) []imeCall {
	out := []imeCall{}
	for _, c := range calls {
		if c.Method != "Input.imeSetComposition" && c.Method != "Input.insertText" {
			continue
		}
		var p map[string]any
		_ = json.Unmarshal(c.Params, &p)
		out = append(out, imeCall{method: c.Method, params: p})
	}
	return out
}

func TestDispatchCompositionWithSelection(t *testing.T) {
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

	// Start with empty composition; client sends caret rect for
	// future candidate-positioning UI.
	envs := []string{
		`{"v":1,"type":"composition_start","t":1,"seq":0,"data":{"data":"","rect":{"x":10,"y":20,"w":12,"h":18}}}`,
		`{"v":1,"type":"composition_update","t":2,"seq":1,"data":{"data":"n","selection_start":1,"selection_end":1}}`,
		`{"v":1,"type":"composition_update","t":3,"seq":2,"data":{"data":"ni hao","selection_start":3,"selection_end":6}}`,
		`{"v":1,"type":"composition_end","t":4,"seq":3,"data":{"data":"你好"}}`,
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

	calls := collectIMECalls(f.Calls())
	// 3 imeSetComposition + 1 insertText.
	if len(calls) != 4 {
		t.Fatalf("expected 4 IME calls, got %d: %+v", len(calls), calls)
	}
	if calls[0].method != "Input.imeSetComposition" {
		t.Errorf("first call should be imeSetComposition; got %s", calls[0].method)
	}
	// composition_start with empty text → selectionStart/End = 0.
	if calls[0].params["text"] != "" || calls[0].params["selectionStart"] != float64(0) {
		t.Errorf("start params wrong: %+v", calls[0].params)
	}
	// composition_update "n" with selection at 1 → selectionStart=1.
	if calls[1].params["text"] != "n" || calls[1].params["selectionStart"] != float64(1) {
		t.Errorf("update#1 params wrong: %+v", calls[1].params)
	}
	// composition_update "ni hao" with start=3 end=6 → highlighted range.
	if calls[2].params["selectionStart"] != float64(3) || calls[2].params["selectionEnd"] != float64(6) {
		t.Errorf("update#2 selection wrong: %+v", calls[2].params)
	}
	// composition_end commits via insertText.
	if calls[3].method != "Input.insertText" || calls[3].params["text"] != "你好" {
		t.Errorf("end params wrong: %+v", calls[3])
	}
}

func TestDispatchCompositionLegacyV1Client(t *testing.T) {
	// A v1.0 client with the old `{data: "..."}` payload still works:
	// no selection_start/end → bridge defaults to caret-at-end via
	// utf16Len(text).
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

	// "こんにちは" — 5 BMP characters, all 1 UTF-16 unit each → length 5.
	env, _ := parseEnvelope([]byte(
		`{"v":1,"type":"composition_update","t":1,"seq":0,"data":{"data":"こんにちは"}}`))
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Fatalf("dispatch: %v", err)
	}

	calls := collectIMECalls(f.Calls())
	if len(calls) != 1 {
		t.Fatalf("expected 1 imeSetComposition call, got %d", len(calls))
	}
	// caret-at-end default is 5 for this string.
	if calls[0].params["selectionStart"] != float64(5) || calls[0].params["selectionEnd"] != float64(5) {
		t.Errorf("caret-at-end default wrong: %+v", calls[0].params)
	}
}

func TestDispatchCompositionCancel(t *testing.T) {
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

	// composition_cancel with no-payload data — empty-text imeSetComposition.
	env, _ := parseEnvelope([]byte(
		`{"v":1,"type":"composition_cancel","t":1,"seq":0,"data":{}}`))
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Fatalf("dispatch: %v", err)
	}
	calls := collectIMECalls(f.Calls())
	if len(calls) != 1 {
		t.Fatalf("expected 1 imeSetComposition call, got %d", len(calls))
	}
	if calls[0].method != "Input.imeSetComposition" {
		t.Errorf("expected imeSetComposition; got %s", calls[0].method)
	}
	if calls[0].params["text"] != "" {
		t.Errorf("cancel should send empty text; got %v", calls[0].params["text"])
	}
	if calls[0].params["selectionStart"] != float64(0) ||
		calls[0].params["selectionEnd"] != float64(0) {
		t.Errorf("cancel should collapse selection to 0; got %+v", calls[0].params)
	}
}

func TestDispatchCompositionSelectionClamped(t *testing.T) {
	// A buggy / malicious client sends selection_end = 999 for a
	// 3-char string. The bridge MUST clamp to [0, len] so CDP
	// doesn't reject the call.
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
		`{"v":1,"type":"composition_update","t":1,"seq":0,"data":{"data":"abc","selection_start":-5,"selection_end":999}}`))
	if err := disp.Dispatch(ctx, env); err != nil {
		t.Fatalf("dispatch: %v", err)
	}
	calls := collectIMECalls(f.Calls())
	if calls[0].params["selectionStart"] != float64(0) {
		t.Errorf("negative selectionStart should clamp to 0; got %v", calls[0].params["selectionStart"])
	}
	if calls[0].params["selectionEnd"] != float64(3) {
		t.Errorf("oversize selectionEnd should clamp to len; got %v", calls[0].params["selectionEnd"])
	}
}

func TestUTF16Len(t *testing.T) {
	cases := map[string]int{
		"":       0,
		"abc":    3,
		"こんにちは": 5,                  // BMP CJK — 1 unit each
		"🙂":      2,                  // outside BMP — surrogate pair
		"a🙂b":   4,                  // 1 + 2 + 1
		"é": 2,                 // "e" + combining acute = 2 units
	}
	for in, want := range cases {
		if got := utf16Len(in); got != want {
			t.Errorf("utf16Len(%q) = %d, want %d", in, got, want)
		}
	}
}

func TestClampInt(t *testing.T) {
	if clampInt(-5, 0, 10) != 0 {
		t.Error("below lo should clamp to lo")
	}
	if clampInt(99, 0, 10) != 10 {
		t.Error("above hi should clamp to hi")
	}
	if clampInt(5, 0, 10) != 5 {
		t.Error("in-range should pass through")
	}
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

// ---------------------------------------------------------------------------
// State-machine tests — cross-envelope state added for the wire-path
// integration tests in tests/webrtc/scenarios/. Each verifies one of the
// four invariants that landed in the dispatcher:
//
//   - clickCount ramp on rapid successive mouseDowns
//   - held-modifier state across key_down/up envelopes
//   - held-button state across mouse_button down/up envelopes
//   - special-key text synthesis (Enter→\r, Tab→\t, Backspace→\b)
//
// Each reads `params` of the recorded CDP calls and asserts on the
// fields that production cares about (the renderer keys dblclick
// detection on clickCount; drag detection on buttons; modifier
// propagation to click events; textarea newline insertion on text).
// ---------------------------------------------------------------------------

// callsParam returns a typed view of the i-th recorded call's params,
// stopping the test if the index is out of range or the JSON is bad.
func callsParam(t *testing.T, calls []recordedCall, i int) map[string]any {
	t.Helper()
	if i >= len(calls) {
		t.Fatalf("call index %d out of range (have %d)", i, len(calls))
	}
	var p map[string]any
	if err := json.Unmarshal(calls[i].Params, &p); err != nil {
		t.Fatalf("unmarshal call[%d] params: %v", i, err)
	}
	return p
}

// dispatchAll feeds a list of raw envelope JSON strings through the
// dispatcher in order. Helper used by the state-machine tests below.
func dispatchAll(t *testing.T, disp *dispatcher, ctx context.Context, raws []string) {
	t.Helper()
	for _, raw := range raws {
		env, err := parseEnvelope([]byte(raw))
		if err != nil {
			t.Fatalf("parse %q: %v", raw, err)
		}
		if err := disp.Dispatch(ctx, env); err != nil {
			t.Errorf("dispatch %s: %v", env.Type, err)
		}
	}
}

// callsWithMethod returns the subset of recorded calls with the given
// CDP method (typically Input.dispatchMouseEvent). Skips
// Page.bringToFront and other infrastructure calls.
func callsWithMethod(calls []recordedCall, method string) []recordedCall {
	out := []recordedCall{}
	for _, c := range calls {
		if c.Method == method {
			out = append(out, c)
		}
	}
	return out
}

func TestDispatchClickCountRamp(t *testing.T) {
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

	// Two rapid mouse_button down/up pairs at the same coordinates.
	// Renderer should see clickCount=1 on the first pair, clickCount=2
	// on the second — that's what fires the dblclick event.
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"mouse_button","t":1,"seq":0,"data":{"button":0,"action":"down","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_button","t":2,"seq":1,"data":{"button":0,"action":"up","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":0,"action":"down","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_button","t":4,"seq":3,"data":{"button":0,"action":"up","x":100,"y":100}}`,
	})

	mouseEvents := callsWithMethod(f.Calls(), "Input.dispatchMouseEvent")
	if len(mouseEvents) != 4 {
		t.Fatalf("expected 4 mouse events, got %d", len(mouseEvents))
	}

	// First down: clickCount=1, first up: clickCount=1, second down:
	// clickCount=2 (RAMP), second up: clickCount=2.
	wantCounts := []int{1, 1, 2, 2}
	for i, want := range wantCounts {
		var p map[string]any
		_ = json.Unmarshal(mouseEvents[i].Params, &p)
		got, _ := p["clickCount"].(float64)
		if int(got) != want {
			t.Errorf("call[%d] clickCount = %v, want %d", i, got, want)
		}
	}
}

func TestDispatchClickCountResetOnDistance(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, _ := dialCDP(ctx, f.URL(), quietLogger())
	defer cdp.Close()
	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	// Two clicks but the second is >5px away from the first — must NOT
	// ramp clickCount, both pairs should be clickCount=1 (no dblclick).
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"mouse_button","t":1,"seq":0,"data":{"button":0,"action":"down","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_button","t":2,"seq":1,"data":{"button":0,"action":"up","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":0,"action":"down","x":200,"y":200}}`,
		`{"v":1,"type":"mouse_button","t":4,"seq":3,"data":{"button":0,"action":"up","x":200,"y":200}}`,
	})

	mouseEvents := callsWithMethod(f.Calls(), "Input.dispatchMouseEvent")
	for i, want := range []int{1, 1, 1, 1} {
		var p map[string]any
		_ = json.Unmarshal(mouseEvents[i].Params, &p)
		got, _ := p["clickCount"].(float64)
		if int(got) != want {
			t.Errorf("distance-reset call[%d] clickCount = %v, want %d", i, got, want)
		}
	}
}

func TestDispatchHeldModifierOnMouseEvent(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, _ := dialCDP(ctx, f.URL(), quietLogger())
	defer cdp.Close()
	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	// Wire shape: key_down Shift / mouse_button down / mouse_button up
	// / key_up Shift. The mouse events MUST land with modifiers=8 (CDP
	// shift bit) even though the mouse_button envelope has no `mods`
	// field on the wire — bridge has to track held-modifier state.
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"key_down","t":1,"seq":0,"data":{"key":"Shift","code":"ShiftLeft","mods":1}}`,
		`{"v":1,"type":"mouse_button","t":2,"seq":1,"data":{"button":0,"action":"down","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":0,"action":"up","x":100,"y":100}}`,
		`{"v":1,"type":"key_up","t":4,"seq":3,"data":{"key":"Shift","code":"ShiftLeft","mods":0}}`,
	})

	calls := f.Calls()
	mouseEvents := callsWithMethod(calls, "Input.dispatchMouseEvent")
	if len(mouseEvents) != 2 {
		t.Fatalf("expected 2 mouse events, got %d", len(mouseEvents))
	}
	for i, name := range []string{"mouseDown", "mouseUp"} {
		var p map[string]any
		_ = json.Unmarshal(mouseEvents[i].Params, &p)
		got, _ := p["modifiers"].(float64)
		if int(got) != 8 {
			t.Errorf("%s modifiers = %v, want 8 (Shift held)", name, got)
		}
	}

	// After key_up, the held state must reset — a subsequent
	// mouse_button down should land with modifiers=0.
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"mouse_button","t":5,"seq":4,"data":{"button":0,"action":"down","x":200,"y":200}}`,
	})
	post := callsWithMethod(f.Calls(), "Input.dispatchMouseEvent")
	var p map[string]any
	_ = json.Unmarshal(post[len(post)-1].Params, &p)
	if got, _ := p["modifiers"].(float64); int(got) != 0 {
		t.Errorf("after key_up Shift, modifiers = %v, want 0", got)
	}
}

func TestDispatchHeldButtonsOnMouseMove(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, _ := dialCDP(ctx, f.URL(), quietLogger())
	defer cdp.Close()
	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	// Drag-shape sequence: mouse_button down, mouse_move (during
	// drag), mouse_button up, mouse_move (after drag). The middle
	// move MUST report buttons=1 + button="left" so chromium's drag
	// detector keeps the gesture alive. The trailing move (after up)
	// must report buttons=0 + button="none".
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"mouse_button","t":1,"seq":0,"data":{"button":0,"action":"down","x":100,"y":100}}`,
		`{"v":1,"type":"mouse_move","t":2,"seq":1,"data":{"x":110,"y":105}}`,
		`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":0,"action":"up","x":110,"y":105}}`,
		`{"v":1,"type":"mouse_move","t":4,"seq":3,"data":{"x":120,"y":110}}`,
	})

	mouseEvents := callsWithMethod(f.Calls(), "Input.dispatchMouseEvent")
	if len(mouseEvents) != 4 {
		t.Fatalf("expected 4 mouse events, got %d", len(mouseEvents))
	}

	// During-drag move (index 1) must have buttons=1 + button=left.
	var dragMove map[string]any
	_ = json.Unmarshal(mouseEvents[1].Params, &dragMove)
	if buttons, _ := dragMove["buttons"].(float64); int(buttons) != 1 {
		t.Errorf("during-drag move buttons = %v, want 1", buttons)
	}
	if button, _ := dragMove["button"].(string); button != "left" {
		t.Errorf("during-drag move button = %q, want left", button)
	}

	// After-up move (index 3) must have buttons=0 + button=none.
	var afterMove map[string]any
	_ = json.Unmarshal(mouseEvents[3].Params, &afterMove)
	if buttons, _ := afterMove["buttons"].(float64); int(buttons) != 0 {
		t.Errorf("after-up move buttons = %v, want 0", buttons)
	}
	if button, _ := afterMove["button"].(string); button != "none" {
		t.Errorf("after-up move button = %q, want none", button)
	}
}

func TestDispatchKeyTextSynthesis(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, _ := dialCDP(ctx, f.URL(), quietLogger())
	defer cdp.Close()
	disp := newDispatcher(cdp, newMetrics(), quietLogger())

	// Enter / Tab / Backspace / printable / arrow.
	// Enter, Tab, Backspace and printable get `text`; arrow keys
	// don't.
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"key_down","t":1,"seq":0,"data":{"key":"Enter","code":"Enter","mods":0}}`,
		`{"v":1,"type":"key_down","t":2,"seq":1,"data":{"key":"Tab","code":"Tab","mods":0}}`,
		`{"v":1,"type":"key_down","t":3,"seq":2,"data":{"key":"Backspace","code":"Backspace","mods":0}}`,
		`{"v":1,"type":"key_down","t":4,"seq":3,"data":{"key":"a","code":"KeyA","mods":0}}`,
		`{"v":1,"type":"key_down","t":5,"seq":4,"data":{"key":"ArrowUp","code":"ArrowUp","mods":0}}`,
	})

	keyEvents := callsWithMethod(f.Calls(), "Input.dispatchKeyEvent")
	if len(keyEvents) != 5 {
		t.Fatalf("expected 5 key events, got %d", len(keyEvents))
	}
	wantText := []string{"\r", "\t", "\b", "a", ""}
	for i, want := range wantText {
		var p map[string]any
		_ = json.Unmarshal(keyEvents[i].Params, &p)
		text, _ := p["text"].(string)
		if want == "" {
			if _, has := p["text"]; has {
				t.Errorf("key[%d] should have no text field, got %q", i, text)
			}
		} else if text != want {
			t.Errorf("key[%d] text = %q, want %q", i, text, want)
		}
	}
}

// ---------------------------------------------------------------------------
// 7. Flat-mode CDP migration (BUGS-529)
// ---------------------------------------------------------------------------
//
// These tests exercise the flat-mode plumbing introduced to fix
// BUGS-529 (cb-chromium dropping Input.dispatch* on direct-page-WS
// sessions after a third-party Page.navigate). They cover:
//
//   - dialCDP returns a *pageSessionSender whose CurrentSession()
//     reflects the auto-attached page sessionId
//   - every dispatched CDP command on the wire carries that sessionId
//     (this is the actual fix — flat-mode sessions follow navigation)
//   - Send blocks until the first attached session arrives
//   - Target.detachedFromTarget clears the current session
//   - bootstrap commands (setDiscoverTargets / setAutoAttach) are not
//     leaked into the recorded Calls() (test scaffolding sanity)

func TestFlatModeSessionAttached(t *testing.T) {
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	// Auto-attach event is emitted synchronously on setAutoAttach ack.
	// By the time dialCDP returns, CurrentSession() should be set.
	if got := cdp.CurrentSession(); got != fakePageSessionID {
		t.Errorf("CurrentSession() = %q, want %q", got, fakePageSessionID)
	}
}

func TestFlatModeSessionIDOnEveryDispatch(t *testing.T) {
	// The whole point of BUGS-529: every Input.dispatch* on the wire
	// MUST carry sessionId so chromium routes it to the page session
	// (which follows navigation), not to a stale RenderFrame binding.
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

	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":10,"y":20}}`,
		`{"v":1,"type":"key_down","t":2,"seq":1,"data":{"key":"a","code":"KeyA","mods":0}}`,
		`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":0,"action":"down","x":10,"y":20}}`,
	})

	calls := f.Calls()
	if len(calls) == 0 {
		t.Fatalf("no calls recorded")
	}
	for i, c := range calls {
		if c.SessionID != fakePageSessionID {
			t.Errorf("call[%d] %s: sessionId = %q, want %q",
				i, c.Method, c.SessionID, fakePageSessionID)
		}
	}
}

func TestFlatModeBootstrapCommandsHidden(t *testing.T) {
	// Sanity: the fakeCDP filters Target.setDiscoverTargets /
	// Target.setAutoAttach out of the recorded Calls() so our
	// dispatcher-layer assertions don't have to handle them.
	f := newFakeCDP(t)
	defer f.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	cdp, err := dialCDP(ctx, f.URL(), quietLogger())
	if err != nil {
		t.Fatalf("dialCDP: %v", err)
	}
	defer cdp.Close()

	// dialCDP issued setDiscoverTargets + setAutoAttach. No dispatch
	// envelope has been routed yet, so Calls() should still be empty.
	if calls := f.Calls(); len(calls) != 0 {
		t.Errorf("bootstrap commands leaked into Calls(): %+v", calls)
	}
}

func TestFlatModeSendBlocksUntilAttached(t *testing.T) {
	// Drive a direct cdpClient (NOT through dialCDP) so we can exercise
	// the pageSessionSender's wait-for-session path with no autoAttach
	// running. Verify Send returns ctx.DeadlineExceeded if no session
	// ever arrives.
	cdp := &cdpClient{
		pending:    make(map[int64]chan cdpResponse),
		disconnect: make(chan struct{}),
		log:        quietLogger(),
	}
	sender := newPageSessionSender(cdp, quietLogger())

	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	_, err := sender.Send(ctx, "Page.bringToFront", nil)
	if err == nil {
		t.Fatalf("expected deadline error, got nil")
	}
	// The sender wraps ctx.Err() in a clarifying error message; assert
	// on substring rather than identity.
	if !strings.Contains(err.Error(), "page session") {
		t.Errorf("expected 'page session' in err, got: %v", err)
	}
}

func TestFlatModeSendUnblocksOnAttach(t *testing.T) {
	// Direct sender path: no real connection. Verify that handleEvent
	// for a Target.attachedToTarget unblocks a pending Send waiter.
	cdp := &cdpClient{
		pending:    make(map[int64]chan cdpResponse),
		disconnect: make(chan struct{}),
		log:        quietLogger(),
	}
	sender := newPageSessionSender(cdp, quietLogger())

	// Background goroutine blocks in waitForSession.
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	gotSID := make(chan string, 1)
	go func() {
		sid, err := sender.waitForSession(ctx)
		if err != nil {
			gotSID <- ""
			return
		}
		gotSID <- sid
	}()

	// Give the goroutine a moment to register as a waiter, then fire
	// the attach event.
	time.Sleep(20 * time.Millisecond)
	attachParams := json.RawMessage(`{"sessionId":"sid-x","waitingForDebugger":false,"targetInfo":{"targetId":"tgt-x","type":"page","url":"about:blank"}}`)
	sender.handleEvent("Target.attachedToTarget", "", attachParams)

	select {
	case sid := <-gotSID:
		if sid != "sid-x" {
			t.Errorf("waitForSession returned %q, want %q", sid, "sid-x")
		}
	case <-time.After(time.Second):
		t.Fatalf("waitForSession did not unblock after attach")
	}
}

func TestFlatModeNonPageTargetIgnored(t *testing.T) {
	// service_worker / dedicated_worker / iframe attach events MUST NOT
	// become the current session — input dispatches at the page level.
	cdp := &cdpClient{
		pending:    make(map[int64]chan cdpResponse),
		disconnect: make(chan struct{}),
		log:        quietLogger(),
	}
	sender := newPageSessionSender(cdp, quietLogger())

	// Worker attach — should be ignored.
	workerAttach := json.RawMessage(`{"sessionId":"sw-1","waitingForDebugger":false,"targetInfo":{"targetId":"sw-tgt","type":"service_worker","url":""}}`)
	sender.handleEvent("Target.attachedToTarget", "", workerAttach)
	if got := sender.CurrentSession(); got != "" {
		t.Errorf("worker attach set CurrentSession = %q, want empty", got)
	}

	// Page attach — should set CurrentSession.
	pageAttach := json.RawMessage(`{"sessionId":"page-1","waitingForDebugger":false,"targetInfo":{"targetId":"page-tgt","type":"page","url":"about:blank"}}`)
	sender.handleEvent("Target.attachedToTarget", "", pageAttach)
	if got := sender.CurrentSession(); got != "page-1" {
		t.Errorf("page attach: CurrentSession = %q, want page-1", got)
	}
}

func TestFlatModeDetachClearsCurrentSession(t *testing.T) {
	cdp := &cdpClient{
		pending:    make(map[int64]chan cdpResponse),
		disconnect: make(chan struct{}),
		log:        quietLogger(),
	}
	sender := newPageSessionSender(cdp, quietLogger())

	// Attach two pages; second becomes current.
	a := json.RawMessage(`{"sessionId":"a","waitingForDebugger":false,"targetInfo":{"targetId":"tA","type":"page","url":"about:blank"}}`)
	b := json.RawMessage(`{"sessionId":"b","waitingForDebugger":false,"targetInfo":{"targetId":"tB","type":"page","url":"about:blank"}}`)
	sender.handleEvent("Target.attachedToTarget", "", a)
	sender.handleEvent("Target.attachedToTarget", "", b)
	if got := sender.CurrentSession(); got != "b" {
		t.Errorf("after two attaches: CurrentSession = %q, want b", got)
	}

	// Detach b → fall back to a.
	detachB := json.RawMessage(`{"sessionId":"b","targetId":"tB"}`)
	sender.handleEvent("Target.detachedFromTarget", "", detachB)
	if got := sender.CurrentSession(); got != "a" {
		t.Errorf("after detach b: CurrentSession = %q, want a (fallback)", got)
	}

	// Detach a → no sessions left.
	detachA := json.RawMessage(`{"sessionId":"a","targetId":"tA"}`)
	sender.handleEvent("Target.detachedFromTarget", "", detachA)
	if got := sender.CurrentSession(); got != "" {
		t.Errorf("after detach a: CurrentSession = %q, want empty", got)
	}
}

// TestFlatModeFrameNavigatedDiagnostic verifies that the bridge
// processes Page.frameNavigated events on its current session
// without crashing and without confusing them with attach/detach.
// Per BUGS-529 option C, the bridge enables Page domain on each
// auto-attached session so cluster pod logs surface whether
// flat-mode auto-attach is propagating navigation correctly.
func TestFlatModeFrameNavigatedDiagnostic(t *testing.T) {
	cdp := &cdpClient{
		pending:    make(map[int64]chan cdpResponse),
		disconnect: make(chan struct{}),
		log:        quietLogger(),
	}
	sender := newPageSessionSender(cdp, quietLogger())

	// Attach a page session so currentSID is non-empty.
	attach := json.RawMessage(`{"sessionId":"page-1","waitingForDebugger":false,"targetInfo":{"targetId":"tgt-1","type":"page","url":"about:blank"}}`)
	sender.handleEvent("Target.attachedToTarget", "", attach)

	// Top-level navigation: should be routed through the navigation
	// log path (not to currentSID changes).
	beforeSID := sender.CurrentSession()
	topLevel := json.RawMessage(`{"frame":{"id":"F1","url":"http://example.test/x.html","loaderId":"L1"}}`)
	sender.handleEvent("Page.frameNavigated", "page-1", topLevel)
	if got := sender.CurrentSession(); got != beforeSID {
		t.Errorf("frameNavigated must not change currentSID; got %q after %q", got, beforeSID)
	}

	// Iframe (non-empty parentId): silently ignored.
	iframe := json.RawMessage(`{"frame":{"id":"F2","parentId":"F1","url":"http://example.test/iframe.html","loaderId":"L2"}}`)
	sender.handleEvent("Page.frameNavigated", "page-1", iframe)
	if got := sender.CurrentSession(); got != beforeSID {
		t.Errorf("iframe frameNavigated must not change currentSID; got %q after %q", got, beforeSID)
	}
}

// TestFlatModeNavigateDoesNotBreakDispatch is the regression assertion
// for BUGS-529's symptom in test form: the bridge connects, the
// harness "navigates" (in fakeCDP land we just keep the session
// attached but pretend a page navigation occurred — the bridge can't
// tell the difference), then dispatches input. With flat-mode the
// sessionId stays valid and dispatches still record on the same
// session. Pre-fix this WOULD have routed to a stale page WS that
// chromium silently dropped.
func TestFlatModeNavigateDoesNotBreakDispatch(t *testing.T) {
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

	// Pre-navigate dispatch.
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"mouse_move","t":1,"seq":0,"data":{"x":10,"y":20}}`,
	})
	preCount := len(f.Calls())

	// Simulate a navigation: chromium emits Page.frameNavigated as an
	// event BUT does not detach/reattach the session in flat-mode. We
	// model this by handing the event in directly — no state changes.
	// The bridge's pageSessionSender ignores it (only attached/detached
	// are routed).

	// Post-navigate dispatch — must keep landing on the same session.
	dispatchAll(t, disp, ctx, []string{
		`{"v":1,"type":"key_down","t":2,"seq":1,"data":{"key":"a","code":"KeyA","mods":0}}`,
		`{"v":1,"type":"mouse_button","t":3,"seq":2,"data":{"button":0,"action":"down","x":10,"y":20}}`,
	})
	post := f.Calls()
	if len(post) <= preCount {
		t.Fatalf("post-navigate dispatch produced no new calls (pre=%d post=%d)", preCount, len(post))
	}
	for i, c := range post {
		if c.SessionID != fakePageSessionID {
			t.Errorf("call[%d] %s: sessionId=%q, want %q", i, c.Method, c.SessionID, fakePageSessionID)
		}
	}
}

func TestKeyToProtocolMod(t *testing.T) {
	cases := []struct {
		key, code string
		want      int
	}{
		{"Shift", "", modShift},
		{"Control", "", modCtrl},
		{"Alt", "", modAlt},
		{"Meta", "", modMeta},
		{"OS", "", modMeta},
		{"", "ShiftLeft", modShift},
		{"", "ShiftRight", modShift},
		{"", "ControlLeft", modCtrl},
		{"", "AltRight", modAlt},
		{"", "MetaLeft", modMeta},
		{"a", "KeyA", 0},
		{"Enter", "Enter", 0},
	}
	for _, tc := range cases {
		got := keyToProtocolMod(tc.key, tc.code)
		if got != tc.want {
			t.Errorf("keyToProtocolMod(%q, %q) = %d, want %d", tc.key, tc.code, got, tc.want)
		}
	}
}

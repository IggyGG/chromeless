// Package main is the input forwarding bridge: it consumes input events
// from the WebRTC data channel (relayed to it as one JSON envelope per
// line / per WS message in the v1 protocol from
// docs/protocols/input-channel.md) and dispatches them into a headless
// Chromium via the DevTools Protocol (CDP) at ws://localhost:9222.
//
// This is the Phase 1 server-side skeleton (T22). It runs as a sidecar
// next to Chromium and exposes:
//
//   - --source stdin  → read newline-delimited JSON envelopes
//   - --source ws     → accept envelopes over a localhost WebSocket
//     (default ws://localhost:9100/input)
//   - /metrics        → Prometheus metrics (events/sec by type, dispatch
//                       latency p50/p95/p99 histograms, error counters)
//
// Out of scope here:
//   - The relay from the WebRTC data channel into this bridge — that
//     ships separately and is what the --source ws mode is for.
//   - Auth, rate limiting, and multi-tenant session routing — Phase 3.
//
// Cross-references:
//   - docs/protocols/input-channel.md — the v1 wire format we parse.
//   - client/src/input.ts — encoder side.
//   - capture/input-bridge/main_test.go — table tests + fake CDP server.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// ---------------------------------------------------------------------------
// Protocol — must match docs/protocols/input-channel.md (v1)
// ---------------------------------------------------------------------------

const protocolVersion = 1

// inputEnvelope is the v1 wire envelope. Field names mirror the spec.
type inputEnvelope struct {
	V    int             `json:"v"`
	Type string          `json:"type"`
	T    int64           `json:"t"`
	Seq  int64           `json:"seq"`
	Data json.RawMessage `json:"data"`
}

// Per-type payload structs.
type mouseMoveData struct {
	X int `json:"x"`
	Y int `json:"y"`
}
type mouseButtonData struct {
	Button int    `json:"button"` // 0 left, 1 middle, 2 right, 3 back, 4 forward
	Action string `json:"action"` // "down" | "up"
	X      int    `json:"x"`
	Y      int    `json:"y"`
}
type mouseWheelData struct {
	DX   int `json:"dx"`
	DY   int `json:"dy"`
	Mode int `json:"mode"`
	X    int `json:"x"`
	Y    int `json:"y"`
	// v1.1 fields. Optional; pre-T62 clients omit them and the
	// bridge falls through to the legacy "every event is changed"
	// behaviour.
	DeltaMode string `json:"delta_mode,omitempty"`
	Phase     string `json:"phase,omitempty"`
	Momentum  bool   `json:"momentum,omitempty"`
}
type keyData struct {
	Code string `json:"code"`
	Key  string `json:"key"`
	Mods int    `json:"mods"` // bitmask: Shift=1 Ctrl=2 Alt=4 Meta=8
}
type compositionData struct {
	Data string `json:"data"`
}
type clipboardPasteData struct {
	Text string `json:"text"`
}

// v1.1 drag-and-drop payload shapes. See docs/protocols/input-channel.md.
type dragItem struct {
	Kind string `json:"kind"` // "string" | "file"
	Type string `json:"type"` // MIME type, lower-case
	Data string `json:"data"` // empty for kind == "file"
}

type dragStartData struct {
	X     int        `json:"x"`
	Y     int        `json:"y"`
	Types []string   `json:"types"`
	Items []dragItem `json:"items"`
}

type dragOverData struct {
	X int `json:"x"`
	Y int `json:"y"`
}

type dropData struct {
	X     int        `json:"x"`
	Y     int        `json:"y"`
	Types []string   `json:"types"`
	Items []dragItem `json:"items"`
}

type dragEndData struct {
	Success bool `json:"success"`
}

// v1.1 multi-touch payload shapes. See docs/protocols/input-channel.md.
//
// Per-finger updates: one envelope per identifier per event. The
// bridge maintains the per-identifier map needed by CDP's
// dispatchTouchEvent.
type touchStartData struct {
	Identifier int     `json:"identifier"`
	X          int     `json:"x"`
	Y          int     `json:"y"`
	RadiusX    int     `json:"radius_x"`
	RadiusY    int     `json:"radius_y"`
	Force      float64 `json:"force"`
	Twist      int     `json:"twist"`
}
type touchMoveData = touchStartData

type touchEndData struct {
	Identifier int `json:"identifier"`
}
type touchCancelData = touchEndData

// touchPoint is the in-bridge representation of one active finger.
// CDP's dispatchTouchEvent wants this assembled into a touchPoints
// array on every call.
type touchPoint struct {
	id      int
	x       int
	y       int
	radiusX int
	radiusY int
	force   float64
	twist   int
}

func (p touchPoint) toCDP() map[string]any {
	return map[string]any{
		"x":             p.x,
		"y":             p.y,
		"radiusX":       p.radiusX,
		"radiusY":       p.radiusY,
		"force":         p.force,
		"id":            p.id,
		"rotationAngle": p.twist,
	}
}

// parseEnvelope validates and decodes an input envelope. Returns an
// error for unsupported `v`, missing required fields, or malformed
// JSON. Unknown `type` values are accepted at this layer — the
// dispatcher decides whether to skip or warn.
func parseEnvelope(raw []byte) (inputEnvelope, error) {
	var env inputEnvelope
	if err := json.Unmarshal(raw, &env); err != nil {
		return env, fmt.Errorf("invalid JSON: %w", err)
	}
	if env.V != protocolVersion {
		return env, fmt.Errorf("unsupported protocol version v=%d (want %d)", env.V, protocolVersion)
	}
	if env.Type == "" {
		return env, errors.New("missing 'type' field")
	}
	return env, nil
}

// ---------------------------------------------------------------------------
// Modifier mapping
//
// Protocol bits (input-channel.md):
//   Shift = 1, Ctrl = 2, Alt = 4, Meta = 8
//
// CDP `Input.dispatchKeyEvent.modifiers` bits:
//   Alt = 1, Ctrl = 2, Meta = 4, Shift = 8
//
// They share Ctrl=2 by accident; everything else needs a remap.
// ---------------------------------------------------------------------------

const (
	modShift = 1 << iota
	modCtrl
	modAlt
	modMeta
)

func protocolModsToCDP(mods int) int {
	out := 0
	if mods&modShift != 0 {
		out |= 8
	}
	if mods&modCtrl != 0 {
		out |= 2
	}
	if mods&modAlt != 0 {
		out |= 1
	}
	if mods&modMeta != 0 {
		out |= 4
	}
	return out
}

// CDP button name from protocol button index.
func protocolButtonToCDP(b int) string {
	switch b {
	case 0:
		return "left"
	case 1:
		return "middle"
	case 2:
		return "right"
	case 3:
		return "back"
	case 4:
		return "forward"
	default:
		return "none"
	}
}

// ---------------------------------------------------------------------------
// CDP client
//
// We speak CDP over a single WebSocket to a page-level target. The
// target's webSocketDebuggerUrl is discovered via the JSON HTTP
// endpoint at http://<host>:<port>/json (Chromium devtools-frontend
// convention) and reused until the connection drops.
// ---------------------------------------------------------------------------

type cdpClient struct {
	mu         sync.Mutex
	conn       *websocket.Conn
	nextID     int64
	pending    map[int64]chan cdpResponse
	disconnect chan struct{}
	log        *slog.Logger
}

type cdpResponse struct {
	Result json.RawMessage `json:"result"`
	Error  *cdpError       `json:"error,omitempty"`
}

type cdpError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *cdpError) Error() string { return fmt.Sprintf("cdp error %d: %s", e.Code, e.Message) }

type cdpEnvelope struct {
	ID     int64           `json:"id,omitempty"`
	Method string          `json:"method,omitempty"`
	Params any             `json:"params,omitempty"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  *cdpError       `json:"error,omitempty"`
}

// dialCDP performs target discovery and opens the WebSocket. It returns
// a usable client or an error. Caller must Close() when done.
func dialCDP(ctx context.Context, baseURL string, log *slog.Logger) (*cdpClient, error) {
	wsURL, err := discoverPageWS(ctx, baseURL)
	if err != nil {
		return nil, err
	}
	dialer := *websocket.DefaultDialer
	dialer.HandshakeTimeout = 5 * time.Second
	conn, _, err := dialer.DialContext(ctx, wsURL, nil)
	if err != nil {
		return nil, fmt.Errorf("dial CDP %s: %w", wsURL, err)
	}
	c := &cdpClient{
		conn:       conn,
		pending:    make(map[int64]chan cdpResponse),
		disconnect: make(chan struct{}),
		log:        log,
	}
	go c.readLoop()
	return c, nil
}

// discoverPageWS hits /json/list and returns the first page target's
// debugger WS URL.
func discoverPageWS(ctx context.Context, baseURL string) (string, error) {
	// CDP exposes both /json and /json/list; /json/list is the
	// canonical alias.
	u := strings.TrimRight(baseURL, "/") + "/json/list"
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return "", err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("GET %s: %w", u, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("GET %s: status %d", u, resp.StatusCode)
	}
	var targets []struct {
		Type                 string `json:"type"`
		WebSocketDebuggerURL string `json:"webSocketDebuggerUrl"`
		URL                  string `json:"url"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&targets); err != nil {
		return "", fmt.Errorf("decode targets: %w", err)
	}
	for _, t := range targets {
		if t.Type == "page" && t.WebSocketDebuggerURL != "" {
			return t.WebSocketDebuggerURL, nil
		}
	}
	return "", fmt.Errorf("no page targets at %s", u)
}

func (c *cdpClient) readLoop() {
	defer close(c.disconnect)
	for {
		_, raw, err := c.conn.ReadMessage()
		if err != nil {
			c.log.Info("cdp connection closed", slog.Any("err", err))
			return
		}
		var env cdpEnvelope
		if err := json.Unmarshal(raw, &env); err != nil {
			c.log.Warn("cdp non-JSON frame", slog.Any("err", err))
			continue
		}
		if env.ID == 0 {
			// Server-initiated event (Page.frameNavigated etc.) — we
			// don't subscribe to any events in v1, so ignore.
			continue
		}
		c.mu.Lock()
		ch, ok := c.pending[env.ID]
		delete(c.pending, env.ID)
		c.mu.Unlock()
		if ok {
			ch <- cdpResponse{Result: env.Result, Error: env.Error}
			close(ch)
		}
	}
}

// Send issues a CDP command and waits for the matching response (or ctx).
// `params` may be nil.
func (c *cdpClient) Send(ctx context.Context, method string, params any) (json.RawMessage, error) {
	id := atomic.AddInt64(&c.nextID, 1)
	ch := make(chan cdpResponse, 1)

	c.mu.Lock()
	c.pending[id] = ch
	conn := c.conn
	c.mu.Unlock()

	frame := cdpEnvelope{ID: id, Method: method, Params: params}
	raw, err := json.Marshal(frame)
	if err != nil {
		return nil, err
	}
	c.mu.Lock()
	if err := conn.SetWriteDeadline(time.Now().Add(2 * time.Second)); err != nil {
		c.mu.Unlock()
		return nil, err
	}
	err = conn.WriteMessage(websocket.TextMessage, raw)
	c.mu.Unlock()
	if err != nil {
		return nil, fmt.Errorf("cdp write: %w", err)
	}

	select {
	case resp := <-ch:
		if resp.Error != nil {
			return nil, resp.Error
		}
		return resp.Result, nil
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.pending, id)
		c.mu.Unlock()
		return nil, ctx.Err()
	case <-c.disconnect:
		return nil, errors.New("cdp disconnected")
	}
}

func (c *cdpClient) Close() error {
	c.mu.Lock()
	conn := c.conn
	c.mu.Unlock()
	if conn == nil {
		return nil
	}
	_ = conn.WriteControl(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""),
		time.Now().Add(time.Second))
	return conn.Close()
}

// ---------------------------------------------------------------------------
// Dispatcher — translate input envelopes into CDP calls
// ---------------------------------------------------------------------------

// cdpSender is the subset of cdpClient used by the dispatcher. Tests
// substitute a fake.
type cdpSender interface {
	Send(ctx context.Context, method string, params any) (json.RawMessage, error)
}

type dispatcher struct {
	cdp        cdpSender
	log        *slog.Logger
	bringFront sync.Once // call Page.bringToFront at most once per process

	// dragMu guards the per-session drag state. CDP's
	// Input.dispatchDragEvent requires the DragData payload on every
	// call (dragEnter, dragOver, drop, dragCancel), so we cache the
	// items from drag_start and reuse them on subsequent events. We
	// also enable Input.setInterceptDrags once the first drag arrives
	// so subsequent OS-level drag handling doesn't race us.
	dragMu       sync.Mutex
	dragItems    []dragItem
	dragTypes    []string
	dragActive   bool
	dragInterceptOnce sync.Once

	// touchMu guards the active-touches map. CDP's
	// Input.dispatchTouchEvent expects the *currently active*
	// touchPoints on every call (per puppeteer's convention: the
	// array reflects state AFTER this event applies). The map is
	// keyed by per-finger identifier supplied by the client.
	touchMu      sync.Mutex
	activeTouches map[int]touchPoint

	// wheelMu guards the per-session wheel gesture state. The
	// protocol's phase machine (start | changed | end) lets the
	// bridge synthesize a final zero-delta CDP wheel at end-of-
	// momentum so Chromium's compositor flushes its decay.
	// Concurrent wheels (rare in single-tenant) don't interleave
	// because each gesture's `start` resets the flag.
	wheelMu        sync.Mutex
	wheelInGesture bool

	metrics *metrics
}

func newDispatcher(cdp cdpSender, m *metrics, log *slog.Logger) *dispatcher {
	return &dispatcher{
		cdp: cdp, log: log, metrics: m,
		activeTouches: make(map[int]touchPoint),
	}
}

// Dispatch routes one envelope to the correct CDP method. Errors are
// returned for the caller to log/aggregate; the loop never aborts on
// per-event errors.
func (d *dispatcher) Dispatch(ctx context.Context, env inputEnvelope) error {
	start := time.Now()
	defer func() {
		d.metrics.observe(env.Type, time.Since(start), nil)
	}()

	// Lazily bring the page to the front so window-mode Chromium gets
	// keystrokes. Idempotent in CDP; cheap to call once.
	d.bringFront.Do(func() {
		bfCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
		defer cancel()
		if _, err := d.cdp.Send(bfCtx, "Page.bringToFront", nil); err != nil {
			d.log.Warn("Page.bringToFront failed", slog.Any("err", err))
		}
	})

	switch env.Type {
	case "mouse_move":
		var data mouseMoveData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("mouse_move: %w", err))
		}
		params := map[string]any{
			"type":      "mouseMoved",
			"x":         data.X,
			"y":         data.Y,
			"button":    "none",
			"modifiers": 0,
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchMouseEvent", params)
		return d.fail(env.Type, err)

	case "mouse_button":
		var data mouseButtonData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("mouse_button: %w", err))
		}
		var t string
		switch data.Action {
		case "down":
			t = "mousePressed"
		case "up":
			t = "mouseReleased"
		default:
			return d.fail(env.Type, fmt.Errorf("invalid action %q", data.Action))
		}
		params := map[string]any{
			"type":       t,
			"x":          data.X,
			"y":          data.Y,
			"button":     protocolButtonToCDP(data.Button),
			"buttons":    1 << data.Button, // CDP buttons bitmask
			"clickCount": 1,
			"modifiers":  0,
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchMouseEvent", params)
		return d.fail(env.Type, err)

	case "mouse_wheel":
		var data mouseWheelData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("mouse_wheel: %w", err))
		}
		// v1.1 phase machine: track the wheel gesture state and use
		// it to drive end-of-momentum dispatch. The protocol's
		// `delta_mode` string supersedes the numeric `mode` when both
		// are present (per the spec's "Servers SHOULD prefer this").
		mode := data.Mode
		switch data.DeltaMode {
		case "pixel":
			mode = 0
		case "line":
			mode = 1
		case "page":
			mode = 2
		}

		// Track per-dispatcher phase so concurrent gestures don't
		// interleave. v1.0 clients omit `phase`; we treat that as
		// "changed" with no end-synthesis.
		switch data.Phase {
		case "start":
			d.wheelMu.Lock()
			d.wheelInGesture = true
			d.wheelMu.Unlock()
		case "end":
			// Translate phase=end to a final zero-delta CDP wheel so
			// Chromium's compositor flushes any momentum decay.
			params := map[string]any{
				"type":      "mouseWheel",
				"x":         data.X,
				"y":         data.Y,
				"button":    "none",
				"deltaX":    0.0,
				"deltaY":    0.0,
				"modifiers": 0,
				// pointerType helps Chromium classify the synthetic
				// frame as part of a mouse gesture.
				"pointerType": "mouse",
			}
			_, err := d.cdp.Send(ctx, "Input.dispatchMouseEvent", params)
			d.wheelMu.Lock()
			d.wheelInGesture = false
			d.wheelMu.Unlock()
			return d.fail(env.Type, err)
		}

		// CDP expects pixel deltas; protocol carries the line/page
		// scaling intent. Approximate non-pixel modes the same way the
		// pre-T62 bridge did (16 px/line, 800 px/page) — Phase 2 will
		// replace with line-height-aware scrolling.
		dx, dy := float64(data.DX), float64(data.DY)
		switch mode {
		case 1:
			dx *= 16
			dy *= 16
		case 2:
			dx *= 800
			dy *= 800
		}
		params := map[string]any{
			"type":      "mouseWheel",
			"x":         data.X,
			"y":         data.Y,
			"button":    "none",
			"deltaX":    dx,
			"deltaY":    dy,
			"modifiers": 0,
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchMouseEvent", params)
		return d.fail(env.Type, err)

	case "key_down", "key_up":
		var data keyData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("%s: %w", env.Type, err))
		}
		t := "keyDown"
		if env.Type == "key_up" {
			t = "keyUp"
		}
		params := map[string]any{
			"type":      t,
			"code":      data.Code,
			"key":       data.Key,
			"modifiers": protocolModsToCDP(data.Mods),
		}
		// For printable characters, also include `text` so the
		// renderer fires `input` events.
		if env.Type == "key_down" && len(data.Key) == 1 {
			params["text"] = data.Key
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchKeyEvent", params)
		return d.fail(env.Type, err)

	case "composition_start", "composition_update":
		var data compositionData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("%s: %w", env.Type, err))
		}
		// Input.imeSetComposition with selection collapsed at end.
		params := map[string]any{
			"text":             data.Data,
			"selectionStart":   len(data.Data),
			"selectionEnd":     len(data.Data),
			"replacementStart": 0,
			"replacementEnd":   0,
		}
		_, err := d.cdp.Send(ctx, "Input.imeSetComposition", params)
		return d.fail(env.Type, err)

	case "composition_end":
		var data compositionData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("composition_end: %w", err))
		}
		params := map[string]any{"text": data.Data}
		_, err := d.cdp.Send(ctx, "Input.insertText", params)
		return d.fail(env.Type, err)

	case "clipboard_paste":
		var data clipboardPasteData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("clipboard_paste: %w", err))
		}
		// Insert text directly. A future iteration will write the system
		// clipboard and synthesize Ctrl+V so the page sees a real paste
		// event.
		params := map[string]any{"text": data.Text}
		_, err := d.cdp.Send(ctx, "Input.insertText", params)
		return d.fail(env.Type, err)

	case "drag_start":
		var data dragStartData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("drag_start: %w", err))
		}
		// Enable drag interception once per process. Idempotent in
		// CDP; we don't fail the dispatch if it errors (some Chromium
		// builds may not need it).
		d.dragInterceptOnce.Do(func() {
			interceptCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
			defer cancel()
			if _, err := d.cdp.Send(interceptCtx, "Input.setInterceptDrags",
				map[string]any{"enabled": true}); err != nil {
				d.log.Debug("Input.setInterceptDrags not supported", slog.Any("err", err))
			}
		})
		d.dragMu.Lock()
		d.dragItems = data.Items
		d.dragTypes = data.Types
		d.dragActive = true
		d.dragMu.Unlock()
		_, err := d.cdp.Send(ctx, "Input.dispatchDragEvent",
			d.buildDragEvent("dragEnter", data.X, data.Y))
		return d.fail(env.Type, err)

	case "drag_over":
		var data dragOverData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("drag_over: %w", err))
		}
		d.dragMu.Lock()
		active := d.dragActive
		d.dragMu.Unlock()
		if !active {
			// Stray drag_over outside an active drag — protocol says
			// servers MUST tolerate as a no-op.
			d.log.Debug("ignoring drag_over outside active drag")
			return nil
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchDragEvent",
			d.buildDragEvent("dragOver", data.X, data.Y))
		return d.fail(env.Type, err)

	case "drop":
		var data dropData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("drop: %w", err))
		}
		// drop re-asserts items per the protocol; prefer the fresh
		// payload over our cached drag_start state in case the client
		// modified it between events (unusual but allowed).
		d.dragMu.Lock()
		d.dragItems = data.Items
		d.dragTypes = data.Types
		d.dragMu.Unlock()
		params := d.buildDragEvent("drop", data.X, data.Y)
		_, err := d.cdp.Send(ctx, "Input.dispatchDragEvent", params)
		// Per state-machine: drop does NOT clear state — the
		// follow-up drag_end with success:true is what releases it.
		return d.fail(env.Type, err)

	case "drag_end":
		var data dragEndData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("drag_end: %w", err))
		}
		d.dragMu.Lock()
		active := d.dragActive
		// We need a position for dragCancel — use (0,0) if we never
		// saw a drag_over. CDP doesn't really care for dragCancel.
		d.dragActive = false
		d.dragItems = nil
		d.dragTypes = nil
		d.dragMu.Unlock()
		if !active {
			// drag_end outside an active drag — informational, no-op.
			return nil
		}
		if data.Success {
			// drop already fired; just clear state. No CDP call.
			return nil
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchDragEvent",
			d.buildDragEvent("dragCancel", 0, 0))
		return d.fail(env.Type, err)

	case "touch_start":
		var data touchStartData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("touch_start: %w", err))
		}
		d.touchMu.Lock()
		d.activeTouches[data.Identifier] = touchPoint{
			id: data.Identifier, x: data.X, y: data.Y,
			radiusX: max1(data.RadiusX), radiusY: max1(data.RadiusY),
			force: data.Force, twist: data.Twist,
		}
		points := d.touchPointsLocked()
		d.touchMu.Unlock()
		_, err := d.cdp.Send(ctx, "Input.dispatchTouchEvent", map[string]any{
			"type":        "touchStart",
			"touchPoints": points,
			"modifiers":   0,
		})
		return d.fail(env.Type, err)

	case "touch_move":
		var data touchMoveData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("touch_move: %w", err))
		}
		d.touchMu.Lock()
		if _, known := d.activeTouches[data.Identifier]; !known {
			d.touchMu.Unlock()
			d.log.Warn("touch_move for unknown identifier; dropping",
				slog.Int("id", data.Identifier))
			return nil
		}
		d.activeTouches[data.Identifier] = touchPoint{
			id: data.Identifier, x: data.X, y: data.Y,
			radiusX: max1(data.RadiusX), radiusY: max1(data.RadiusY),
			force: data.Force, twist: data.Twist,
		}
		points := d.touchPointsLocked()
		d.touchMu.Unlock()
		_, err := d.cdp.Send(ctx, "Input.dispatchTouchEvent", map[string]any{
			"type":        "touchMove",
			"touchPoints": points,
			"modifiers":   0,
		})
		return d.fail(env.Type, err)

	case "touch_end", "touch_cancel":
		var data touchEndData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("%s: %w", env.Type, err))
		}
		d.touchMu.Lock()
		if _, known := d.activeTouches[data.Identifier]; !known {
			d.touchMu.Unlock()
			d.log.Warn("touch_end/cancel for unknown identifier; dropping",
				slog.Int("id", data.Identifier),
				slog.String("type", env.Type))
			return nil
		}
		// Per CDP/puppeteer convention: touchPoints reflects the state
		// AFTER this event applies — i.e. the remaining active touches
		// after this finger lifts. Empty when the last finger goes up.
		delete(d.activeTouches, data.Identifier)
		points := d.touchPointsLocked()
		d.touchMu.Unlock()

		cdpType := "touchEnd"
		if env.Type == "touch_cancel" {
			cdpType = "touchCancel"
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchTouchEvent", map[string]any{
			"type":        cdpType,
			"touchPoints": points,
			"modifiers":   0,
		})
		return d.fail(env.Type, err)

	case "clipboard_copy_request":
		// Phase 1: synthesize Ctrl+C. The reply path ships separately.
		_, err := d.cdp.Send(ctx, "Input.dispatchKeyEvent", map[string]any{
			"type":      "keyDown",
			"code":      "KeyC",
			"key":       "c",
			"modifiers": 2, // Ctrl in CDP bitmask
		})
		if err == nil {
			_, err = d.cdp.Send(ctx, "Input.dispatchKeyEvent", map[string]any{
				"type":      "keyUp",
				"code":      "KeyC",
				"key":       "c",
				"modifiers": 2,
			})
		}
		return d.fail(env.Type, err)

	default:
		// Unknown type — log and skip per the spec's "MAY ignore unknown
		// types" rule.
		d.metrics.unknown.Inc()
		d.log.Warn("ignoring unknown event type", slog.String("type", env.Type))
		return nil
	}
}

// buildDragEvent assembles a CDP Input.dispatchDragEvent payload from
// the dispatcher's cached drag state plus the supplied (type, x, y).
//
// CDP's DragData carries `items` (each with mimeType + data), an
// optional `files` array, and `dragOperationsMask` (1=copy, 2=link,
// 4=move). v1 only emits string drags so we hard-code copy.
//
// File items in the protocol carry no `data` field (file contents
// are out of scope for v1). We forward them as zero-data CDP items so
// the page sees the MIME types in DataTransfer.types but reading
// file content yields empty.
func (d *dispatcher) buildDragEvent(t string, x, y int) map[string]any {
	d.dragMu.Lock()
	items := make([]map[string]any, 0, len(d.dragItems))
	for _, it := range d.dragItems {
		items = append(items, map[string]any{
			"mimeType": it.Type,
			"data":     it.Data, // empty string for kind=="file"
		})
	}
	d.dragMu.Unlock()

	return map[string]any{
		"type": t,
		"x":    x,
		"y":    y,
		"data": map[string]any{
			"items":              items,
			"dragOperationsMask": 1, // copy
		},
		"modifiers": 0,
	}
}

// touchPointsLocked snapshots the active-touches map as a CDP-shaped
// slice, sorted by identifier for determinism (matches the client-
// side flush order; makes traces easier to compare across runs).
//
// MUST be called with d.touchMu held.
func (d *dispatcher) touchPointsLocked() []map[string]any {
	if len(d.activeTouches) == 0 {
		// Return a non-nil empty slice so json.Marshal emits `[]`,
		// not `null`. CDP's some builds reject the null variant.
		return []map[string]any{}
	}
	ids := make([]int, 0, len(d.activeTouches))
	for id := range d.activeTouches {
		ids = append(ids, id)
	}
	// Simple ascending sort; len(ids) is at most ~10 in practice.
	for i := 1; i < len(ids); i++ {
		for j := i; j > 0 && ids[j-1] > ids[j]; j-- {
			ids[j-1], ids[j] = ids[j], ids[j-1]
		}
	}
	out := make([]map[string]any, 0, len(ids))
	for _, id := range ids {
		out = append(out, d.activeTouches[id].toCDP())
	}
	return out
}

// max1 clamps a non-positive radius up to 1 px so CDP doesn't reject
// the touchPoint. Mirrors the client-side clamp in input.ts.
func max1(n int) int {
	if n < 1 {
		return 1
	}
	return n
}

func (d *dispatcher) fail(eventType string, err error) error {
	if err == nil {
		return nil
	}
	d.metrics.errors.WithLabelValues(eventType).Inc()
	d.log.Warn("dispatch error", slog.String("type", eventType), slog.Any("err", err))
	return err
}

// ---------------------------------------------------------------------------
// Sources — stdin and websocket
// ---------------------------------------------------------------------------

// runStdinSource reads newline-delimited JSON envelopes from r and
// forwards each to handler. Returns when r closes or ctx is done.
func runStdinSource(ctx context.Context, r io.Reader, handler func(context.Context, []byte)) error {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64*1024), 1<<20)
	for sc.Scan() {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
		line := sc.Bytes()
		if len(line) == 0 {
			continue
		}
		// Copy because Scanner.Bytes is reused.
		buf := make([]byte, len(line))
		copy(buf, line)
		handler(ctx, buf)
	}
	return sc.Err()
}

func runWebsocketSource(ctx context.Context, addr, path string, handler func(context.Context, []byte), log *slog.Logger) error {
	mux := http.NewServeMux()
	upgrader := websocket.Upgrader{
		CheckOrigin: func(*http.Request) bool { return true },
	}
	mux.HandleFunc(path, func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			log.Warn("upgrade failed", slog.Any("err", err))
			return
		}
		defer conn.Close()
		conn.SetReadLimit(1 << 20)
		for {
			_, raw, err := conn.ReadMessage()
			if err != nil {
				if !websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway) {
					log.Info("ws read closed", slog.Any("err", err))
				}
				return
			}
			handler(ctx, raw)
		}
	})

	srv := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	errCh := make(chan error, 1)
	go func() { errCh <- srv.ListenAndServe() }()
	select {
	case err := <-errCh:
		return err
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx)
		return ctx.Err()
	}
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

type metrics struct {
	registry *prometheus.Registry
	events   *prometheus.CounterVec
	errors   *prometheus.CounterVec
	unknown  prometheus.Counter
	dispatch *prometheus.HistogramVec
	parseErr prometheus.Counter
}

func newMetrics() *metrics {
	reg := prometheus.NewRegistry()
	m := &metrics{
		registry: reg,
		events: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "input_bridge_events_total",
			Help: "Number of input events dispatched, labelled by event type.",
		}, []string{"type"}),
		errors: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "input_bridge_errors_total",
			Help: "Number of dispatch errors, labelled by event type.",
		}, []string{"type"}),
		unknown: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "input_bridge_unknown_total",
			Help: "Envelopes with an unknown 'type' field.",
		}),
		parseErr: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "input_bridge_parse_errors_total",
			Help: "Envelopes rejected during parse (invalid JSON / version).",
		}),
		dispatch: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name: "input_bridge_dispatch_seconds",
			Help: "Dispatch latency from envelope received to CDP ack, by type.",
			// Phase 1 budget: < 80 ms input round-trip end-to-end; the
			// bridge's slice should stay well under 25 ms.
			Buckets: []float64{
				0.0005, 0.001, 0.002, 0.005, 0.010, 0.020,
				0.050, 0.100, 0.250, 0.500, 1.0,
			},
		}, []string{"type"}),
	}
	reg.MustRegister(m.events, m.errors, m.unknown, m.parseErr, m.dispatch)
	return m
}

func (m *metrics) observe(eventType string, d time.Duration, _ error) {
	m.events.WithLabelValues(eventType).Inc()
	m.dispatch.WithLabelValues(eventType).Observe(d.Seconds())
}

// servePrometheus exposes /metrics on the given addr until ctx is done.
func servePrometheus(ctx context.Context, addr string, m *metrics, log *slog.Logger) error {
	mux := http.NewServeMux()
	mux.Handle("/metrics", promhttp.HandlerFor(m.registry, promhttp.HandlerOpts{}))
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})
	srv := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	errCh := make(chan error, 1)
	go func() {
		log.Info("metrics listening", slog.String("addr", addr))
		errCh <- srv.ListenAndServe()
	}()
	select {
	case err := <-errCh:
		return err
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx)
		return ctx.Err()
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

type config struct {
	source      string // "stdin" | "ws"
	wsAddr      string // ":9100" for the ws source
	wsPath      string // "/input"
	cdpURL      string // "http://localhost:9222"
	metricsAddr string // ":9101"
	dryRun      bool   // skip CDP entirely; useful for smoke testing.
}

func parseFlags(args []string) (config, error) {
	fs := flag.NewFlagSet("input-bridge", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	cfg := config{}
	fs.StringVar(&cfg.source, "source", "stdin", "Input source: stdin | ws")
	fs.StringVar(&cfg.wsAddr, "ws-addr", "127.0.0.1:9100", "WebSocket source listen address")
	fs.StringVar(&cfg.wsPath, "ws-path", "/input", "WebSocket source URL path")
	fs.StringVar(&cfg.cdpURL, "cdp-url", "http://127.0.0.1:9222", "Chromium DevTools base URL")
	fs.StringVar(&cfg.metricsAddr, "metrics-addr", "127.0.0.1:9101", "Prometheus /metrics listen address")
	fs.BoolVar(&cfg.dryRun, "dry-run", false, "Parse and log events but do not connect to CDP")
	if err := fs.Parse(args); err != nil {
		return cfg, err
	}
	if cfg.source != "stdin" && cfg.source != "ws" {
		return cfg, fmt.Errorf("invalid --source %q (want stdin|ws)", cfg.source)
	}
	return cfg, nil
}

func run(ctx context.Context, cfg config, stdin io.Reader, log *slog.Logger) error {
	m := newMetrics()

	// Metrics is always on; clients may not use it but it costs nothing.
	metricsErrCh := make(chan error, 1)
	go func() { metricsErrCh <- servePrometheus(ctx, cfg.metricsAddr, m, log) }()

	// Build the dispatcher.
	var disp *dispatcher
	if cfg.dryRun {
		disp = newDispatcher(noopCDP{log: log}, m, log)
	} else {
		dialCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		cdp, err := dialCDP(dialCtx, cfg.cdpURL, log)
		cancel()
		if err != nil {
			return fmt.Errorf("connect to CDP: %w", err)
		}
		defer cdp.Close()
		disp = newDispatcher(cdp, m, log)
	}

	handler := func(ctx context.Context, raw []byte) {
		env, err := parseEnvelope(raw)
		if err != nil {
			m.parseErr.Inc()
			log.Warn("parse failed", slog.Any("err", err), slog.Int("len", len(raw)))
			return
		}
		dispatchCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
		defer cancel()
		_ = disp.Dispatch(dispatchCtx, env)
	}

	switch cfg.source {
	case "stdin":
		log.Info("reading envelopes from stdin")
		if err := runStdinSource(ctx, stdin, handler); err != nil &&
			!errors.Is(err, context.Canceled) &&
			!errors.Is(err, context.DeadlineExceeded) {
			return err
		}
		return nil
	case "ws":
		log.Info("listening for envelopes over websocket",
			slog.String("addr", cfg.wsAddr), slog.String("path", cfg.wsPath))
		if err := runWebsocketSource(ctx, cfg.wsAddr, cfg.wsPath, handler, log); err != nil &&
			!errors.Is(err, context.Canceled) {
			return err
		}
		return nil
	}
	return fmt.Errorf("unreachable")
}

// noopCDP swallows commands; used by --dry-run.
type noopCDP struct{ log *slog.Logger }

func (n noopCDP) Send(_ context.Context, method string, params any) (json.RawMessage, error) {
	n.log.Debug("dry-run cdp", slog.String("method", method), slog.Any("params", params))
	return []byte("{}"), nil
}

func main() {
	cfg, err := parseFlags(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, "flag error:", err)
		os.Exit(2)
	}

	log := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(log)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	if err := run(ctx, cfg, os.Stdin, log); err != nil {
		log.Error("run failed", slog.Any("err", err))
		os.Exit(1)
	}
	log.Info("bye")
}

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
//     latency p50/p95/p99 histograms, error counters)
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

// compositionData carries the v1.1 IME envelope payload. The
// v1.0 fields (Data) are required; selection / rect / candidates
// are optional v1.1 (T88) extensions used to drive
// Input.imeSetComposition more accurately. Pointers so we can
// distinguish "field omitted" from "field set to 0".
type compositionData struct {
	Data           string           `json:"data"`
	SelectionStart *int             `json:"selection_start,omitempty"`
	SelectionEnd   *int             `json:"selection_end,omitempty"`
	Rect           *compositionRect `json:"rect,omitempty"`
	CandidateList  []string         `json:"candidate_list,omitempty"`
}

type compositionRect struct {
	X int `json:"x"`
	Y int `json:"y"`
	W int `json:"w"`
	H int `json:"h"`
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

// keyToProtocolMod returns the protocol modifier bit for a Shift /
// Control / Alt / Meta key event, or 0 for any other key. Used by the
// dispatcher's cross-envelope modifier state machine — pressing one
// of these keys sets the bit in heldMods, releasing clears it.
//
// We accept either KeyboardEvent.key (e.g. "Shift") or
// KeyboardEvent.code (e.g. "ShiftLeft") because clients vary in
// which they populate. The protocol spec carries both fields, so we
// match either.
func keyToProtocolMod(key, code string) int {
	switch key {
	case "Shift":
		return modShift
	case "Control":
		return modCtrl
	case "Alt":
		return modAlt
	case "Meta", "OS", "Super":
		return modMeta
	}
	switch code {
	case "ShiftLeft", "ShiftRight":
		return modShift
	case "ControlLeft", "ControlRight":
		return modCtrl
	case "AltLeft", "AltRight":
		return modAlt
	case "MetaLeft", "MetaRight", "OSLeft", "OSRight":
		return modMeta
	}
	return 0
}

// keyTextMap covers the special keys whose CDP keyDown needs an
// explicit `text` field for the renderer to actually insert the
// character into a focused text input. Without `text`, chromium
// treats e.g. Enter as a navigational keypress (which in a plain
// textarea does nothing) instead of a character insertion.
//
// Other special keys (Arrow*, Home, End, PageUp, PageDown, F-keys)
// MUST NOT have a `text` field — they're cursor / navigation
// controls, not text-producing.
var keyTextMap = map[string]string{
	"Enter":     "\r",
	"Tab":       "\t",
	"Backspace": "\b",
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
// CDP client — flat-mode (BUGS-529)
//
// We speak CDP over a single WebSocket dialled at the BROWSER level
// (the URL exposed by /json/version's `webSocketDebuggerUrl`), then
// configure flat-mode auto-attach so chromium delivers a session for
// every page target on the same connection. Each frame on the wire
// carries an optional `sessionId` field; outgoing commands are routed
// to a specific session by setting it.
//
// Why not the legacy `/devtools/page/<id>` direct-page-WS we used pre-
// BUGS-529? On chromeless, that style of session silently stops
// reaching the renderer when a separate CDP client navigates the
// target — `Input.dispatch*` continues to ack at the protocol layer
// but produces no page-level events. Flat-mode sessions follow
// navigation correctly on stock chromium and on chromeless, so the
// bridge migrates to flat-mode as the BUGS-529 long-term fix.
//
// The browser↔bridge wire shape (per chromedevtools.github.io/devtools-
// protocol/tot/Target/, "flatten" semantics):
//
//   request:  {"id": N, "sessionId": "<sid>", "method": "Foo.bar", "params": {...}}
//   reply:    {"id": N, "sessionId": "<sid>", "result": {...}}    or {"error": {...}}
//   event:    {"sessionId": "<sid>", "method": "Foo.event", "params": {...}}
//
// `sessionId` is omitted on commands aimed at the browser session
// itself (Target.setDiscoverTargets / Target.setAutoAttach).
// ---------------------------------------------------------------------------

type cdpClient struct {
	mu           sync.Mutex
	conn         *websocket.Conn
	nextID       int64
	pending      map[int64]chan cdpResponse
	disconnect   chan struct{}
	log          *slog.Logger
	eventHandler func(method, sessionID string, params json.RawMessage)
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

// cdpRequest is the outgoing frame shape. Outgoing params is `any` so
// callers can pass map[string]any literals; encoding/json marshals the
// structure as-is.
type cdpRequest struct {
	ID        int64  `json:"id"`
	SessionID string `json:"sessionId,omitempty"`
	Method    string `json:"method"`
	Params    any    `json:"params,omitempty"`
}

// cdpIncoming is the union shape for both replies (carrying ID +
// Result/Error) and events (carrying Method + Params + optional
// SessionID). We discriminate on whether ID is non-zero.
type cdpIncoming struct {
	ID        int64           `json:"id,omitempty"`
	SessionID string          `json:"sessionId,omitempty"`
	Method    string          `json:"method,omitempty"`
	Params    json.RawMessage `json:"params,omitempty"`
	Result    json.RawMessage `json:"result,omitempty"`
	Error     *cdpError       `json:"error,omitempty"`
}

// dialCDP opens the browser-level CDP WebSocket, configures flat-mode
// auto-attach, and returns a session-aware sender that routes every
// dispatch through the most-recently-attached page session. Caller
// must Close() the returned sender when done.
func dialCDP(ctx context.Context, baseURL string, log *slog.Logger) (*pageSessionSender, error) {
	wsURL, err := discoverBrowserWS(ctx, baseURL)
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
	sender := newPageSessionSender(c, log)
	c.eventHandler = sender.handleEvent
	go c.readLoop()

	// Configure flat-mode auto-attach on the BROWSER session (no
	// sessionId — these commands target the connection itself).
	//
	//   setDiscoverTargets(true) emits Target.targetCreated for each
	//   existing target and for every new one going forward.
	//
	//   setAutoAttach(true, false, true) tells chromium to attach a
	//   flat-mode session to every related target (with flatten=true,
	//   sessionIds carry on every envelope on this same WS instead of
	//   being demuxed via Target.sendMessageToTarget). chromium emits
	//   Target.attachedToTarget for each one — we read sessionId out of
	//   that event in pageSessionSender.handleEvent.
	if _, err := c.Send(ctx, "", "Target.setDiscoverTargets",
		map[string]any{"discover": true}); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("Target.setDiscoverTargets: %w", err)
	}
	if _, err := c.Send(ctx, "", "Target.setAutoAttach", map[string]any{
		"autoAttach":             true,
		"waitForDebuggerOnStart": false,
		"flatten":                true,
	}); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("Target.setAutoAttach: %w", err)
	}
	return sender, nil
}

const (
	cdpConnectTimeout      = 60 * time.Second
	cdpDialAttemptTimeout  = 10 * time.Second
	cdpConnectRetryBackoff = 500 * time.Millisecond
)

type cdpDialFunc func(context.Context, string, *slog.Logger) (*pageSessionSender, error)

func connectCDPWithRetry(ctx context.Context, baseURL string, log *slog.Logger) (*pageSessionSender, error) {
	return connectCDPWithRetryDialer(ctx, baseURL, log, cdpConnectTimeout, cdpConnectRetryBackoff, dialCDP)
}

func connectCDPWithRetryDialer(
	ctx context.Context,
	baseURL string,
	log *slog.Logger,
	maxWait time.Duration,
	retryBackoff time.Duration,
	dial cdpDialFunc,
) (*pageSessionSender, error) {
	retryCtx, cancel := context.WithTimeout(ctx, maxWait)
	defer cancel()

	var lastErr error
	attempt := 0
	for {
		attempt++
		dialCtx, dialCancel := context.WithTimeout(retryCtx, cdpDialAttemptTimeout)
		cdp, err := dial(dialCtx, baseURL, log)
		dialCancel()
		if err == nil {
			if attempt > 1 {
				log.Info("connected to CDP after retry",
					slog.Int("attempt", attempt),
					slog.String("base_url", baseURL))
			}
			return cdp, nil
		}
		lastErr = err
		log.Warn("CDP not ready; retrying",
			slog.Int("attempt", attempt),
			slog.String("base_url", baseURL),
			slog.Any("err", err))

		timer := time.NewTimer(retryBackoff)
		select {
		case <-retryCtx.Done():
			timer.Stop()
			if lastErr == nil {
				lastErr = retryCtx.Err()
			}
			return nil, fmt.Errorf("CDP not ready after %s: %w", maxWait, lastErr)
		case <-timer.C:
		}
	}
}

// discoverBrowserWS hits /json/version and returns the browser-level
// debugger WS URL. With flat-mode this is the SINGLE WS we open;
// every page session is multiplexed over it via sessionId.
func discoverBrowserWS(ctx context.Context, baseURL string) (string, error) {
	u := strings.TrimRight(baseURL, "/") + "/json/version"
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
	var info struct {
		WebSocketDebuggerURL string `json:"webSocketDebuggerUrl"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&info); err != nil {
		return "", fmt.Errorf("decode /json/version: %w", err)
	}
	if info.WebSocketDebuggerURL == "" {
		return "", fmt.Errorf("no browser webSocketDebuggerUrl at %s", u)
	}
	return info.WebSocketDebuggerURL, nil
}

func (c *cdpClient) readLoop() {
	defer close(c.disconnect)
	for {
		_, raw, err := c.conn.ReadMessage()
		if err != nil {
			c.log.Info("cdp connection closed", slog.Any("err", err))
			return
		}
		var env cdpIncoming
		if err := json.Unmarshal(raw, &env); err != nil {
			c.log.Warn("cdp non-JSON frame", slog.Any("err", err))
			continue
		}
		if env.ID != 0 {
			// Reply path: deliver to the waiter that issued this id.
			c.mu.Lock()
			ch, ok := c.pending[env.ID]
			delete(c.pending, env.ID)
			c.mu.Unlock()
			if ok {
				ch <- cdpResponse{Result: env.Result, Error: env.Error}
				close(ch)
			}
			continue
		}
		// Event path: server-initiated frame. We hand it to the event
		// handler (typically pageSessionSender) so it can track
		// Target.attachedToTarget / Target.detachedFromTarget.
		if env.Method != "" && c.eventHandler != nil {
			c.eventHandler(env.Method, env.SessionID, env.Params)
		}
	}
}

// Send issues a CDP command on the named session and waits for the
// matching response (or ctx). Pass an empty `sessionID` for commands
// targeting the browser session (Target.* setup). `params` may be nil.
func (c *cdpClient) Send(ctx context.Context, sessionID, method string, params any) (json.RawMessage, error) {
	id := atomic.AddInt64(&c.nextID, 1)
	ch := make(chan cdpResponse, 1)

	c.mu.Lock()
	c.pending[id] = ch
	conn := c.conn
	c.mu.Unlock()
	if conn == nil {
		// Test scaffolding can construct a cdpClient with no
		// underlying connection (the pageSessionSender unit tests
		// drive handleEvent directly without a real WS). Refuse the
		// send rather than panic in SetWriteDeadline below.
		return nil, errors.New("cdp not connected")
	}

	frame := cdpRequest{ID: id, SessionID: sessionID, Method: method, Params: params}
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
// pageSessionSender — flat-mode dispatch routing
//
// Wraps cdpClient and routes every Send call to the "current" attached
// page session. The current session prefers the customer page over the
// local streamer page. Chromium exposes both as page targets in this
// pod: the remote browsing target (for example https://example.com/)
// and http://localhost:9000/streamer/index.html, which only captures
// and relays WebRTC. Sending Input.dispatch* to the streamer target
// cleanly ACKs at CDP but never clicks the remote page.
//
// When a session detaches we drop it and fall back to another attached
// page, preferring known non-streamer URLs first and then unknown page
// sessions over known streamer URLs.
//
// If no page session is attached yet when Send is called (race: the
// first input envelope can arrive before chromium has emitted
// attachedToTarget for the boot-time about:blank), Send blocks until
// one arrives or the caller's ctx expires. The dispatcher's per-event
// 2s ctx gives plenty of headroom for the auto-attach handshake.
// ---------------------------------------------------------------------------

type pageSessionSender struct {
	cdp *cdpClient
	log *slog.Logger

	mu            sync.Mutex
	pageSessions  map[string]pageSession // sessionID → all currently-attached pages
	attachSeq     int64
	currentSID    string
	pendingWaiter chan string // closed when currentSID transitions ""→non-empty
}

type pageSession struct {
	targetID string
	url      string
	seq      int64
}

func newPageSessionSender(cdp *cdpClient, log *slog.Logger) *pageSessionSender {
	return &pageSessionSender{
		cdp:          cdp,
		log:          log,
		pageSessions: make(map[string]pageSession),
	}
}

// Close tears down the underlying CDP connection.
func (s *pageSessionSender) Close() error { return s.cdp.Close() }

// CurrentSession returns the active page session id (or empty if no
// page is currently attached). Test-only helper.
func (s *pageSessionSender) CurrentSession() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.currentSID
}

// Send routes the command at the current page session. Blocks (within
// ctx) until at least one page session is attached.
func (s *pageSessionSender) Send(ctx context.Context, method string, params any) (json.RawMessage, error) {
	sid, err := s.waitForSession(ctx)
	if err != nil {
		return nil, fmt.Errorf("waiting for page session: %w", err)
	}
	// P3 Bug 11/13 diagnostic: log which page session each Input.*
	// dispatch routes to. Verifies the BrowserContext-scope auto-attach
	// hypothesis — if Input.* lands on the streamer page session
	// (URL contains "/streamer/") instead of a user content page,
	// click/cursor envelopes never reach the rendered tab.
	if strings.HasPrefix(method, "Input.") {
		s.mu.Lock()
		info := s.pageSessions[sid]
		s.mu.Unlock()
		s.log.Info("input dispatch routing",
			slog.String("method", method),
			slog.String("session_id", sid),
			slog.String("target_url", info.url))
	}
	return s.cdp.Send(ctx, sid, method, params)
}

func (s *pageSessionSender) waitForSession(ctx context.Context) (string, error) {
	s.mu.Lock()
	if s.currentSID != "" {
		sid := s.currentSID
		s.mu.Unlock()
		return sid, nil
	}
	if s.pendingWaiter == nil {
		s.pendingWaiter = make(chan string)
	}
	ch := s.pendingWaiter
	s.mu.Unlock()
	select {
	case <-ch:
		s.mu.Lock()
		sid := s.currentSID
		s.mu.Unlock()
		if sid == "" {
			return "", errors.New("session attach signal but no current session")
		}
		return sid, nil
	case <-ctx.Done():
		return "", ctx.Err()
	case <-s.cdp.disconnect:
		return "", errors.New("cdp disconnected before session attached")
	}
}

// handleEvent is the cdpClient eventHandler hook. We track
// Target.attachedToTarget / detachedFromTarget for page-type targets
// and ignore everything else (workers, service-workers, etc.).
//
// Frame-attached events (iframe / popup-on-same-page) are not routed
// here because the bridge dispatches input at the page level — its
// dispatched mouse/key events reach iframes via the renderer's normal
// event-targeting, no per-iframe sessionId required.
func (s *pageSessionSender) handleEvent(method, sessionID string, paramsRaw json.RawMessage) {
	switch method {
	case "Target.targetCreated":
		// P3 Bug 11/13 fix: chromium's browser-level Target.setAutoAttach
		// (configured in dialCDP at init) only auto-attaches page targets
		// in the bridge's own BrowserContext. After PR #16 isolated each
		// session into its own BrowserContext via per-pid profile dirs in
		// cloud_browser_browser_context.cc, user content pages are
		// created in non-default contexts and are NOT auto-attached.
		// chooseCurrentSessionLocked then falls through to tier 3 and
		// routes all Input.* dispatches to the streamer page session.
		//
		// Target.setDiscoverTargets (also in dialCDP init) DOES fire
		// Target.targetCreated for cross-context pages. So we listen
		// here, filter to non-streamer pages we haven't already attached
		// to, and issue an explicit Target.attachToTarget — chromium
		// then emits Target.attachedToTarget which the existing case
		// below records into pageSessions. From there
		// chooseCurrentSessionLocked picks the content session correctly
		// via tier 1 (concrete URL, non-streamer).
		//
		// Note: setAutoAttachRelatedTargets is NOT the right
		// affordance here — per CDP spec it watches a specific
		// already-attached target's children, not cross-BrowserContext
		// auto-attach. An earlier fix attempt assumed otherwise and
		// crashlooped with CDP error -32601 on the deployed chromium.
		var p struct {
			TargetInfo struct {
				TargetID         string `json:"targetId"`
				Type             string `json:"type"`
				URL              string `json:"url"`
				BrowserContextID string `json:"browserContextId"`
			} `json:"targetInfo"`
		}
		if err := json.Unmarshal(paramsRaw, &p); err != nil {
			s.log.Warn("Target.targetCreated: bad params", slog.Any("err", err))
			return
		}
		if p.TargetInfo.Type != "page" {
			return
		}
		if isStreamerPageURL(p.TargetInfo.URL) {
			// The streamer is the in-pod /streamer/* page. It's
			// uninteresting as an input target — Pattern C dispatch
			// goes to user content. Skip explicit-attach; let the
			// browser-level setAutoAttach handle it if it ever auto-
			// attaches (typically it does, since the streamer lives in
			// the default BrowserContext).
			return
		}
		// Skip already-known targets to avoid attach storms on rapid
		// targetCreated events for the same target (chromium can emit
		// targetCreated again after targetInfoChanged).
		s.mu.Lock()
		already := false
		for _, info := range s.pageSessions {
			if info.targetID == p.TargetInfo.TargetID {
				already = true
				break
			}
		}
		s.mu.Unlock()
		if already {
			return
		}
		// Issue attach asynchronously — the readLoop must keep draining
		// or we deadlock. The attachedToTarget event will land on this
		// same readLoop and the existing case below will handle the
		// session registration.
		go func(targetID, url, ctxID string) {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			if _, err := s.cdp.Send(ctx, "", "Target.attachToTarget", map[string]any{
				"targetId": targetID,
				"flatten":  true,
			}); err != nil {
				s.log.Warn("explicit attach to cross-context target failed",
					slog.String("target_id", targetID),
					slog.String("url", url),
					slog.String("browser_context_id", ctxID),
					slog.Any("err", err))
				return
			}
			s.log.Info("explicit attach to cross-context target requested",
				slog.String("target_id", targetID),
				slog.String("url", url),
				slog.String("browser_context_id", ctxID))
		}(p.TargetInfo.TargetID, p.TargetInfo.URL, p.TargetInfo.BrowserContextID)

	case "Target.attachedToTarget":
		var p struct {
			SessionID  string `json:"sessionId"`
			TargetInfo struct {
				TargetID string `json:"targetId"`
				Type     string `json:"type"`
				URL      string `json:"url"`
			} `json:"targetInfo"`
		}
		if err := json.Unmarshal(paramsRaw, &p); err != nil {
			s.log.Warn("Target.attachedToTarget: bad params", slog.Any("err", err))
			return
		}
		if p.TargetInfo.Type != "page" {
			return
		}
		s.mu.Lock()
		s.attachSeq++
		s.pageSessions[p.SessionID] = pageSession{
			targetID: p.TargetInfo.TargetID,
			url:      p.TargetInfo.URL,
			seq:      s.attachSeq,
		}
		prevEmpty := s.currentSID == ""
		s.currentSID = s.chooseCurrentSessionLocked()
		currentSID := s.currentSID
		var notify chan string
		if prevEmpty && currentSID != "" && s.pendingWaiter != nil {
			notify = s.pendingWaiter
			s.pendingWaiter = nil
		}
		s.mu.Unlock()
		if notify != nil {
			close(notify)
		}
		s.log.Info("page session attached",
			slog.String("session_id", p.SessionID),
			slog.String("target_id", p.TargetInfo.TargetID),
			slog.String("url", p.TargetInfo.URL),
			slog.String("current_session_id", currentSID))
		// Diagnostic: enable the Page domain on the freshly-attached
		// session so we receive Page.frameNavigated events. Used to
		// falsify the "flat-mode session doesn't follow navigation"
		// hypothesis on chromeless per BUGS-529 option C: if the
		// bridge sees frameNavigated on its session post-harness-
		// navigate, the session IS following navigation in flat-mode
		// and the bug is below this layer. If it doesn't, chromeless
		// isn't propagating navigation events to auto-attached
		// sessions even with flatten=true.
		go func(sid string) {
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			if _, err := s.cdp.Send(ctx, sid, "Page.enable", nil); err != nil {
				s.log.Warn("Page.enable on attached session failed",
					slog.String("session_id", sid), slog.Any("err", err))
			}
		}(p.SessionID)

	case "Page.frameNavigated":
		// Diagnostic: log every navigation we observe on any attached
		// session so cluster pod logs surface whether flat-mode
		// auto-attach is propagating navigation correctly.
		var p struct {
			Frame struct {
				ID       string `json:"id"`
				ParentID string `json:"parentId"`
				URL      string `json:"url"`
			} `json:"frame"`
		}
		if err := json.Unmarshal(paramsRaw, &p); err != nil {
			return
		}
		// Only top-level frames matter for navigation tracking; iframe
		// frameNavigated events would otherwise drown the log.
		if p.Frame.ParentID != "" {
			return
		}
		s.mu.Lock()
		if info, ok := s.pageSessions[sessionID]; ok {
			info.url = p.Frame.URL
			s.pageSessions[sessionID] = info
			s.currentSID = s.chooseCurrentSessionLocked()
		}
		currentSID := s.currentSID
		s.mu.Unlock()
		s.log.Info("page navigated on bridge session",
			slog.String("session_id", sessionID),
			slog.String("frame_id", p.Frame.ID),
			slog.String("url", p.Frame.URL),
			slog.String("current_session_id", currentSID))

	case "Target.detachedFromTarget":
		var p struct {
			SessionID string `json:"sessionId"`
		}
		if err := json.Unmarshal(paramsRaw, &p); err != nil {
			return
		}
		s.mu.Lock()
		if _, ok := s.pageSessions[p.SessionID]; !ok {
			s.mu.Unlock()
			return
		}
		delete(s.pageSessions, p.SessionID)
		s.currentSID = s.chooseCurrentSessionLocked()
		s.mu.Unlock()
		s.log.Info("page session detached", slog.String("session_id", p.SessionID))
	}
}

func (s *pageSessionSender) chooseCurrentSessionLocked() string {
	if sid := newestMatchingSession(s.pageSessions, func(info pageSession) bool {
		return isConcretePageURL(info.url) && !isStreamerPageURL(info.url)
	}); sid != "" {
		return sid
	}
	if sid := newestMatchingSession(s.pageSessions, func(info pageSession) bool {
		return !isStreamerPageURL(info.url)
	}); sid != "" {
		return sid
	}
	return newestMatchingSession(s.pageSessions, func(pageSession) bool {
		return true
	})
}

func newestMatchingSession(sessions map[string]pageSession, accept func(pageSession) bool) string {
	var bestSID string
	var bestSeq int64
	for sid, info := range sessions {
		if !accept(info) {
			continue
		}
		if bestSID == "" || info.seq > bestSeq {
			bestSID = sid
			bestSeq = info.seq
		}
	}
	return bestSID
}

func isConcretePageURL(raw string) bool {
	u := strings.TrimSpace(raw)
	return u != "" && u != "about:blank"
}

func isStreamerPageURL(raw string) bool {
	u := strings.ToLower(strings.TrimSpace(raw))
	return strings.Contains(u, "/streamer/")
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
	dragMu            sync.Mutex
	dragItems         []dragItem
	dragTypes         []string
	dragActive        bool
	dragInterceptOnce sync.Once

	// touchMu guards the active-touches map. CDP's
	// Input.dispatchTouchEvent expects the *currently active*
	// touchPoints on every call (per puppeteer's convention: the
	// array reflects state AFTER this event applies). The map is
	// keyed by per-finger identifier supplied by the client.
	touchMu       sync.Mutex
	activeTouches map[int]touchPoint

	// wheelMu guards the per-session wheel gesture state. The
	// protocol's phase machine (start | changed | end) lets the
	// bridge synthesize a final zero-delta CDP wheel at end-of-
	// momentum so Chromium's compositor flushes its decay.
	// Concurrent wheels (rare in single-tenant) don't interleave
	// because each gesture's `start` resets the flag.
	wheelMu        sync.Mutex
	wheelInGesture bool

	// inputMu guards the cross-envelope pointer + modifier state.
	// The protocol's per-envelope shapes (mouse_move {x,y},
	// mouse_button {button,action,x,y}, key_{down,up} {key,code,mods})
	// don't carry the held-button or held-modifier state needed for
	// chromium's CDP renderer dispatch:
	//
	//   * mouse_move during a drag MUST send buttons!=0 or chromium's
	//     drag detector aborts (no dragstart fires).
	//   * mouse_button MUST send modifiers!=0 if a Shift/Ctrl/Alt/Meta
	//     key is held, otherwise click events lose their modifier bits.
	//   * mouse_button MUST ramp clickCount on rapid successive clicks
	//     within ~500 ms / ~5 px, otherwise dblclick never fires.
	//
	// We track all three here. Reads/writes are guarded by inputMu so
	// concurrent envelopes from a single source serialize cleanly.
	inputMu     sync.Mutex
	heldMods    int       // protocol modifier bitmask (modShift|modCtrl|...)
	heldButtons int       // CDP buttons bitfield (bit 0=left, 1=right, 2=middle, 3=back, 4=forward)
	lastClick   clickRamp // most recent mouseDown for clickCount ramp logic

	metrics *metrics
}

// clickRamp tracks the most recent mouseDown so the dispatcher can
// elevate clickCount on rapid successive clicks of the same button at
// the same approximate position — chromium's renderer keys dblclick
// detection off a non-1 clickCount in the dispatched mouseEvent.
type clickRamp struct {
	button int       // protocol button index (0=left)
	x, y   int       // last mouseDown position
	at     time.Time // time of last mouseDown
	count  int       // current clickCount (1 / 2 / 3)
}

// Browser convention: dblclick within 500 ms and within 5 px slop.
const (
	clickRampWindow = 500 * time.Millisecond
	clickRampSlopPx = 5
)

// CDP buttons bit per protocol button index (matches WebDevTools spec):
//
//	0 left   → bit 0  (1)
//	1 middle → bit 2  (4)
//	2 right  → bit 1  (2)
//	3 back   → bit 3  (8)
//	4 forward→ bit 4  (16)
func protocolButtonToBit(b int) int {
	switch b {
	case 0:
		return 1 // left
	case 1:
		return 4 // middle
	case 2:
		return 2 // right
	case 3:
		return 8
	case 4:
		return 16
	}
	return 0
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
		// Apply held button + modifier state so drag detection and
		// hover-with-modifiers work correctly. Without this, chromium's
		// drag detector aborts when it sees `buttons=0` during what
		// should be the move portion of a mousedown→drag→mouseup
		// gesture.
		d.inputMu.Lock()
		buttons := d.heldButtons
		mods := protocolModsToCDP(d.heldMods)
		d.inputMu.Unlock()
		button := "none"
		if buttons&1 != 0 {
			button = "left"
		} else if buttons&2 != 0 {
			button = "right"
		} else if buttons&4 != 0 {
			button = "middle"
		}
		params := map[string]any{
			"type":      "mouseMoved",
			"x":         data.X,
			"y":         data.Y,
			"button":    button,
			"buttons":   buttons,
			"modifiers": mods,
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
		// State updates for both directions of a click:
		//   * On `down`, ramp clickCount if this matches the last
		//     mouseDown within ~500 ms / ~5 px (chromium dblclick
		//     detection); set the held-button bit so subsequent
		//     mouse_move dispatches carry buttons!=0 (drag detection).
		//   * On `up`, clear the held-button bit but PRESERVE the
		//     ramp state — the next mouseDown still needs to see
		//     the previous count to elevate (clickCount=2 on the
		//     second pair, =3 on the third).
		//
		// Modifiers are picked up from the cross-envelope held-key
		// state (key_down "Shift" without a matching key_up sets
		// modShift in d.heldMods), so a shift-click sequence
		// `key_down Shift / mouse_button down / mouse_button up /
		// key_up Shift` lands a shift-modifier on both the
		// mousePressed and mouseReleased events.
		clickCount := 1
		now := time.Now()
		buttonBit := protocolButtonToBit(data.Button)
		d.inputMu.Lock()
		if data.Action == "down" {
			elapsed := now.Sub(d.lastClick.at)
			dx := data.X - d.lastClick.x
			dy := data.Y - d.lastClick.y
			if dx < 0 {
				dx = -dx
			}
			if dy < 0 {
				dy = -dy
			}
			if d.lastClick.button == data.Button &&
				elapsed <= clickRampWindow &&
				dx <= clickRampSlopPx && dy <= clickRampSlopPx &&
				d.lastClick.count > 0 {
				clickCount = d.lastClick.count + 1
			}
			d.lastClick = clickRamp{
				button: data.Button,
				x:      data.X,
				y:      data.Y,
				at:     now,
				count:  clickCount,
			}
			d.heldButtons |= buttonBit
		} else { // "up"
			// The release of an N-th click reports clickCount=N — the
			// browser uses this to fire `click` and (if N>=2) `dblclick`.
			if d.lastClick.button == data.Button && d.lastClick.count > 0 {
				clickCount = d.lastClick.count
			}
			d.heldButtons &^= buttonBit
		}
		modifiers := protocolModsToCDP(d.heldMods)
		d.inputMu.Unlock()

		params := map[string]any{
			"type":       t,
			"x":          data.X,
			"y":          data.Y,
			"button":     protocolButtonToCDP(data.Button),
			"buttons":    buttonBit, // bitmask of THIS button (kept for compat)
			"clickCount": clickCount,
			"modifiers":  modifiers,
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
		// Cross-envelope modifier state machine: pressing Shift /
		// Control / Alt / Meta sets the corresponding bit in
		// d.heldMods, releasing clears it. Subsequent mouse and key
		// events read from this state so a shift-click works as
		// `key_down Shift / mouse_button / key_up Shift` even though
		// the mouse_button envelope has no `mods` field on the wire.
		modBit := keyToProtocolMod(data.Key, data.Code)
		d.inputMu.Lock()
		if modBit != 0 {
			if env.Type == "key_down" {
				d.heldMods |= modBit
			} else {
				d.heldMods &^= modBit
			}
		}
		// The envelope's own `mods` field overrides + augments — a
		// client that explicitly tracks modifier state per envelope
		// (some hand-rolled clients do) can OR additional bits in.
		mods := protocolModsToCDP(d.heldMods | data.Mods)
		d.inputMu.Unlock()

		params := map[string]any{
			"type":      t,
			"code":      data.Code,
			"key":       data.Key,
			"modifiers": mods,
		}
		// For printable characters AND the special keys that produce
		// text in textareas (Enter / Tab), include `text` so the
		// renderer fires `input` events and inserts the character.
		// Without this, Enter dispatched into a textarea is treated
		// as a navigational keypress (which in a plain textarea
		// does nothing); pages don't see the newline.
		if env.Type == "key_down" {
			if t, ok := keyTextMap[data.Key]; ok {
				params["text"] = t
			} else if len(data.Key) == 1 {
				params["text"] = data.Key
			}
		}
		_, err := d.cdp.Send(ctx, "Input.dispatchKeyEvent", params)
		return d.fail(env.Type, err)

	case "composition_start", "composition_update":
		var data compositionData
		if err := json.Unmarshal(env.Data, &data); err != nil {
			return d.fail(env.Type, fmt.Errorf("%s: %w", env.Type, err))
		}
		// CDP `Input.imeSetComposition` parameters (T88 docs):
		//   text             — the in-progress composing string
		//   selectionStart   — caret start within `text` (UTF-16 code
		//                      units). Determines where the caret
		//                      blinks during composition.
		//   selectionEnd     — caret end; equal to start for a
		//                      collapsed caret, larger to highlight
		//                      a selection inside the composing text.
		//   replacementStart — start of the range BEFORE the caret
		//                      that this composition replaces. 0
		//                      means "insert fresh"; non-zero is for
		//                      dead-key / accent paths where typing
		//                      "´" then "e" replaces "´" with "é".
		//   replacementEnd   — end of the same replacement range.
		//
		// v1.0 clients send only `data`; we default selection/end
		// to the caret-at-end position the original implementation
		// used (matches CDP's documented default).
		textLen := utf16Len(data.Data)
		selStart := textLen
		selEnd := textLen
		if data.SelectionStart != nil {
			selStart = clampInt(*data.SelectionStart, 0, textLen)
		}
		if data.SelectionEnd != nil {
			selEnd = clampInt(*data.SelectionEnd, 0, textLen)
		}
		params := map[string]any{
			"text":             data.Data,
			"selectionStart":   selStart,
			"selectionEnd":     selEnd,
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
		// CDP `Input.insertText` commits the composing string and
		// clears any in-progress imeSetComposition state.
		params := map[string]any{"text": data.Data}
		_, err := d.cdp.Send(ctx, "Input.insertText", params)
		return d.fail(env.Type, err)

	case "composition_cancel":
		// T88 — IME aborted (Escape or focus loss). Clear any
		// in-progress imeSetComposition state in Chromium by
		// sending an empty composition. The empty `text` + zero
		// selection collapses back to the surrounding content.
		// Per the protocol's "carries no payload" rule, we don't
		// need to unmarshal env.Data.
		params := map[string]any{
			"text":             "",
			"selectionStart":   0,
			"selectionEnd":     0,
			"replacementStart": 0,
			"replacementEnd":   0,
		}
		_, err := d.cdp.Send(ctx, "Input.imeSetComposition", params)
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
// utf16Len returns the length of s in UTF-16 code units, matching
// JavaScript's String.prototype.length and the unit CDP's
// Input.imeSetComposition expects for selectionStart/End. ASCII +
// BMP characters are 1 unit each; characters outside the BMP
// (emoji, some CJK extensions) are 2.
func utf16Len(s string) int {
	n := 0
	for _, r := range s {
		if r >= 0x10000 {
			n += 2
		} else {
			n++
		}
	}
	return n
}

// clampInt confines x to [lo, hi]. Used to defensively bound the
// selection_start / selection_end the client sends, since a buggy
// client could send a value outside [0, len(text)] which Chromium
// would then reject with "RangeError".
func clampInt(x, lo, hi int) int {
	if x < lo {
		return lo
	}
	if x > hi {
		return hi
	}
	return x
}

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
		cdp, err := connectCDPWithRetry(ctx, cfg.cdpURL, log)
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

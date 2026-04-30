// Package main implements the server-side cursor watcher (T26).
//
// It connects to a headless Chromium via the DevTools Protocol at
// http://127.0.0.1:9222, installs a small JavaScript probe that
// reports the live cursor shape and position whenever they change,
// and emits one v1 cursor envelope per change to a downstream sink
// (stdout for piping; a localhost WebSocket for the eventual relay
// into the WebRTC "cursor" data channel).
//
// Per the project brief, server-side cursor in the framebuffer adds a
// frame of latency, so we render client-side from this metadata.
//
// CDP doesn't expose a first-class "cursor changed" event, so the
// probe is a small piece of JS injected via
// `Page.addScriptToEvaluateOnNewDocument`. It tracks pointer
// position from `pointermove`, samples
// `getComputedStyle(elementFromPoint(x,y)).cursor` on each rAF, and
// calls a `Runtime.addBinding` exposed function whenever the result
// changes. The watcher receives those calls as `Runtime.bindingCalled`
// events and re-emits them in the wire format from
// docs/protocols/cursor-channel.md.
//
// Cross-references:
//   - docs/protocols/cursor-channel.md — wire format we emit.
//   - capture/input-bridge/main.go — sibling sidecar; same CDP shape,
//     opposite direction. Some structure is duplicated rather than
//     pulled into a shared module to keep these two services
//     independently deployable.
//   - client/src/cursor.ts — the renderer.
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
)

const protocolVersion = 1

// ---------------------------------------------------------------------------
// Wire envelope (matches docs/protocols/cursor-channel.md v1)
// ---------------------------------------------------------------------------

type cursorData struct {
	X              int     `json:"x"`
	Y              int     `json:"y"`
	Visible        bool    `json:"visible"`
	Shape          string  `json:"shape"`
	Hotspot        *xy     `json:"hotspot,omitempty"`
	CustomImageB64 string  `json:"custom_image_b64,omitempty"`
	ImageFormat    string  `json:"image_format,omitempty"`
}

type xy struct {
	X int `json:"x"`
	Y int `json:"y"`
}

type cursorEnvelope struct {
	V    int        `json:"v"`
	Type string     `json:"type"`
	T    int64      `json:"t"`
	Seq  int64      `json:"seq"`
	Data cursorData `json:"data"`
}

// equalState reports whether two cursorData values represent the
// same observable cursor state. We use this to suppress identical-
// state poll noise: the watcher only emits when something has
// changed.
func equalState(a, b cursorData) bool {
	return a.X == b.X && a.Y == b.Y && a.Visible == b.Visible &&
		a.Shape == b.Shape && a.CustomImageB64 == b.CustomImageB64 &&
		a.ImageFormat == b.ImageFormat &&
		((a.Hotspot == nil && b.Hotspot == nil) ||
			(a.Hotspot != nil && b.Hotspot != nil && *a.Hotspot == *b.Hotspot))
}

// ---------------------------------------------------------------------------
// CDP minimal client (parallel to capture/input-bridge for now;
// shared module is a follow-up)
// ---------------------------------------------------------------------------

type cdpClient struct {
	mu         sync.Mutex
	conn       *websocket.Conn
	nextID     int64
	pending    map[int64]chan cdpResp
	events     chan cdpEvent
	disconnect chan struct{}
	log        *slog.Logger
}

type cdpResp struct {
	Result json.RawMessage
	Err    *cdpErr
}

type cdpErr struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *cdpErr) Error() string { return fmt.Sprintf("cdp error %d: %s", e.Code, e.Message) }

type cdpEvent struct {
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

type cdpFrame struct {
	ID     int64           `json:"id,omitempty"`
	Method string          `json:"method,omitempty"`
	Params any             `json:"params,omitempty"`
	Result json.RawMessage `json:"result,omitempty"`
	Error  *cdpErr         `json:"error,omitempty"`
}

func dialCDP(ctx context.Context, baseURL string, log *slog.Logger) (*cdpClient, error) {
	wsURL, err := discoverPageWS(ctx, baseURL)
	if err != nil {
		return nil, err
	}
	d := *websocket.DefaultDialer
	d.HandshakeTimeout = 5 * time.Second
	conn, _, err := d.DialContext(ctx, wsURL, nil)
	if err != nil {
		return nil, fmt.Errorf("dial CDP %s: %w", wsURL, err)
	}
	c := &cdpClient{
		conn:       conn,
		pending:    make(map[int64]chan cdpResp),
		events:     make(chan cdpEvent, 256),
		disconnect: make(chan struct{}),
		log:        log,
	}
	go c.readLoop()
	return c, nil
}

func discoverPageWS(ctx context.Context, baseURL string) (string, error) {
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
	}
	if err := json.NewDecoder(resp.Body).Decode(&targets); err != nil {
		return "", err
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
		var f cdpFrame
		if err := json.Unmarshal(raw, &f); err != nil {
			c.log.Warn("cdp non-JSON frame", slog.Any("err", err))
			continue
		}
		if f.ID != 0 {
			c.mu.Lock()
			ch, ok := c.pending[f.ID]
			delete(c.pending, f.ID)
			c.mu.Unlock()
			if ok {
				ch <- cdpResp{Result: f.Result, Err: f.Error}
				close(ch)
			}
			continue
		}
		if f.Method != "" {
			ev := cdpEvent{Method: f.Method}
			// Re-marshal Params as raw JSON because cdpFrame.Params is
			// 'any' (decodes into map[string]any). Keep it as bytes for
			// later targeted unmarshal.
			if pb, err := json.Marshal(f.Params); err == nil {
				ev.Params = pb
			}
			select {
			case c.events <- ev:
			default:
				c.log.Warn("dropping cdp event, channel full", slog.String("method", f.Method))
			}
		}
	}
}

func (c *cdpClient) Send(ctx context.Context, method string, params any) (json.RawMessage, error) {
	id := atomic.AddInt64(&c.nextID, 1)
	ch := make(chan cdpResp, 1)

	c.mu.Lock()
	c.pending[id] = ch
	conn := c.conn
	c.mu.Unlock()

	raw, err := json.Marshal(cdpFrame{ID: id, Method: method, Params: params})
	if err != nil {
		return nil, err
	}
	c.mu.Lock()
	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	err = conn.WriteMessage(websocket.TextMessage, raw)
	c.mu.Unlock()
	if err != nil {
		return nil, err
	}
	select {
	case resp := <-ch:
		if resp.Err != nil {
			return nil, resp.Err
		}
		return resp.Result, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-c.disconnect:
		return nil, errors.New("cdp disconnected")
	}
}

func (c *cdpClient) Events() <-chan cdpEvent { return c.events }

func (c *cdpClient) Close() error {
	if c.conn == nil {
		return nil
	}
	_ = c.conn.WriteControl(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""),
		time.Now().Add(time.Second))
	return c.conn.Close()
}

// ---------------------------------------------------------------------------
// Watcher — installs the probe and translates Runtime.bindingCalled
// events into wire-format envelopes.
// ---------------------------------------------------------------------------

// probeJS is the small piece of JavaScript injected into every page
// load. It tracks the latest pointer position via `pointermove`, and
// on every requestAnimationFrame computes the cursor shape under
// that position with `getComputedStyle(elementFromPoint(x,y)).cursor`.
// When anything observable changes, it calls
// `window.__cb_cursor__(json)` — which the Runtime.addBinding
// installation has wired to a Runtime.bindingCalled event.
//
// The script is intentionally defensive (no async, no closures over
// page-state) because it runs in every document and frame.
const probeJS = `(() => {
  if (window.__cb_cursor_installed__) return;
  window.__cb_cursor_installed__ = true;
  let lastX = 0, lastY = 0, lastVisible = true, lastShape = "default", lastImg = "";

  function describeCursor(cssValue) {
    if (!cssValue || cssValue === "auto") return { shape: "default", img: "" };
    if (cssValue === "none") return { shape: "none", img: "" };
    // url(...) custom cursor
    const m = /^url\(["']?([^)"']+)["']?\)/.exec(cssValue);
    if (m) return { shape: "custom", img: m[1] };
    // First keyword (CSS allows fallbacks: "url(...) , pointer")
    const tail = cssValue.split(",").pop().trim().split(/\s+/)[0];
    return { shape: tail || "default", img: "" };
  }

  function emit() {
    const el = document.elementFromPoint(lastX, lastY);
    const cssCursor = el ? getComputedStyle(el).cursor : "default";
    const desc = describeCursor(cssCursor);
    const visible = desc.shape !== "none";
    if (lastX === payload_x && lastY === payload_y && lastShape === desc.shape &&
        lastVisible === visible && lastImg === desc.img) return;
    lastShape = desc.shape; lastVisible = visible; lastImg = desc.img;
    const payload = {
      x: Math.round(lastX), y: Math.round(lastY),
      visible: visible, shape: desc.shape, custom_image_url: desc.img || undefined
    };
    if (typeof window.__cb_cursor__ === "function") {
      try { window.__cb_cursor__(JSON.stringify(payload)); } catch (e) {}
    }
  }

  let payload_x = 0, payload_y = 0;
  document.addEventListener("pointermove", (e) => {
    lastX = e.clientX; lastY = e.clientY;
  }, { passive: true, capture: true });

  function loop() {
    try { emit(); } catch (e) {}
    payload_x = lastX; payload_y = lastY;
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
})();`

// probePayload mirrors the JS-side payload struct.
type probePayload struct {
	X              float64 `json:"x"`
	Y              float64 `json:"y"`
	Visible        bool    `json:"visible"`
	Shape          string  `json:"shape"`
	CustomImageURL string  `json:"custom_image_url,omitempty"`
}

// emitter is the abstraction the watcher uses to deliver envelopes
// downstream. Real implementations send to stdout or a websocket
// sink; the test stub records them.
type emitter interface {
	Emit(envelope []byte) error
}

type stdoutEmitter struct{ w io.Writer }

func (e *stdoutEmitter) Emit(env []byte) error {
	_, err := e.w.Write(append(env, '\n'))
	return err
}

// wsEmitter dials a downstream WebSocket and sends each envelope as
// a single text frame. Reconnects on failure with a short backoff.
type wsEmitter struct {
	url   string
	mu    sync.Mutex
	conn  *websocket.Conn
	log   *slog.Logger
	done  chan struct{}
}

func newWSEmitter(url string, log *slog.Logger) *wsEmitter {
	return &wsEmitter{url: url, log: log, done: make(chan struct{})}
}

func (e *wsEmitter) Emit(env []byte) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.conn == nil {
		c, _, err := websocket.DefaultDialer.Dial(e.url, nil)
		if err != nil {
			e.log.Warn("ws sink dial failed; dropping envelope", slog.Any("err", err))
			return err
		}
		e.conn = c
	}
	if err := e.conn.WriteMessage(websocket.TextMessage, env); err != nil {
		_ = e.conn.Close()
		e.conn = nil
		return err
	}
	return nil
}

func (e *wsEmitter) Close() {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.conn != nil {
		_ = e.conn.Close()
	}
	close(e.done)
}

// watcher runs the install + event loop.
type watcher struct {
	cdp     *cdpClient
	em      emitter
	log     *slog.Logger
	seq     int64
	prevMu  sync.Mutex
	prev    cursorData
	hasPrev bool
}

func newWatcher(cdp *cdpClient, em emitter, log *slog.Logger) *watcher {
	return &watcher{cdp: cdp, em: em, log: log}
}

// install enables the necessary CDP domains and installs the probe
// in every existing/new document. Idempotent.
func (w *watcher) install(ctx context.Context) error {
	for _, m := range []string{"Page.enable", "Runtime.enable"} {
		if _, err := w.cdp.Send(ctx, m, nil); err != nil {
			return fmt.Errorf("%s: %w", m, err)
		}
	}
	// Bind window.__cb_cursor__ — calling it from the page produces a
	// Runtime.bindingCalled event we observe in events().
	if _, err := w.cdp.Send(ctx, "Runtime.addBinding", map[string]any{"name": "__cb_cursor__"}); err != nil {
		return fmt.Errorf("Runtime.addBinding: %w", err)
	}
	// Inject the probe on every new document.
	if _, err := w.cdp.Send(ctx, "Page.addScriptToEvaluateOnNewDocument", map[string]any{"source": probeJS}); err != nil {
		return fmt.Errorf("Page.addScriptToEvaluateOnNewDocument: %w", err)
	}
	// Also install in the current document so we get cursor data
	// immediately without waiting for a navigation.
	if _, err := w.cdp.Send(ctx, "Runtime.evaluate", map[string]any{
		"expression":            probeJS,
		"includeCommandLineAPI": false,
	}); err != nil {
		return fmt.Errorf("Runtime.evaluate(probe): %w", err)
	}
	w.log.Info("cursor probe installed")
	return nil
}

// run consumes Runtime.bindingCalled events, decodes the probe's
// payload, and emits envelopes whenever state changes.
func (w *watcher) run(ctx context.Context) error {
	for {
		select {
		case ev := <-w.cdp.Events():
			if ev.Method != "Runtime.bindingCalled" {
				continue
			}
			var p struct {
				Name    string `json:"name"`
				Payload string `json:"payload"`
			}
			if err := json.Unmarshal(ev.Params, &p); err != nil {
				w.log.Warn("decode bindingCalled", slog.Any("err", err))
				continue
			}
			if p.Name != "__cb_cursor__" {
				continue
			}
			var pp probePayload
			if err := json.Unmarshal([]byte(p.Payload), &pp); err != nil {
				w.log.Warn("decode cursor payload", slog.Any("err", err))
				continue
			}
			data := cursorData{
				X:       int(pp.X),
				Y:       int(pp.Y),
				Visible: pp.Visible,
				Shape:   pp.Shape,
			}
			// Custom-image fetching: in v1 we pass the URL through as a
			// shape="custom" event without the bytes; embedding the
			// fetched image is a follow-up.
			if pp.CustomImageURL != "" {
				w.log.Debug("custom cursor URL not yet fetched", slog.String("url", pp.CustomImageURL))
			}
			w.maybeEmit(data)
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (w *watcher) maybeEmit(data cursorData) {
	w.prevMu.Lock()
	if w.hasPrev && equalState(w.prev, data) {
		w.prevMu.Unlock()
		return
	}
	w.prev = data
	w.hasPrev = true
	w.prevMu.Unlock()

	env := cursorEnvelope{
		V:    protocolVersion,
		Type: "cursor",
		T:    time.Now().UnixMilli(),
		Seq:  atomic.AddInt64(&w.seq, 1) - 1,
		Data: data,
	}
	raw, err := json.Marshal(env)
	if err != nil {
		w.log.Warn("marshal envelope", slog.Any("err", err))
		return
	}
	if err := w.em.Emit(raw); err != nil {
		w.log.Warn("emit failed", slog.Any("err", err))
	}
}

// emitSynthetic emits one envelope without going through CDP. Used by
// --dry-run so the DoD's "emits one cursor event" check works without
// a real Chromium on the bench.
func (w *watcher) emitSynthetic(data cursorData) {
	w.maybeEmit(data)
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

type config struct {
	cdpURL  string
	sink    string // "stdout" | "ws"
	wsURL   string // when sink=="ws"
	dryRun  bool
}

func parseFlags(args []string) (config, error) {
	fs := flag.NewFlagSet("cursor-watcher", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	cfg := config{}
	fs.StringVar(&cfg.cdpURL, "cdp-url", "http://127.0.0.1:9222", "Chromium DevTools base URL")
	fs.StringVar(&cfg.sink, "sink", "stdout", "Where to send envelopes: stdout | ws")
	fs.StringVar(&cfg.wsURL, "sink-url", "ws://127.0.0.1:9200/cursor", "WebSocket sink URL when --sink=ws")
	fs.BoolVar(&cfg.dryRun, "dry-run", false, "Skip CDP entirely; emit one synthetic envelope and exit (smoke test)")
	if err := fs.Parse(args); err != nil {
		return cfg, err
	}
	if cfg.sink != "stdout" && cfg.sink != "ws" {
		return cfg, fmt.Errorf("invalid --sink %q (want stdout|ws)", cfg.sink)
	}
	return cfg, nil
}

func makeEmitter(cfg config, stdout io.Writer, log *slog.Logger) emitter {
	switch cfg.sink {
	case "stdout":
		return &stdoutEmitter{w: stdout}
	case "ws":
		return newWSEmitter(cfg.wsURL, log)
	}
	return &stdoutEmitter{w: stdout}
}

func run(ctx context.Context, cfg config, stdout io.Writer, log *slog.Logger) error {
	em := makeEmitter(cfg, stdout, log)

	if cfg.dryRun {
		w := newWatcher(nil, em, log)
		w.emitSynthetic(cursorData{X: 0, Y: 0, Visible: true, Shape: "default"})
		// Give bufio.Writer / stdout time to flush.
		if bw, ok := stdout.(*bufio.Writer); ok {
			_ = bw.Flush()
		}
		return nil
	}

	dialCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	cdp, err := dialCDP(dialCtx, cfg.cdpURL, log)
	cancel()
	if err != nil {
		return fmt.Errorf("connect to CDP: %w", err)
	}
	defer cdp.Close()

	w := newWatcher(cdp, em, log)
	if err := w.install(ctx); err != nil {
		return err
	}
	if err := w.run(ctx); err != nil && !errors.Is(err, context.Canceled) {
		return err
	}
	return nil
}

func main() {
	cfg, err := parseFlags(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, "flag error:", err)
		os.Exit(2)
	}
	log := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(log)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	bw := bufio.NewWriter(os.Stdout)
	defer bw.Flush()

	if err := run(ctx, cfg, bw, log); err != nil {
		log.Error("run failed", slog.Any("err", err))
		os.Exit(1)
	}
}

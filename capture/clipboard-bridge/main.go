// Package main is the bidirectional clipboard sync sidecar (T32).
//
// It carries v1 clipboard_offer envelopes (per
// docs/protocols/clipboard-channel.md) between the WebRTC "clipboard"
// data channel and the cloud Chromium. There are two halves:
//
//   * Inbound (client→cloud): receive envelopes on stdin or a
//     localhost WebSocket; for each, write the text into the cloud
//     Chromium's clipboard via Browser.grantPermissions +
//     navigator.clipboard.writeText via Runtime.evaluate, then
//     synthesize Ctrl+V into the focused element so the user
//     observes a real paste event.
//
//   * Outbound (cloud→client): inject a tiny JS probe in every page
//     that listens for "copy" events and reports the copied text via
//     a Runtime.addBinding callback. The bridge translates that into
//     an outbound clipboard_offer envelope written to stdout (or the
//     ws relay).
//
// Strict v1 stance per the protocol — text only, user-action gated,
// 1 MiB cap, never silent polling.
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

const (
	protocolVersion = 1
	maxBytes        = 1 << 20 // 1 MiB
)

// ---------------------------------------------------------------------------
// Wire types — must match docs/protocols/clipboard-channel.md (v1)
// ---------------------------------------------------------------------------

type clipboardData struct {
	Direction string `json:"direction"` // "client->cloud" | "cloud->client"
	Source    string `json:"source"`    // "user_action"
	Text      string `json:"text"`
}

type clipboardEnvelope struct {
	V    int           `json:"v"`
	Type string        `json:"type"`
	T    int64         `json:"t"`
	Seq  int64         `json:"seq"`
	Data clipboardData `json:"data"`
}

func parseEnvelope(raw []byte) (clipboardEnvelope, error) {
	var e clipboardEnvelope
	if err := json.Unmarshal(raw, &e); err != nil {
		return e, fmt.Errorf("invalid JSON: %w", err)
	}
	if e.V != protocolVersion {
		return e, fmt.Errorf("unsupported protocol version v=%d (want %d)", e.V, protocolVersion)
	}
	if e.Type != "clipboard_offer" {
		return e, fmt.Errorf("unsupported type %q", e.Type)
	}
	if e.Data.Source != "user_action" {
		return e, fmt.Errorf("unsupported source %q", e.Data.Source)
	}
	if e.Data.Direction != "client->cloud" && e.Data.Direction != "cloud->client" {
		return e, fmt.Errorf("invalid direction %q", e.Data.Direction)
	}
	if len(e.Data.Text) > maxBytes {
		return e, fmt.Errorf("text exceeds 1 MiB cap (%d bytes)", len(e.Data.Text))
	}
	return e, nil
}

// ---------------------------------------------------------------------------
// CDP minimal client (parallel to capture/{input-bridge,cursor-watcher}
// for now — shared module is a follow-up)
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
	Method string
	Params json.RawMessage
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
		return "", err
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
			pb, _ := json.Marshal(f.Params)
			select {
			case c.events <- cdpEvent{Method: f.Method, Params: pb}:
			default:
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
	case r := <-ch:
		if r.Err != nil {
			return nil, r.Err
		}
		return r.Result, nil
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
// Bridge — applies inbound and observes outbound
// ---------------------------------------------------------------------------

// cdpSender is the small subset of cdpClient the bridge depends on.
// Tests substitute a fake.
type cdpSender interface {
	Send(ctx context.Context, method string, params any) (json.RawMessage, error)
}

// outProbeJS is injected via Page.addScriptToEvaluateOnNewDocument so
// every page reports `copy` events back to the bridge.
const outProbeJS = `(() => {
  if (window.__cb_clip_installed__) return;
  window.__cb_clip_installed__ = true;
  document.addEventListener("copy", (e) => {
    try {
      const sel = (window.getSelection && window.getSelection().toString()) || "";
      const txt = (e.clipboardData && e.clipboardData.getData("text/plain")) || sel;
      if (txt && typeof window.__cb_clip__ === "function") {
        window.__cb_clip__(txt);
      }
    } catch (err) {}
  }, { capture: true });
})();`

type bridge struct {
	cdp    cdpSender
	out    emitter
	log    *slog.Logger
	seq    int64
	origin string // origin to grant clipboard permission to; auto-detected
	mu     sync.Mutex
	last   string // last text we wrote to the cloud clipboard, for echo suppression
}

type emitter interface {
	Emit(b []byte) error
}

type stdoutEmitter struct{ w io.Writer }

func (e *stdoutEmitter) Emit(b []byte) error {
	_, err := e.w.Write(append(b, '\n'))
	return err
}

type wsEmitter struct {
	url  string
	mu   sync.Mutex
	conn *websocket.Conn
	log  *slog.Logger
}

func (e *wsEmitter) Emit(b []byte) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.conn == nil {
		c, _, err := websocket.DefaultDialer.Dial(e.url, nil)
		if err != nil {
			return err
		}
		e.conn = c
	}
	if err := e.conn.WriteMessage(websocket.TextMessage, b); err != nil {
		_ = e.conn.Close()
		e.conn = nil
		return err
	}
	return nil
}

func newBridge(c cdpSender, out emitter, log *slog.Logger) *bridge {
	return &bridge{cdp: c, out: out, log: log}
}

// install enables CDP domains, grants clipboard permissions, and
// installs the copy probe.
func (b *bridge) install(ctx context.Context) error {
	for _, m := range []string{"Page.enable", "Runtime.enable"} {
		if _, err := b.cdp.Send(ctx, m, nil); err != nil {
			return fmt.Errorf("%s: %w", m, err)
		}
	}
	// Grant clipboard read+write to all origins for this Browser-level
	// session; this is the v1 narrow path — the bridge only runs in a
	// single-tenant container, so granting browser-wide is acceptable.
	if _, err := b.cdp.Send(ctx, "Browser.grantPermissions", map[string]any{
		"permissions": []string{"clipboardReadWrite", "clipboardSanitizedWrite"},
	}); err != nil {
		// Some Chromium builds don't accept "clipboardSanitizedWrite";
		// retry with just the basic permission.
		if _, err2 := b.cdp.Send(ctx, "Browser.grantPermissions", map[string]any{
			"permissions": []string{"clipboardReadWrite"},
		}); err2 != nil {
			b.log.Warn("Browser.grantPermissions failed; clipboard.writeText will likely fail",
				slog.Any("err", err2))
		}
	}
	if _, err := b.cdp.Send(ctx, "Runtime.addBinding", map[string]any{"name": "__cb_clip__"}); err != nil {
		return fmt.Errorf("Runtime.addBinding: %w", err)
	}
	if _, err := b.cdp.Send(ctx, "Page.addScriptToEvaluateOnNewDocument", map[string]any{"source": outProbeJS}); err != nil {
		return fmt.Errorf("Page.addScriptToEvaluateOnNewDocument: %w", err)
	}
	if _, err := b.cdp.Send(ctx, "Runtime.evaluate", map[string]any{
		"expression":            outProbeJS,
		"includeCommandLineAPI": false,
	}); err != nil {
		return fmt.Errorf("Runtime.evaluate(probe): %w", err)
	}
	b.log.Info("clipboard probe installed")
	return nil
}

// applyInbound writes text to the cloud Chromium clipboard.
//
// Strategy choice (documented in README):
//   1. PRIMARY — Runtime.evaluate(`navigator.clipboard.writeText(...)`)
//      This actually updates the OS clipboard inside the container so
//      a Ctrl+V into the focused element fires a real `paste` event,
//      and any other JS that reads the clipboard sees the new value.
//   2. SECONDARY — Input.insertText
//      Inserts the text into the focused element directly. Doesn't
//      touch the clipboard, but works even when permissions or focus
//      cause #1 to fail.
//
// We try #1 first, then synthesize Ctrl+V to deliver a real paste
// event to the focused element. If #1 reports an exception, we
// degrade to #2.
func (b *bridge) applyInbound(ctx context.Context, text string) error {
	b.mu.Lock()
	b.last = text
	b.mu.Unlock()

	// Use an awaited Promise so Runtime.evaluate's `result` reflects
	// success/failure of the writeText call itself.
	expr := fmt.Sprintf(`navigator.clipboard.writeText(%s).then(()=>"ok").catch(e=>"err:"+e.message)`,
		quoteJSString(text))
	res, err := b.cdp.Send(ctx, "Runtime.evaluate", map[string]any{
		"expression":   expr,
		"awaitPromise": true,
		"returnByValue": true,
	})
	wroteToClipboard := false
	if err == nil {
		var rv struct {
			Result struct {
				Value string `json:"value"`
			} `json:"result"`
		}
		if jerr := json.Unmarshal(res, &rv); jerr == nil && rv.Result.Value == "ok" {
			wroteToClipboard = true
		} else {
			b.log.Warn("clipboard.writeText returned non-ok",
				slog.String("value", rv.Result.Value))
		}
	} else {
		b.log.Warn("Runtime.evaluate(writeText) failed", slog.Any("err", err))
	}

	if wroteToClipboard {
		// Synthesize Ctrl+V so the focused element fires its `paste`
		// handler and sees the new clipboard contents.
		_, _ = b.cdp.Send(ctx, "Input.dispatchKeyEvent", map[string]any{
			"type": "keyDown", "code": "KeyV", "key": "v", "modifiers": 2, // Ctrl
		})
		_, _ = b.cdp.Send(ctx, "Input.dispatchKeyEvent", map[string]any{
			"type": "keyUp", "code": "KeyV", "key": "v", "modifiers": 2,
		})
		return nil
	}

	// Fallback: insert text directly. The OS clipboard isn't updated,
	// but the focused element receives the content.
	_, err = b.cdp.Send(ctx, "Input.insertText", map[string]any{"text": text})
	if err != nil {
		return fmt.Errorf("Input.insertText fallback: %w", err)
	}
	return nil
}

// runOutbound consumes Runtime.bindingCalled events and emits
// "cloud->client" envelopes. Returns when ctx is cancelled.
func (b *bridge) runOutbound(ctx context.Context, events <-chan cdpEvent) error {
	for {
		select {
		case ev, ok := <-events:
			if !ok {
				return nil
			}
			if ev.Method != "Runtime.bindingCalled" {
				continue
			}
			var p struct {
				Name    string `json:"name"`
				Payload string `json:"payload"`
			}
			if err := json.Unmarshal(ev.Params, &p); err != nil {
				continue
			}
			if p.Name != "__cb_clip__" {
				continue
			}
			text := p.Payload
			if len(text) > maxBytes {
				b.log.Warn("dropping outbound clipboard: oversize",
					slog.Int("bytes", len(text)))
				continue
			}
			// Echo suppression: if the cloud copy is exactly the text we
			// just wrote inbound, swallow it — that's the same content
			// bouncing back, not a new user action.
			b.mu.Lock()
			same := text == b.last
			b.mu.Unlock()
			if same {
				continue
			}
			env := clipboardEnvelope{
				V:    protocolVersion,
				Type: "clipboard_offer",
				T:    time.Now().UnixMilli(),
				Seq:  atomic.AddInt64(&b.seq, 1) - 1,
				Data: clipboardData{
					Direction: "cloud->client",
					Source:    "user_action",
					Text:      text,
				},
			}
			raw, _ := json.Marshal(env)
			if err := b.out.Emit(raw); err != nil {
				b.log.Warn("emit outbound failed", slog.Any("err", err))
			}
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

// quoteJSString produces a JS string literal for use inside Runtime.evaluate.
// We use json.Marshal because it already handles quoting / escaping / unicode.
func quoteJSString(s string) string {
	b, err := json.Marshal(s)
	if err != nil {
		return `""`
	}
	return string(b)
}

// ---------------------------------------------------------------------------
// Sources — stdin and websocket
// ---------------------------------------------------------------------------

func runStdin(ctx context.Context, r io.Reader, handle func(context.Context, []byte)) error {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64*1024), 4*maxBytes)
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
		buf := make([]byte, len(line))
		copy(buf, line)
		handle(ctx, buf)
	}
	return sc.Err()
}

func runWS(ctx context.Context, addr, path string, handle func(context.Context, []byte), log *slog.Logger) error {
	mux := http.NewServeMux()
	upg := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	mux.HandleFunc(path, func(w http.ResponseWriter, r *http.Request) {
		conn, err := upg.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		conn.SetReadLimit(int64(4 * maxBytes))
		for {
			_, raw, err := conn.ReadMessage()
			if err != nil {
				return
			}
			handle(ctx, raw)
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
// Lifecycle
// ---------------------------------------------------------------------------

type config struct {
	source  string // stdin | ws
	wsAddr  string
	wsPath  string
	cdpURL  string
	sink    string // stdout | ws
	sinkURL string
	dryRun  bool
}

func parseFlags(args []string) (config, error) {
	fs := flag.NewFlagSet("clipboard-bridge", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	cfg := config{}
	fs.StringVar(&cfg.source, "source", "stdin", "Inbound source: stdin | ws")
	fs.StringVar(&cfg.wsAddr, "ws-addr", "127.0.0.1:9300", "WS source listen addr")
	fs.StringVar(&cfg.wsPath, "ws-path", "/clipboard", "WS source path")
	fs.StringVar(&cfg.cdpURL, "cdp-url", "http://127.0.0.1:9222", "Chromium DevTools base URL")
	fs.StringVar(&cfg.sink, "sink", "stdout", "Outbound sink: stdout | ws")
	fs.StringVar(&cfg.sinkURL, "sink-url", "ws://127.0.0.1:9301/clipboard", "WS sink URL")
	fs.BoolVar(&cfg.dryRun, "dry-run", false, "Skip CDP entirely")
	if err := fs.Parse(args); err != nil {
		return cfg, err
	}
	if cfg.source != "stdin" && cfg.source != "ws" {
		return cfg, fmt.Errorf("invalid --source %q", cfg.source)
	}
	if cfg.sink != "stdout" && cfg.sink != "ws" {
		return cfg, fmt.Errorf("invalid --sink %q", cfg.sink)
	}
	return cfg, nil
}

func makeEmitter(cfg config, stdout io.Writer, log *slog.Logger) emitter {
	switch cfg.sink {
	case "ws":
		return &wsEmitter{url: cfg.sinkURL, log: log}
	default:
		return &stdoutEmitter{w: stdout}
	}
}

func run(ctx context.Context, cfg config, stdin io.Reader, stdout io.Writer, log *slog.Logger) error {
	em := makeEmitter(cfg, stdout, log)

	var sender cdpSender
	var events <-chan cdpEvent
	if cfg.dryRun {
		// no-op CDP; events channel is empty.
		sender = noopCDP{}
		c := make(chan cdpEvent)
		events = c
		// Don't close — runOutbound will exit when ctx is cancelled.
	} else {
		dialCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		c, err := dialCDP(dialCtx, cfg.cdpURL, log)
		cancel()
		if err != nil {
			return fmt.Errorf("connect to CDP: %w", err)
		}
		defer c.Close()
		sender = c
		events = c.Events()
	}

	br := newBridge(sender, em, log)
	if !cfg.dryRun {
		if err := br.install(ctx); err != nil {
			return err
		}
	}

	handle := func(ctx context.Context, raw []byte) {
		env, err := parseEnvelope(raw)
		if err != nil {
			log.Warn("inbound parse failed", slog.Any("err", err))
			return
		}
		if env.Data.Direction != "client->cloud" {
			// We ignore cloud->client envelopes coming back at us.
			return
		}
		applyCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
		defer cancel()
		if cfg.dryRun {
			log.Info("dry-run inbound", slog.String("text", env.Data.Text))
			return
		}
		if err := br.applyInbound(applyCtx, env.Data.Text); err != nil {
			log.Warn("applyInbound failed", slog.Any("err", err))
		}
	}

	// Outbound loop runs in the background.
	outCtx, outCancel := context.WithCancel(ctx)
	defer outCancel()
	outDone := make(chan struct{})
	go func() {
		defer close(outDone)
		_ = br.runOutbound(outCtx, events)
	}()

	var srcErr error
	switch cfg.source {
	case "stdin":
		log.Info("reading inbound envelopes from stdin")
		srcErr = runStdin(ctx, stdin, handle)
	case "ws":
		log.Info("listening for inbound envelopes",
			slog.String("addr", cfg.wsAddr), slog.String("path", cfg.wsPath))
		srcErr = runWS(ctx, cfg.wsAddr, cfg.wsPath, handle, log)
	}
	outCancel()
	<-outDone
	if srcErr != nil &&
		!errors.Is(srcErr, context.Canceled) &&
		!errors.Is(srcErr, context.DeadlineExceeded) {
		return srcErr
	}
	return nil
}

type noopCDP struct{}

func (noopCDP) Send(_ context.Context, _ string, _ any) (json.RawMessage, error) {
	return []byte(`{"result":{"value":"ok"}}`), nil
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

	if err := run(ctx, cfg, os.Stdin, os.Stdout, log); err != nil {
		log.Error("run failed", slog.Any("err", err))
		os.Exit(1)
	}
}

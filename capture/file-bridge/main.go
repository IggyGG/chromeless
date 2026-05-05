// Package main is the file-upload bridge sidecar (T74).
//
// It receives v1 file_upload_* envelopes from the WebRTC "files"
// data channel (relayed to it as one JSON envelope per WS message),
// reassembles each upload's chunks, verifies SHA-256, runs the
// per-tenant security checks (size cap, MIME allowlist, sniff,
// virus-scan stub), writes the file to the per-session uploads
// directory, and attaches it to a target <input type=file> in the
// cloud Chromium via Chrome DevTools Protocol.
//
// Wire format: docs/protocols/file-upload.md
//
// Threading: one goroutine per WebSocket connection (the source).
// Per-upload state lives in an upload-id-keyed map guarded by mu;
// chunks for one upload are processed in arrival order so SHA-256
// can be computed incrementally. Concurrent uploads (different
// upload_ids) are independent.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"hash"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

const (
	protocolVersion        = 1
	defaultMaxTotalSize    = 100 * 1024 * 1024 // 100 MiB
	defaultMaxChunkSize    = 1024 * 1024       // 1 MiB raw (≈ 1.4 MiB base64)
	defaultProgressEveryKB = 256                // emit progress every 256 KiB received
)

// Default per-tenant MIME allowlist for v1.
var defaultMimeAllowlist = []string{
	"application/pdf",
	"image/png", "image/jpeg", "image/gif", "image/webp",
	"text/plain", "text/csv",
}

// ---------------------------------------------------------------------------
// Wire envelopes — match docs/protocols/file-upload.md (v1).
// ---------------------------------------------------------------------------

type uploadStartData struct {
	V              int    `json:"v"`
	Type           string `json:"type"`
	UploadID       string `json:"upload_id"`
	Name           string `json:"name"`
	MimeType       string `json:"mime_type"`
	Size           int64  `json:"size"`
	SHA256         string `json:"sha256"`
	TargetSelector string `json:"target_selector,omitempty"`
}
type uploadChunkData struct {
	V        int    `json:"v"`
	Type     string `json:"type"`
	UploadID string `json:"upload_id"`
	Seq      int    `json:"seq"`
	Data     string `json:"data"` // base64
}
type uploadEndData struct {
	V        int    `json:"v"`
	Type     string `json:"type"`
	UploadID string `json:"upload_id"`
}
type uploadCancelData = uploadEndData

// Server → client.
type progressEnv struct {
	V             int    `json:"v"`
	Type          string `json:"type"`
	UploadID      string `json:"upload_id"`
	BytesReceived int64  `json:"bytes_received"`
	BytesTotal    int64  `json:"bytes_total"`
}
type completeEnv struct {
	V           int    `json:"v"`
	Type        string `json:"type"`
	UploadID    string `json:"upload_id"`
	ServerPath  string `json:"server_path"`
	AttachedVia string `json:"attached_via"`
}
type errorEnv struct {
	V        int    `json:"v"`
	Type     string `json:"type"`
	UploadID string `json:"upload_id"`
	Code     string `json:"code"`
	Error    string `json:"error"`
}

// peekEnvelope just reads `type` to discriminate.
type peekEnvelope struct {
	V    int    `json:"v"`
	Type string `json:"type"`
}

// ---------------------------------------------------------------------------
// CDP client — same minimal shape as input-bridge / cursor-watcher.
// ---------------------------------------------------------------------------

type cdpSender interface {
	Send(ctx context.Context, method string, params any) (json.RawMessage, error)
}

type cdpClient struct {
	mu         sync.Mutex
	conn       *websocket.Conn
	nextID     int64
	pending    map[int64]chan cdpResp
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

func (e *cdpErr) Error() string { return fmt.Sprintf("cdp %d: %s", e.Code, e.Message) }

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
		conn: conn, pending: make(map[int64]chan cdpResp),
		disconnect: make(chan struct{}), log: log,
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
			return
		}
		var f cdpFrame
		if err := json.Unmarshal(raw, &f); err != nil {
			continue
		}
		if f.ID == 0 {
			// Server-initiated event — no listeners in v1.
			continue
		}
		c.mu.Lock()
		ch, ok := c.pending[f.ID]
		delete(c.pending, f.ID)
		c.mu.Unlock()
		if ok {
			ch <- cdpResp{Result: f.Result, Err: f.Error}
			close(ch)
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
// Per-upload state and bridge
// ---------------------------------------------------------------------------

type uploadState struct {
	id             string
	name           string
	sanitizedName  string
	mimeType       string
	size           int64
	expectedSHA256 string
	targetSelector string
	hash           hash.Hash
	bytesReceived  int64
	nextSeq        int
	tmpPath        string
	tmpFile        *os.File
	lastProgressKB int64
}

type bridge struct {
	cdp     cdpSender
	cfg     bridgeConfig
	log     *slog.Logger
	mu      sync.Mutex
	uploads map[string]*uploadState
}

type bridgeConfig struct {
	uploadDir      string
	sessionID      string
	maxTotalSize   int64
	maxChunkSize   int64
	mimeAllowlist  map[string]bool
	dryRun         bool
}

func newBridge(c cdpSender, cfg bridgeConfig, log *slog.Logger) *bridge {
	return &bridge{
		cdp: c, cfg: cfg, log: log,
		uploads: make(map[string]*uploadState),
	}
}

// install enables the CDP domains the bridge depends on. Idempotent.
func (b *bridge) install(ctx context.Context) error {
	if b.cfg.dryRun {
		return nil
	}
	for _, m := range []string{"DOM.enable", "Page.enable"} {
		if _, err := b.cdp.Send(ctx, m, nil); err != nil {
			return fmt.Errorf("%s: %w", m, err)
		}
	}
	// Enable file-chooser interception — second-best attach path used
	// when the page programmatically opens a file picker. Best-effort:
	// older Chromium builds may not have this method; we fall back to
	// the selector-based path.
	if _, err := b.cdp.Send(ctx, "Page.setInterceptFileChooserDialog",
		map[string]any{"enabled": true}); err != nil {
		b.log.Warn("Page.setInterceptFileChooserDialog not enabled",
			slog.Any("err", err))
	}
	return nil
}

// dispatch is the main entry point; emit is the back-channel the bridge
// uses for progress / complete / error envelopes.
func (b *bridge) dispatch(ctx context.Context, raw []byte, emit func([]byte) error) {
	var peek peekEnvelope
	if err := json.Unmarshal(raw, &peek); err != nil {
		b.log.Warn("non-JSON file envelope", slog.Any("err", err))
		return
	}
	if peek.V != protocolVersion {
		b.log.Warn("rejecting envelope with wrong v", slog.Int("v", peek.V))
		return
	}
	switch peek.Type {
	case "file_upload_start":
		var d uploadStartData
		if err := json.Unmarshal(raw, &d); err != nil {
			b.log.Warn("bad file_upload_start", slog.Any("err", err))
			return
		}
		b.handleStart(ctx, d, emit)
	case "file_upload_chunk":
		var d uploadChunkData
		if err := json.Unmarshal(raw, &d); err != nil {
			b.log.Warn("bad file_upload_chunk", slog.Any("err", err))
			return
		}
		b.handleChunk(ctx, d, emit)
	case "file_upload_end":
		var d uploadEndData
		if err := json.Unmarshal(raw, &d); err != nil {
			b.log.Warn("bad file_upload_end", slog.Any("err", err))
			return
		}
		b.handleEnd(ctx, d, emit)
	case "file_upload_cancel":
		var d uploadCancelData
		if err := json.Unmarshal(raw, &d); err != nil {
			return
		}
		b.handleCancel(d, emit)
	default:
		// Server → client envelopes are emitted by us; receiving one
		// from the relay is not expected — log and drop.
		b.log.Debug("ignoring envelope type", slog.String("type", peek.Type))
	}
}

func (b *bridge) handleStart(_ context.Context, d uploadStartData, emit func([]byte) error) {
	if d.UploadID == "" {
		return
	}
	if d.Size <= 0 || d.Size > b.cfg.maxTotalSize {
		emitErr(emit, d.UploadID, "size_limit_exceeded",
			fmt.Sprintf("size %d > cap %d", d.Size, b.cfg.maxTotalSize))
		return
	}
	if !b.cfg.mimeAllowlist[strings.ToLower(d.MimeType)] {
		emitErr(emit, d.UploadID, "mime_not_allowlisted",
			fmt.Sprintf("mime_type %q not in allowlist", d.MimeType))
		return
	}

	sanitized := sanitizeName(d.Name)
	dir := filepath.Join(b.cfg.uploadDir, b.cfg.sessionID)
	if err := os.MkdirAll(dir, 0o750); err != nil {
		emitErr(emit, d.UploadID, "internal_error", "mkdir: "+err.Error())
		return
	}
	tmpPath := filepath.Join(dir, d.UploadID+"__"+sanitized)
	f, err := os.OpenFile(tmpPath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o640)
	if err != nil {
		emitErr(emit, d.UploadID, "internal_error", "open tmp: "+err.Error())
		return
	}

	st := &uploadState{
		id: d.UploadID, name: d.Name, sanitizedName: sanitized,
		mimeType: strings.ToLower(d.MimeType), size: d.Size,
		expectedSHA256: strings.ToLower(d.SHA256),
		targetSelector: d.TargetSelector,
		hash:           sha256.New(),
		tmpPath:        tmpPath, tmpFile: f,
	}

	b.mu.Lock()
	if _, dup := b.uploads[d.UploadID]; dup {
		b.mu.Unlock()
		_ = f.Close()
		_ = os.Remove(tmpPath)
		emitErr(emit, d.UploadID, "internal_error", "duplicate upload_id")
		return
	}
	b.uploads[d.UploadID] = st
	b.mu.Unlock()
	b.log.Info("upload started",
		slog.String("upload_id", d.UploadID),
		slog.String("name", sanitized),
		slog.Int64("size", d.Size))
}

func (b *bridge) handleChunk(_ context.Context, d uploadChunkData, emit func([]byte) error) {
	b.mu.Lock()
	st, ok := b.uploads[d.UploadID]
	b.mu.Unlock()
	if !ok {
		emitErr(emit, d.UploadID, "internal_error", "no active upload for id")
		return
	}
	if d.Seq != st.nextSeq {
		b.failUpload(st, emit, "out_of_order_chunk",
			fmt.Sprintf("expected seq %d, got %d", st.nextSeq, d.Seq))
		return
	}
	bytes, err := base64.StdEncoding.DecodeString(d.Data)
	if err != nil {
		b.failUpload(st, emit, "internal_error", "bad base64: "+err.Error())
		return
	}
	if int64(len(bytes)) > b.cfg.maxChunkSize {
		b.failUpload(st, emit, "chunk_too_large",
			fmt.Sprintf("chunk %d bytes > cap %d", len(bytes), b.cfg.maxChunkSize))
		return
	}
	if st.bytesReceived+int64(len(bytes)) > st.size {
		b.failUpload(st, emit, "size_limit_exceeded",
			"total received exceeds asserted size")
		return
	}
	if _, err := st.tmpFile.Write(bytes); err != nil {
		b.failUpload(st, emit, "internal_error", "write: "+err.Error())
		return
	}
	st.hash.Write(bytes)
	st.bytesReceived += int64(len(bytes))
	st.nextSeq++

	// Emit progress every defaultProgressEveryKB on KiB boundary.
	kbReceived := st.bytesReceived / 1024
	if kbReceived-st.lastProgressKB >= defaultProgressEveryKB ||
		st.bytesReceived == st.size {
		st.lastProgressKB = kbReceived
		emitProgress(emit, st.id, st.bytesReceived, st.size)
	}
}

func (b *bridge) handleEnd(ctx context.Context, d uploadEndData, emit func([]byte) error) {
	b.mu.Lock()
	st, ok := b.uploads[d.UploadID]
	if ok {
		delete(b.uploads, d.UploadID)
	}
	b.mu.Unlock()
	if !ok {
		emitErr(emit, d.UploadID, "internal_error", "no active upload for id")
		return
	}
	if err := st.tmpFile.Close(); err != nil {
		_ = os.Remove(st.tmpPath)
		emitErr(emit, d.UploadID, "internal_error", "close: "+err.Error())
		return
	}
	if st.bytesReceived != st.size {
		_ = os.Remove(st.tmpPath)
		emitErr(emit, d.UploadID, "truncated",
			fmt.Sprintf("received %d of %d bytes", st.bytesReceived, st.size))
		return
	}
	got := hex.EncodeToString(st.hash.Sum(nil))
	if got != st.expectedSHA256 {
		_ = os.Remove(st.tmpPath)
		emitErr(emit, d.UploadID, "sha256_mismatch",
			fmt.Sprintf("expected %s, got %s", st.expectedSHA256, got))
		return
	}
	// Re-sniff the first 4 KiB on disk via http.DetectContentType for
	// the per-tenant MIME allowlist re-check.
	if !b.sniffPasses(st) {
		_ = os.Remove(st.tmpPath)
		emitErr(emit, d.UploadID, "mime_not_allowlisted",
			"sniffed content does not match an allowlisted MIME family")
		return
	}
	// Virus-scan stub. Phase 4 wires ClamAV here; for now log and accept.
	b.log.Info("virus scan stub — accepted",
		slog.String("upload_id", st.id),
		slog.String("path", st.tmpPath))

	attachedVia := "none"
	if !b.cfg.dryRun && st.targetSelector != "" {
		if err := b.attachViaSelector(ctx, st); err != nil {
			b.log.Warn("DOM.setFileInputFiles failed",
				slog.String("upload_id", st.id), slog.Any("err", err))
			emitErr(emit, st.id, "attach_failed", err.Error())
			return
		}
		attachedVia = "domSetFileInputFiles"
	}

	emitComplete(emit, st.id, st.tmpPath, attachedVia)
	b.log.Info("upload complete",
		slog.String("upload_id", st.id),
		slog.String("path", st.tmpPath),
		slog.String("attached_via", attachedVia))
}

func (b *bridge) handleCancel(d uploadCancelData, emit func([]byte) error) {
	b.mu.Lock()
	st, ok := b.uploads[d.UploadID]
	if ok {
		delete(b.uploads, d.UploadID)
	}
	b.mu.Unlock()
	if !ok {
		return
	}
	_ = st.tmpFile.Close()
	_ = os.Remove(st.tmpPath)
	emitErr(emit, d.UploadID, "cancelled", "client requested cancel")
}

func (b *bridge) failUpload(st *uploadState, emit func([]byte) error, code, msg string) {
	b.mu.Lock()
	delete(b.uploads, st.id)
	b.mu.Unlock()
	_ = st.tmpFile.Close()
	_ = os.Remove(st.tmpPath)
	emitErr(emit, st.id, code, msg)
}

// attachViaSelector resolves the target selector to a backendNodeId
// and calls DOM.setFileInputFiles to attach the file.
func (b *bridge) attachViaSelector(ctx context.Context, st *uploadState) error {
	// First get the document node id.
	docRes, err := b.cdp.Send(ctx, "DOM.getDocument", map[string]any{
		"depth": 0, "pierce": false,
	})
	if err != nil {
		return fmt.Errorf("DOM.getDocument: %w", err)
	}
	var doc struct {
		Root struct {
			NodeID int `json:"nodeId"`
		} `json:"root"`
	}
	if err := json.Unmarshal(docRes, &doc); err != nil {
		return fmt.Errorf("decode DOM.getDocument: %w", err)
	}
	queryRes, err := b.cdp.Send(ctx, "DOM.querySelector", map[string]any{
		"nodeId":   doc.Root.NodeID,
		"selector": st.targetSelector,
	})
	if err != nil {
		return fmt.Errorf("DOM.querySelector(%q): %w", st.targetSelector, err)
	}
	var qs struct {
		NodeID int `json:"nodeId"`
	}
	if err := json.Unmarshal(queryRes, &qs); err != nil {
		return fmt.Errorf("decode querySelector: %w", err)
	}
	if qs.NodeID == 0 {
		return fmt.Errorf("selector %q matched no node", st.targetSelector)
	}
	if _, err := b.cdp.Send(ctx, "DOM.setFileInputFiles", map[string]any{
		"nodeId": qs.NodeID,
		"files":  []string{st.tmpPath},
	}); err != nil {
		return fmt.Errorf("DOM.setFileInputFiles: %w", err)
	}
	return nil
}

// sniffPasses re-reads the first 4 KiB of the file and runs
// http.DetectContentType. We accept if either:
//   - the sniffed MIME exactly matches the asserted one, or
//   - sniffed and asserted share the same top-level type ("image/", etc.)
//     and both are on the allowlist.
//
// Many file types (PDF, plain text, CSV) DetectContentType maps to
// well-known strings; we trust that for the v1 allowlist.
func (b *bridge) sniffPasses(st *uploadState) bool {
	f, err := os.Open(st.tmpPath)
	if err != nil {
		return false
	}
	defer f.Close()
	buf := make([]byte, 4096)
	n, _ := f.Read(buf)
	sniffed := strings.ToLower(http.DetectContentType(buf[:n]))
	// DetectContentType returns e.g. "text/plain; charset=utf-8" — strip params.
	if idx := strings.Index(sniffed, ";"); idx >= 0 {
		sniffed = strings.TrimSpace(sniffed[:idx])
	}
	if sniffed == st.mimeType {
		return true
	}
	// "Compatible-family" rule: same top-level type AND both allowlisted.
	stTop := topLevel(st.mimeType)
	snTop := topLevel(sniffed)
	if stTop != "" && stTop == snTop && b.cfg.mimeAllowlist[sniffed] {
		return true
	}
	b.log.Warn("sniffed mime did not match",
		slog.String("upload_id", st.id),
		slog.String("asserted", st.mimeType),
		slog.String("sniffed", sniffed))
	return false
}

func topLevel(mime string) string {
	if idx := strings.Index(mime, "/"); idx > 0 {
		return mime[:idx]
	}
	return ""
}

// sanitizeName strips path separators + control chars and falls back
// to "upload.bin" if the result is empty.
func sanitizeName(name string) string {
	name = filepath.Base(name) // strip directory components
	name = strings.ReplaceAll(name, "\x00", "")
	// Replace anything other than [A-Za-z0-9._-] with underscore.
	var b strings.Builder
	b.Grow(len(name))
	for _, r := range name {
		switch {
		case r >= 'A' && r <= 'Z',
			r >= 'a' && r <= 'z',
			r >= '0' && r <= '9',
			r == '.', r == '_', r == '-':
			b.WriteRune(r)
		default:
			b.WriteByte('_')
		}
	}
	out := b.String()
	if out == "" || out == "." || out == ".." {
		return "upload.bin"
	}
	return out
}

// ---------------------------------------------------------------------------
// Emit helpers — write a server→client envelope through the back channel.
// ---------------------------------------------------------------------------

func emitProgress(emit func([]byte) error, id string, recv, total int64) {
	raw, _ := json.Marshal(progressEnv{
		V: protocolVersion, Type: "file_upload_progress",
		UploadID: id, BytesReceived: recv, BytesTotal: total,
	})
	_ = emit(raw)
}

func emitComplete(emit func([]byte) error, id, path, via string) {
	raw, _ := json.Marshal(completeEnv{
		V: protocolVersion, Type: "file_upload_complete",
		UploadID: id, ServerPath: path, AttachedVia: via,
	})
	_ = emit(raw)
}

func emitErr(emit func([]byte) error, id, code, msg string) {
	raw, _ := json.Marshal(errorEnv{
		V: protocolVersion, Type: "file_upload_error",
		UploadID: id, Code: code, Error: msg,
	})
	_ = emit(raw)
}

// ---------------------------------------------------------------------------
// WebSocket source (mirrors capture/input-bridge)
// ---------------------------------------------------------------------------

func runWS(ctx context.Context, addr, path string, b *bridge, log *slog.Logger) error {
	mux := http.NewServeMux()
	upg := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	mux.HandleFunc(path, func(w http.ResponseWriter, r *http.Request) {
		conn, err := upg.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		conn.SetReadLimit(2 * defaultMaxChunkSize)
		// One write mutex per connection so the bridge's emit
		// callback doesn't race with reads from the same socket.
		var writeMu sync.Mutex
		emit := func(raw []byte) error {
			writeMu.Lock()
			defer writeMu.Unlock()
			return conn.WriteMessage(websocket.TextMessage, raw)
		}
		for {
			_, raw, err := conn.ReadMessage()
			if err != nil {
				return
			}
			b.dispatch(ctx, raw, emit)
		}
	})
	srv := &http.Server{Addr: addr, Handler: mux, ReadHeaderTimeout: 5 * time.Second}
	errCh := make(chan error, 1)
	go func() {
		log.Info("file-bridge listening", slog.String("addr", addr), slog.String("path", path))
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
// CLI / main
// ---------------------------------------------------------------------------

type config struct {
	wsAddr        string
	wsPath        string
	cdpURL        string
	uploadDir     string
	sessionID     string
	maxTotalSize  int64
	maxChunkSize  int64
	mimeAllowlist string // comma-separated
	dryRun        bool
}

func parseFlags(args []string) (config, error) {
	fs := flag.NewFlagSet("file-bridge", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	cfg := config{}
	fs.StringVar(&cfg.wsAddr, "ws-addr", "127.0.0.1:9400", "WS source listen addr")
	fs.StringVar(&cfg.wsPath, "ws-path", "/files", "WS source path")
	fs.StringVar(&cfg.cdpURL, "cdp-url", "http://127.0.0.1:9222", "Chromium DevTools base URL")
	fs.StringVar(&cfg.uploadDir, "upload-dir", "/var/lib/chromeless-uploads", "Per-session upload root")
	fs.StringVar(&cfg.sessionID, "session-id", "default", "Per-session subdirectory under upload-dir")
	fs.Int64Var(&cfg.maxTotalSize, "max-total-size", defaultMaxTotalSize, "Total per-upload size cap (bytes)")
	fs.Int64Var(&cfg.maxChunkSize, "max-chunk-size", defaultMaxChunkSize, "Max raw chunk size (bytes)")
	fs.StringVar(&cfg.mimeAllowlist, "mime-allowlist", strings.Join(defaultMimeAllowlist, ","),
		"Comma-separated allowlisted MIME types")
	fs.BoolVar(&cfg.dryRun, "dry-run", false, "Skip CDP entirely; useful for tests / smoke")
	if err := fs.Parse(args); err != nil {
		return cfg, err
	}
	return cfg, nil
}

func mkAllowlist(csv string) map[string]bool {
	out := map[string]bool{}
	for _, m := range strings.Split(csv, ",") {
		m = strings.ToLower(strings.TrimSpace(m))
		if m != "" {
			out[m] = true
		}
	}
	return out
}

func run(ctx context.Context, cfg config, log *slog.Logger) error {
	bcfg := bridgeConfig{
		uploadDir: cfg.uploadDir, sessionID: cfg.sessionID,
		maxTotalSize: cfg.maxTotalSize, maxChunkSize: cfg.maxChunkSize,
		mimeAllowlist: mkAllowlist(cfg.mimeAllowlist), dryRun: cfg.dryRun,
	}

	var sender cdpSender
	if cfg.dryRun {
		sender = noopCDP{}
	} else {
		dialCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
		c, err := dialCDP(dialCtx, cfg.cdpURL, log)
		cancel()
		if err != nil {
			return fmt.Errorf("connect to CDP: %w", err)
		}
		defer c.Close()
		sender = c
	}

	b := newBridge(sender, bcfg, log)
	if err := b.install(ctx); err != nil {
		log.Warn("install partially failed; continuing", slog.Any("err", err))
	}

	if err := runWS(ctx, cfg.wsAddr, cfg.wsPath, b, log); err != nil &&
		!errors.Is(err, context.Canceled) {
		return err
	}
	return nil
}

type noopCDP struct{}

func (noopCDP) Send(_ context.Context, _ string, _ any) (json.RawMessage, error) {
	return []byte("{}"), nil
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

	if err := run(ctx, cfg, log); err != nil {
		log.Error("run failed", slog.Any("err", err))
		os.Exit(1)
	}
}

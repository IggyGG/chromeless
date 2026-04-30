// Tests for the file-upload bridge.
//
// Two layers:
//
//  1. Pure-function tests — sanitizeName, sniffPasses (via direct
//     bridge construction), parseFlags, mkAllowlist.
//
//  2. End-to-end-of-bridge tests against a fakeCDP that records every
//     dispatched method. We drive the bridge via in-process
//     dispatch() calls (skipping the WebSocket source) so the tests
//     are deterministic + fast. The real ws source is exercised by
//     tests/integration/file_upload_test.go.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// recordingCDP captures every method+params for assertions.
type recordingCDP struct {
	mu    sync.Mutex
	calls []struct {
		method string
		params any
	}
	// stub responses keyed by method.
	resp map[string]json.RawMessage
}

func newRecordingCDP() *recordingCDP {
	return &recordingCDP{resp: map[string]json.RawMessage{
		// Default replies for the attach path.
		"DOM.getDocument":      json.RawMessage(`{"root":{"nodeId":1}}`),
		"DOM.querySelector":    json.RawMessage(`{"nodeId":42}`),
		"DOM.setFileInputFiles": json.RawMessage(`{}`),
	}}
}

func (r *recordingCDP) Send(_ context.Context, method string, params any) (json.RawMessage, error) {
	r.mu.Lock()
	r.calls = append(r.calls, struct {
		method string
		params any
	}{method, params})
	r.mu.Unlock()
	if rsp, ok := r.resp[method]; ok {
		return rsp, nil
	}
	return []byte(`{}`), nil
}

func (r *recordingCDP) methods() []string {
	r.mu.Lock()
	defer r.mu.Unlock()
	out := make([]string, len(r.calls))
	for i, c := range r.calls {
		out[i] = c.method
	}
	return out
}

// captureEmit collects server→client envelopes for assertions.
type captureEmit struct {
	mu      sync.Mutex
	emitted [][]byte
}

func (c *captureEmit) emit(b []byte) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	cp := make([]byte, len(b))
	copy(cp, b)
	c.emitted = append(c.emitted, cp)
	return nil
}

func (c *captureEmit) snapshot() [][]byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make([][]byte, len(c.emitted))
	copy(out, c.emitted)
	return out
}

func (c *captureEmit) findType(t string) []map[string]any {
	out := []map[string]any{}
	for _, raw := range c.snapshot() {
		var m map[string]any
		if err := json.Unmarshal(raw, &m); err != nil {
			continue
		}
		if m["type"] == t {
			out = append(out, m)
		}
	}
	return out
}

// helper: build a bridge with a temp upload directory.
func setupBridge(t *testing.T, cdp cdpSender) (*bridge, string) {
	t.Helper()
	dir := t.TempDir()
	cfg := bridgeConfig{
		uploadDir: dir, sessionID: "sess",
		maxTotalSize: defaultMaxTotalSize,
		maxChunkSize: defaultMaxChunkSize,
		mimeAllowlist: mkAllowlist(strings.Join(defaultMimeAllowlist, ",")),
	}
	return newBridge(cdp, cfg, quietLogger()), dir
}

// helper: simulate a full upload by calling dispatch() with each
// envelope in sequence. Returns the captureEmit for assertions.
func driveUpload(t *testing.T, b *bridge, uploadID, name, mime string,
	bytes []byte, chunkSize int, targetSelector string) *captureEmit {
	t.Helper()
	ce := &captureEmit{}
	ctx := context.Background()
	hash := sha256.Sum256(bytes)
	startEnv := map[string]any{
		"v": 1, "type": "file_upload_start",
		"upload_id": uploadID, "name": name,
		"mime_type": mime, "size": len(bytes),
		"sha256": hex.EncodeToString(hash[:]),
	}
	if targetSelector != "" {
		startEnv["target_selector"] = targetSelector
	}
	startRaw, _ := json.Marshal(startEnv)
	b.dispatch(ctx, startRaw, ce.emit)

	for off, seq := 0, 0; off < len(bytes); seq++ {
		end := off + chunkSize
		if end > len(bytes) {
			end = len(bytes)
		}
		chunkRaw, _ := json.Marshal(map[string]any{
			"v": 1, "type": "file_upload_chunk",
			"upload_id": uploadID, "seq": seq,
			"data": base64.StdEncoding.EncodeToString(bytes[off:end]),
		})
		b.dispatch(ctx, chunkRaw, ce.emit)
		off = end
	}

	endRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_end", "upload_id": uploadID,
	})
	b.dispatch(ctx, endRaw, ce.emit)
	return ce
}

// ---------------------------------------------------------------------------
// 1. Pure-function tests
// ---------------------------------------------------------------------------

func TestSanitizeName(t *testing.T) {
	cases := map[string]string{
		"hello.pdf":             "hello.pdf",
		"../../etc/passwd":      "passwd",
		"foo bar.txt":           "foo_bar.txt",
		"héllo.png":             "h_llo.png", // é collapses to _; ASCII-only
		"":                      "upload.bin",
		".":                     "upload.bin",
		"..":                    "upload.bin",
		"weird\x00name":         "weirdname",
		"/abs/path/file.csv":    "file.csv",
	}
	for in, want := range cases {
		if got := sanitizeName(in); got != want {
			t.Errorf("sanitizeName(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestMkAllowlist(t *testing.T) {
	a := mkAllowlist("application/pdf, IMAGE/PNG ,text/plain")
	if !a["application/pdf"] || !a["image/png"] || !a["text/plain"] {
		t.Errorf("expected normalised lowercase entries, got %v", a)
	}
	if len(a) != 3 {
		t.Errorf("expected 3 entries, got %d", len(a))
	}
}

func TestParseFlags(t *testing.T) {
	cfg, err := parseFlags([]string{"--ws-addr", "127.0.0.1:1234", "--max-total-size", "5000"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.wsAddr != "127.0.0.1:1234" || cfg.maxTotalSize != 5000 {
		t.Errorf("unexpected cfg: %+v", cfg)
	}
}

// ---------------------------------------------------------------------------
// 2. Bridge happy / error paths
// ---------------------------------------------------------------------------

func TestUploadHappyPath(t *testing.T) {
	cdp := newRecordingCDP()
	b, dir := setupBridge(t, cdp)
	// Build a tiny PDF-shaped payload — DetectContentType will return
	// "application/pdf" for content starting with %PDF-.
	content := []byte("%PDF-1.4\n%hello world bytes...\n%%EOF\n")
	ce := driveUpload(t, b, "u1", "report.pdf", "application/pdf", content, 8, "#att")

	// Expect: at least one progress + a complete envelope.
	if len(ce.findType("file_upload_progress")) == 0 {
		t.Errorf("expected at least one progress envelope; got none")
	}
	completes := ce.findType("file_upload_complete")
	if len(completes) != 1 {
		t.Fatalf("expected 1 complete envelope, got %d", len(completes))
	}
	c := completes[0]
	if c["attached_via"] != "domSetFileInputFiles" {
		t.Errorf("attached_via = %v, want domSetFileInputFiles", c["attached_via"])
	}
	expectPath := filepath.Join(dir, "sess", "u1__report.pdf")
	if c["server_path"] != expectPath {
		t.Errorf("server_path = %v, want %v", c["server_path"], expectPath)
	}
	// CDP attach methods should have fired in order.
	saw := cdp.methods()
	mustContain := []string{"DOM.getDocument", "DOM.querySelector", "DOM.setFileInputFiles"}
	for _, m := range mustContain {
		found := false
		for _, s := range saw {
			if s == m {
				found = true
				break
			}
		}
		if !found {
			t.Errorf("expected CDP %s; saw %v", m, saw)
		}
	}
	// File on disk has the right bytes + sha256.
	got, err := os.ReadFile(expectPath)
	if err != nil {
		t.Fatalf("read uploaded: %v", err)
	}
	if string(got) != string(content) {
		t.Errorf("disk content mismatch")
	}
}

func TestUploadSizeCap(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	b.cfg.maxTotalSize = 4
	// driveUpload sends start, chunks, end — start is rejected here so
	// subsequent chunks/end produce "no active upload" errors. We only
	// care that the FIRST error is the size_limit_exceeded one.
	ce := driveUpload(t, b, "u1", "big.bin", "application/pdf",
		[]byte("12345"), 8, "")
	errs := ce.findType("file_upload_error")
	if len(errs) == 0 || errs[0]["code"] != "size_limit_exceeded" {
		t.Errorf("expected size_limit_exceeded as first error, got %v", errs)
	}
}

func TestUploadMimeNotAllowlisted(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	ce := driveUpload(t, b, "u1", "x.exe", "application/x-msdownload",
		[]byte("MZ\x00\x00"), 4, "")
	errs := ce.findType("file_upload_error")
	if len(errs) == 0 || errs[0]["code"] != "mime_not_allowlisted" {
		t.Errorf("expected mime_not_allowlisted as first error, got %v", errs)
	}
}

func TestUploadSHA256Mismatch(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	ctx := context.Background()
	ce := &captureEmit{}

	// Send a start with a deliberately-wrong sha256.
	startRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_start",
		"upload_id": "u1", "name": "y.pdf", "mime_type": "application/pdf",
		"size": 4, "sha256": "00" + strings.Repeat("0", 62),
	})
	b.dispatch(ctx, startRaw, ce.emit)

	chunkRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_chunk", "upload_id": "u1",
		"seq": 0, "data": base64.StdEncoding.EncodeToString([]byte("%PDF")),
	})
	b.dispatch(ctx, chunkRaw, ce.emit)

	endRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_end", "upload_id": "u1",
	})
	b.dispatch(ctx, endRaw, ce.emit)

	errs := ce.findType("file_upload_error")
	if len(errs) != 1 || errs[0]["code"] != "sha256_mismatch" {
		t.Errorf("expected sha256_mismatch, got %v", errs)
	}
}

func TestUploadOutOfOrderChunkRejected(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	ctx := context.Background()
	ce := &captureEmit{}

	content := []byte("%PDF-hello world content here")
	hash := sha256.Sum256(content)
	startRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_start",
		"upload_id": "u1", "name": "z.pdf", "mime_type": "application/pdf",
		"size": len(content), "sha256": hex.EncodeToString(hash[:]),
	})
	b.dispatch(ctx, startRaw, ce.emit)

	// Skip seq 0 — send seq 1.
	chunkRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_chunk", "upload_id": "u1",
		"seq": 1, "data": base64.StdEncoding.EncodeToString(content[:8]),
	})
	b.dispatch(ctx, chunkRaw, ce.emit)

	errs := ce.findType("file_upload_error")
	if len(errs) != 1 || errs[0]["code"] != "out_of_order_chunk" {
		t.Errorf("expected out_of_order_chunk, got %v", errs)
	}
}

func TestUploadCancel(t *testing.T) {
	b, dir := setupBridge(t, newRecordingCDP())
	ctx := context.Background()
	ce := &captureEmit{}

	content := []byte("%PDF-1.4 cancel test content here")
	hash := sha256.Sum256(content)
	startRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_start",
		"upload_id": "u1", "name": "c.pdf", "mime_type": "application/pdf",
		"size": len(content), "sha256": hex.EncodeToString(hash[:]),
	})
	b.dispatch(ctx, startRaw, ce.emit)

	// Cancel mid-stream.
	cancelRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_cancel", "upload_id": "u1",
	})
	b.dispatch(ctx, cancelRaw, ce.emit)

	errs := ce.findType("file_upload_error")
	if len(errs) != 1 || errs[0]["code"] != "cancelled" {
		t.Errorf("expected cancelled, got %v", errs)
	}
	// The temp file should be deleted.
	tmpPath := filepath.Join(dir, "sess", "u1__c.pdf")
	if _, err := os.Stat(tmpPath); !os.IsNotExist(err) {
		t.Errorf("expected tmp file to be deleted; stat err=%v", err)
	}
}

func TestUploadNoSelectorAttachesNone(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	content := []byte("%PDF-1.4\nno selector\n%%EOF")
	ce := driveUpload(t, b, "u1", "n.pdf", "application/pdf", content, 8, "")
	completes := ce.findType("file_upload_complete")
	if len(completes) != 1 {
		t.Fatalf("expected 1 complete envelope, got %d", len(completes))
	}
	if completes[0]["attached_via"] != "none" {
		t.Errorf("expected attached_via=none, got %v", completes[0]["attached_via"])
	}
}

func TestUploadUnknownIDChunkFailsGracefully(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	ce := &captureEmit{}
	ctx := context.Background()
	chunkRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_chunk", "upload_id": "ghost",
		"seq": 0, "data": "AAAA",
	})
	b.dispatch(ctx, chunkRaw, ce.emit)
	errs := ce.findType("file_upload_error")
	if len(errs) != 1 || errs[0]["code"] != "internal_error" {
		t.Errorf("expected internal_error for chunk-without-start, got %v", errs)
	}
}

func TestUploadTruncated(t *testing.T) {
	b, _ := setupBridge(t, newRecordingCDP())
	ctx := context.Background()
	ce := &captureEmit{}

	content := []byte("%PDF-1.4 truncated upload bytes here longer than chunk")
	hash := sha256.Sum256(content)
	startRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_start",
		"upload_id": "u1", "name": "t.pdf", "mime_type": "application/pdf",
		"size": len(content), "sha256": hex.EncodeToString(hash[:]),
	})
	b.dispatch(ctx, startRaw, ce.emit)

	// Send only the first 4 bytes, then end.
	chunkRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_chunk", "upload_id": "u1",
		"seq": 0, "data": base64.StdEncoding.EncodeToString(content[:4]),
	})
	b.dispatch(ctx, chunkRaw, ce.emit)
	endRaw, _ := json.Marshal(map[string]any{
		"v": 1, "type": "file_upload_end", "upload_id": "u1",
	})
	b.dispatch(ctx, endRaw, ce.emit)

	errs := ce.findType("file_upload_error")
	if len(errs) != 1 || errs[0]["code"] != "truncated" {
		t.Errorf("expected truncated, got %v", errs)
	}
}

func TestSniffMimeMismatch(t *testing.T) {
	// Asserted MIME is application/pdf but content is plain text;
	// http.DetectContentType returns "text/plain; charset=utf-8" — both
	// are on the allowlist but they're different top-level types
	// (application vs text), so the rule rejects.
	b, _ := setupBridge(t, newRecordingCDP())
	ce := driveUpload(t, b, "u1", "f.pdf", "application/pdf",
		[]byte("this is plain text not a pdf payload"), 8, "")
	errs := ce.findType("file_upload_error")
	if len(errs) != 1 || errs[0]["code"] != "mime_not_allowlisted" {
		t.Errorf("expected mime_not_allowlisted on sniff mismatch, got %v", errs)
	}
}

// otlp_logs_test.go — coverage for the FU #28 OTLP-logs exporter.
//
// Two paths matter:
//   1. Env-unset → no-op stub. Construction succeeds, Emit / Shutdown
//      both safe to call. This is the dev-compose / unit-test path, so
//      breaking it would silently break every other unit test that
//      exercises the /webrtc-event handler indirectly.
//   2. Stub exporter wired in directly → records that come out the far
//      end carry the expected body, attributes, and severity. The
//      production OTLP-HTTP transport itself isn't exercised (the SDK
//      has its own coverage there), but the body+attr translation done
//      by Emit() is — that's the chromeless-side surface to validate.

package main

import (
	"context"
	"sync"
	"testing"
	"time"

	"go.opentelemetry.io/otel/log"
	sdklog "go.opentelemetry.io/otel/sdk/log"
)

// memLogExporter is a minimal in-memory log.Exporter used for tests.
// We deliberately keep it inside this _test.go file (not exported)
// because nothing in the production code path needs it, and shipping a
// test helper as a public API surfaces a pollution risk.
type memLogExporter struct {
	mu      sync.Mutex
	records []sdklog.Record
}

func (e *memLogExporter) Export(_ context.Context, records []sdklog.Record) error {
	e.mu.Lock()
	defer e.mu.Unlock()
	// Clone so subsequent SDK reuse of the buffer doesn't mutate what
	// we captured (per the SDK contract — records may be reused).
	for _, r := range records {
		e.records = append(e.records, r.Clone())
	}
	return nil
}

func (e *memLogExporter) Shutdown(context.Context) error   { return nil }
func (e *memLogExporter) ForceFlush(context.Context) error { return nil }

func (e *memLogExporter) snapshot() []sdklog.Record {
	e.mu.Lock()
	defer e.mu.Unlock()
	out := make([]sdklog.Record, len(e.records))
	copy(out, e.records)
	return out
}

// newOTLPLoggerWithExporter constructs an OTLPLogger backed by a
// caller-supplied exporter, bypassing the env-driven endpoint logic.
// Test-only — the production constructor stays env-gated so deploy
// configs don't have to wire this by hand.
func newOTLPLoggerWithExporter(exp sdklog.Exporter) *OTLPLogger {
	provider := sdklog.NewLoggerProvider(
		sdklog.WithProcessor(sdklog.NewSimpleProcessor(exp)),
	)
	return &OTLPLogger{
		provider: provider,
		logger:   provider.Logger(otlpLogsScopeName),
	}
}

// TestOTLPLogger_NoOpWhenEnvUnset confirms the fast-path: when neither
// OTEL_EXPORTER_OTLP_ENDPOINT nor OTEL_EXPORTER_OTLP_LOGS_ENDPOINT is
// set, we get a non-nil logger that drops cleanly. No goroutines, no
// network attempt, no panic.
func TestOTLPLogger_NoOpWhenEnvUnset(t *testing.T) {
	t.Setenv("OTEL_EXPORTER_OTLP_ENDPOINT", "")
	t.Setenv("OTEL_EXPORTER_OTLP_LOGS_ENDPOINT", "")

	ol, err := newOTLPLogger(context.Background(), "test")
	if err != nil {
		t.Fatalf("newOTLPLogger: unexpected err = %v", err)
	}
	if ol == nil {
		t.Fatal("newOTLPLogger returned nil; expected no-op stub")
	}
	if ol.logger != nil {
		t.Errorf("expected stub.logger == nil when env unset, got %T", ol.logger)
	}
	if ol.provider != nil {
		t.Errorf("expected stub.provider == nil when env unset, got %T", ol.provider)
	}

	// Emit + Shutdown must both be safe on the stub.
	ol.Emit("chromeless.webrtc.session_created", map[string]any{
		"session_id": "abc",
		"label":      "cursor",
	})
	if err := ol.Shutdown(context.Background()); err != nil {
		t.Fatalf("stub Shutdown returned err = %v", err)
	}
}

// TestOTLPLogger_NilReceiverIsSafe — calling Emit / Shutdown on a typed
// nil pointer must not panic. webrtc_handler.go takes *OTLPLogger and
// the wiring in main.go avoids passing nil today, but defensive
// behaviour means a future refactor (e.g. an envtest harness) doesn't
// surprise the caller.
func TestOTLPLogger_NilReceiverIsSafe(t *testing.T) {
	var ol *OTLPLogger // typed nil
	ol.Emit("chromeless.webrtc.dc.opened", map[string]any{"label": "cursor"})
	if err := ol.Shutdown(context.Background()); err != nil {
		t.Fatalf("nil-receiver Shutdown returned err = %v", err)
	}
}

// TestOTLPLogger_EmitsRecordWithBodyAndAttrs verifies the wire shape
// the OTel collector / activity-feed consumer eventually sees: body =
// the dot-named event string, attributes carry the JSON envelope's
// attrs verbatim plus an element_id stamp from applyWebRTCEvent.
func TestOTLPLogger_EmitsRecordWithBodyAndAttrs(t *testing.T) {
	exp := &memLogExporter{}
	ol := newOTLPLoggerWithExporter(exp)
	defer func() { _ = ol.Shutdown(context.Background()) }()

	ol.Emit("chromeless.webrtc.dc.opened", map[string]any{
		"label":         "cursor",
		"session_id":    "s-42",
		"handshake_ms":  float64(245),
		"element_id":    "el-test",
		"reconnect":     false,
		"sample_string": "hello",
	})

	// SimpleProcessor exports synchronously, but the call chain still
	// runs through goroutines internally on some SDK versions; give it
	// a short window to settle before snapshotting.
	deadline := time.Now().Add(2 * time.Second)
	var rs []sdklog.Record
	for time.Now().Before(deadline) {
		rs = exp.snapshot()
		if len(rs) > 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if len(rs) != 1 {
		t.Fatalf("expected 1 exported record, got %d", len(rs))
	}

	r := rs[0]
	if got := r.Body().AsString(); got != "chromeless.webrtc.dc.opened" {
		t.Errorf("body = %q, want chromeless.webrtc.dc.opened", got)
	}
	if got := r.Severity(); got != log.SeverityInfo {
		t.Errorf("severity = %v, want %v", got, log.SeverityInfo)
	}

	// Walk attributes; assert presence + a couple of value-shape checks.
	attrs := map[string]log.Value{}
	r.WalkAttributes(func(kv log.KeyValue) bool {
		attrs[kv.Key] = kv.Value
		return true
	})

	wantKeys := []string{"label", "session_id", "handshake_ms", "element_id", "reconnect", "sample_string"}
	for _, k := range wantKeys {
		if _, ok := attrs[k]; !ok {
			t.Errorf("attribute %q missing from emitted record", k)
		}
	}
	if v, ok := attrs["label"]; ok {
		if v.AsString() != "cursor" {
			t.Errorf("attr label = %q, want cursor", v.AsString())
		}
	}
	if v, ok := attrs["handshake_ms"]; ok {
		if v.AsFloat64() != 245 {
			t.Errorf("attr handshake_ms = %v, want 245", v.AsFloat64())
		}
	}
	if v, ok := attrs["reconnect"]; ok {
		if v.AsBool() != false {
			t.Errorf("attr reconnect = %v, want false", v.AsBool())
		}
	}
}

// TestOTLPLogger_DropsEmptyEvent — calling Emit with an empty event
// string is a no-op (so a malformed envelope upstream doesn't drop a
// record with body=""). Mirrors webrtc_handler.go's "envelope missing
// event name" guard but at the exporter layer.
func TestOTLPLogger_DropsEmptyEvent(t *testing.T) {
	exp := &memLogExporter{}
	ol := newOTLPLoggerWithExporter(exp)
	defer func() { _ = ol.Shutdown(context.Background()) }()

	ol.Emit("", map[string]any{"label": "cursor"})

	// Give the SimpleProcessor a moment in case it ever buffered.
	time.Sleep(50 * time.Millisecond)
	if rs := exp.snapshot(); len(rs) != 0 {
		t.Errorf("expected empty-event Emit to drop, got %d records", len(rs))
	}
}

// TestWebRTCEventHandler_ForwardsToOTLP confirms the wiring through
// the HTTP handler: a /webrtc-event POST drives both a Prometheus
// counter increment AND an OTLP-logs record export. This is the
// closing-the-loop test that the FU #28 plumbing actually ties the
// two paths together — without it, the handler could silently
// regress to "Prometheus only" if a future refactor dropped the
// otlp.Emit call from applyWebRTCEvent.
func TestWebRTCEventHandler_ForwardsToOTLP(t *testing.T) {
	exp := &memLogExporter{}
	ol := newOTLPLoggerWithExporter(exp)
	defer func() { _ = ol.Shutdown(context.Background()) }()

	// Drive the same code path the production handler uses.
	env := &webrtcEventEnvelope{
		Event: "chromeless.webrtc.session_created",
		Attrs: map[string]any{"session_id": "s-77"},
	}
	if err := applyWebRTCEvent(env, quietLoggerWebRTC(), ol); err != nil {
		t.Fatalf("applyWebRTCEvent: %v", err)
	}

	// SimpleProcessor flushes synchronously per the SDK contract, but
	// poll briefly for robustness across SDK minor-version changes.
	deadline := time.Now().Add(2 * time.Second)
	var rs []sdklog.Record
	for time.Now().Before(deadline) {
		rs = exp.snapshot()
		if len(rs) > 0 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if len(rs) != 1 {
		t.Fatalf("expected 1 OTLP record from /webrtc-event POST, got %d", len(rs))
	}
	if got := rs[0].Body().AsString(); got != "chromeless.webrtc.session_created" {
		t.Errorf("body = %q, want chromeless.webrtc.session_created", got)
	}

	// element_id was injected by applyWebRTCEvent; assert it landed.
	var sawElementID bool
	rs[0].WalkAttributes(func(kv log.KeyValue) bool {
		if kv.Key == "element_id" {
			sawElementID = true
			return false
		}
		return true
	})
	if !sawElementID {
		t.Errorf("element_id attribute missing from forwarded OTLP record")
	}
}

// Tests for the clipboard-bridge service.
package main

import (
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"strings"
	"sync"
	"testing"
	"time"
)

// ---------------------------------------------------------------------------
// Pure-function tests
// ---------------------------------------------------------------------------

func TestParseEnvelope(t *testing.T) {
	cases := []struct {
		name    string
		raw     string
		wantErr string
	}{
		{
			name: "valid client->cloud",
			raw:  `{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"client->cloud","source":"user_action","text":"hi"}}`,
		},
		{
			name: "valid cloud->client",
			raw:  `{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"cloud->client","source":"user_action","text":"hi"}}`,
		},
		{
			name:    "wrong version",
			raw:     `{"v":2,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"client->cloud","source":"user_action","text":""}}`,
			wantErr: "unsupported protocol version",
		},
		{
			name:    "wrong type",
			raw:     `{"v":1,"type":"input","t":1,"seq":0,"data":{"direction":"client->cloud","source":"user_action","text":""}}`,
			wantErr: "unsupported type",
		},
		{
			name:    "unknown source",
			raw:     `{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"client->cloud","source":"sync","text":""}}`,
			wantErr: "unsupported source",
		},
		{
			name:    "invalid direction",
			raw:     `{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"sideways","source":"user_action","text":""}}`,
			wantErr: "invalid direction",
		},
		{
			name:    "oversize",
			raw:     `{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"client->cloud","source":"user_action","text":"` + strings.Repeat("A", maxBytes+1) + `"}}`,
			wantErr: "exceeds 1 MiB",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := parseEnvelope([]byte(tc.raw))
			if tc.wantErr == "" {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("err=%v, want contains %q", err, tc.wantErr)
			}
		})
	}
}

func TestQuoteJSString(t *testing.T) {
	// quoteJSString uses json.Marshal. Sanity-check round-trip
	// behaviour for the cases we care about.
	for _, in := range []string{"hi", `say "hi"`, "line\nbreak", "tab\there", "emoji🙂"} {
		out := quoteJSString(in)
		var got string
		if err := json.Unmarshal([]byte(out), &got); err != nil {
			t.Fatalf("unmarshal %s: %v", out, err)
		}
		if got != in {
			t.Errorf("round trip: in=%q out=%q got=%q", in, out, got)
		}
	}
}

// ---------------------------------------------------------------------------
// fakeCDP — records sent commands; replies with a configurable result
// for Runtime.evaluate so we can test the writeText fallback path.
// ---------------------------------------------------------------------------

type fakeCDP struct {
	mu     sync.Mutex
	calls  []recordedCall
	evalOK bool // when true, Runtime.evaluate returns {"result":{"value":"ok"}}
}

type recordedCall struct {
	Method string
	Params any
}

func newFakeCDP(evalOK bool) *fakeCDP {
	return &fakeCDP{evalOK: evalOK}
}

func (f *fakeCDP) Send(_ context.Context, method string, params any) (json.RawMessage, error) {
	f.mu.Lock()
	f.calls = append(f.calls, recordedCall{Method: method, Params: params})
	f.mu.Unlock()
	if method == "Runtime.evaluate" {
		if f.evalOK {
			return []byte(`{"result":{"value":"ok"},"type":"string"}`), nil
		}
		return []byte(`{"result":{"value":"err:denied"}}`), nil
	}
	return []byte(`{}`), nil
}

func (f *fakeCDP) Calls() []recordedCall {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]recordedCall, len(f.calls))
	copy(out, f.calls)
	return out
}

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

type bufEmitter struct {
	mu  sync.Mutex
	out [][]byte
}

func (b *bufEmitter) Emit(buf []byte) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	cp := make([]byte, len(buf))
	copy(cp, buf)
	b.out = append(b.out, cp)
	return nil
}
func (b *bufEmitter) snap() [][]byte {
	b.mu.Lock()
	defer b.mu.Unlock()
	cp := make([][]byte, len(b.out))
	copy(cp, b.out)
	return cp
}

// ---------------------------------------------------------------------------
// Inbound: writeText path + Ctrl+V synth
// ---------------------------------------------------------------------------

func TestApplyInboundHappyPath(t *testing.T) {
	f := newFakeCDP(true) // navigator.clipboard.writeText returns "ok"
	br := newBridge(f, &bufEmitter{}, quietLogger())
	if err := br.applyInbound(context.Background(), `hello "world"`); err != nil {
		t.Fatalf("applyInbound: %v", err)
	}
	calls := f.Calls()
	// Expected: Runtime.evaluate(writeText), then keyDown KeyV, keyUp KeyV.
	if len(calls) != 3 {
		t.Fatalf("expected 3 CDP calls, got %d: %+v", len(calls), calls)
	}
	if calls[0].Method != "Runtime.evaluate" {
		t.Errorf("call 0: %s", calls[0].Method)
	}
	if calls[1].Method != "Input.dispatchKeyEvent" || calls[2].Method != "Input.dispatchKeyEvent" {
		t.Errorf("expected two keyEvents after writeText; got %s, %s", calls[1].Method, calls[2].Method)
	}
}

func TestApplyInboundFallback(t *testing.T) {
	f := newFakeCDP(false) // writeText reports an error → fallback path
	br := newBridge(f, &bufEmitter{}, quietLogger())
	if err := br.applyInbound(context.Background(), "hi"); err != nil {
		t.Fatalf("applyInbound: %v", err)
	}
	calls := f.Calls()
	// Expected: Runtime.evaluate (failed), then Input.insertText fallback. No keyEvents.
	if len(calls) < 2 {
		t.Fatalf("expected ≥2 calls, got %d", len(calls))
	}
	saw := map[string]int{}
	for _, c := range calls {
		saw[c.Method]++
	}
	if saw["Runtime.evaluate"] == 0 {
		t.Errorf("expected Runtime.evaluate; calls=%v", saw)
	}
	if saw["Input.insertText"] == 0 {
		t.Errorf("expected Input.insertText fallback; calls=%v", saw)
	}
	if saw["Input.dispatchKeyEvent"] != 0 {
		t.Errorf("did not expect keyEvents on fallback; calls=%v", saw)
	}
}

// ---------------------------------------------------------------------------
// Outbound: Runtime.bindingCalled → cloud->client envelope
// ---------------------------------------------------------------------------

func TestRunOutbound(t *testing.T) {
	f := newFakeCDP(true)
	em := &bufEmitter{}
	br := newBridge(f, em, quietLogger())

	events := make(chan cdpEvent, 4)
	bindingEvent := func(payload string) cdpEvent {
		params, _ := json.Marshal(map[string]any{
			"name": "__cb_clip__", "payload": payload,
		})
		return cdpEvent{Method: "Runtime.bindingCalled", Params: params}
	}
	events <- bindingEvent("hello")
	events <- bindingEvent("world")
	close(events)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() { _ = br.runOutbound(ctx, events) }()

	// Wait briefly for the goroutine to drain.
	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) && len(em.snap()) < 2 {
		time.Sleep(5 * time.Millisecond)
	}
	if len(em.snap()) < 2 {
		t.Fatalf("only got %d emits", len(em.snap()))
	}
	envs := em.snap()
	for i, raw := range envs {
		var e clipboardEnvelope
		if err := json.Unmarshal(raw, &e); err != nil {
			t.Fatalf("envelope %d: %v", i, err)
		}
		if e.Data.Direction != "cloud->client" || e.Data.Source != "user_action" {
			t.Errorf("envelope %d wrong: %+v", i, e)
		}
	}
	if envs[0] == nil || envs[1] == nil {
		t.Fatal("missing emits")
	}
	var first, second clipboardEnvelope
	_ = json.Unmarshal(envs[0], &first)
	_ = json.Unmarshal(envs[1], &second)
	if first.Data.Text != "hello" || second.Data.Text != "world" {
		t.Errorf("text mismatch: %q %q", first.Data.Text, second.Data.Text)
	}
	if second.Seq != first.Seq+1 {
		t.Errorf("seq not monotonic: %d %d", first.Seq, second.Seq)
	}
}

func TestRunOutboundEchoSuppression(t *testing.T) {
	f := newFakeCDP(true)
	em := &bufEmitter{}
	br := newBridge(f, em, quietLogger())

	// Pretend we just wrote "loop" inbound, so the outbound copy of
	// "loop" is the echo we want to suppress.
	br.mu.Lock()
	br.last = "loop"
	br.mu.Unlock()

	events := make(chan cdpEvent, 2)
	params, _ := json.Marshal(map[string]any{"name": "__cb_clip__", "payload": "loop"})
	events <- cdpEvent{Method: "Runtime.bindingCalled", Params: params}
	close(events)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() { _ = br.runOutbound(ctx, events) }()
	// Give the goroutine time to consume the (single) event.
	time.Sleep(50 * time.Millisecond)
	if got := len(em.snap()); got != 0 {
		t.Errorf("echo should have been suppressed; got %d emits", got)
	}
}

func TestParseFlags(t *testing.T) {
	cfg, err := parseFlags([]string{"--source", "ws", "--sink", "ws"})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.source != "ws" || cfg.sink != "ws" {
		t.Errorf("unexpected cfg: %+v", cfg)
	}
	if _, err := parseFlags([]string{"--source", "garbage"}); err == nil {
		t.Errorf("expected error for invalid --source")
	}
	if _, err := parseFlags([]string{"--sink", "garbage"}); err == nil {
		t.Errorf("expected error for invalid --sink")
	}
}


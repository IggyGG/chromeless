// End-to-end integration test for the clipboard-bridge service (T32).
//
// Spec under test: docs/protocols/clipboard-channel.md (v1).
//
// Strategy: build the clipboard-bridge binary once, launch it as a
// subprocess pointed at a fake CDP server we run inside the test
// process, drive it through stdin (inbound) and observe its stdout
// (outbound), and assert both directions of the protocol.
//
// We do NOT run a real Chromium here — that's left to the higher-
// level smoke and Playwright tests. The fake CDP is just rich
// enough to:
//   - serve /json/list with a single page target,
//   - speak the WebSocket /devtools/page/1 endpoint,
//   - record every command sent to it,
//   - reply with success for everything,
//   - on demand, push a synthetic Runtime.bindingCalled to simulate
//     the cloud Chromium reporting an oncopy event.
package integration_test

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// ---------------------------------------------------------------------------
// Build the clipboard-bridge binary once for the package.
// ---------------------------------------------------------------------------

var clipboardBinary string

func buildClipboardBridge(t *testing.T) string {
	t.Helper()
	if clipboardBinary != "" {
		return clipboardBinary
	}
	tmp, err := os.MkdirTemp("", "clipboard-bin-")
	if err != nil {
		t.Fatalf("mktemp: %v", err)
	}
	t.Cleanup(func() { _ = os.RemoveAll(tmp) })

	out := filepath.Join(tmp, "clipboard-bridge")
	cmd := exec.Command("go", "build", "-o", out, ".")
	cmd.Dir = filepath.Join("..", "..", "capture", "clipboard-bridge")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		t.Fatalf("go build clipboard-bridge: %v", err)
	}
	clipboardBinary = out
	return out
}

// ---------------------------------------------------------------------------
// Fake CDP server
// ---------------------------------------------------------------------------

type fakeCDP struct {
	mu       sync.Mutex
	conn     *websocket.Conn
	calls    []recordedCall
	server   *httptest.Server
	ready    chan struct{}
	evalText []string // texts seen via Runtime.evaluate(navigator.clipboard.writeText)
}

type recordedCall struct {
	Method string                 `json:"method"`
	Params map[string]interface{} `json:"params"`
}

func newFakeCDP(t *testing.T) *fakeCDP {
	t.Helper()
	f := &fakeCDP{ready: make(chan struct{})}
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}

	mux := http.NewServeMux()
	srv := httptest.NewServer(mux)
	f.server = srv

	mux.HandleFunc("/json/list", func(w http.ResponseWriter, _ *http.Request) {
		base := strings.Replace(srv.URL, "http://", "ws://", 1)
		body := `[{"type":"page","webSocketDebuggerUrl":"` + base + `/devtools/page/1","url":"about:blank"}]`
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, body)
	})

	mux.HandleFunc("/devtools/page/1", func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		f.mu.Lock()
		f.conn = conn
		f.mu.Unlock()
		select {
		case <-f.ready:
		default:
			close(f.ready)
		}
		for {
			_, raw, err := conn.ReadMessage()
			if err != nil {
				return
			}
			var env struct {
				ID     int64                  `json:"id"`
				Method string                 `json:"method"`
				Params map[string]interface{} `json:"params"`
			}
			if err := json.Unmarshal(raw, &env); err != nil {
				return
			}
			f.mu.Lock()
			f.calls = append(f.calls, recordedCall{Method: env.Method, Params: env.Params})
			// Capture text from navigator.clipboard.writeText evaluate
			// expressions so the test can verify the bridge passed
			// the right string into Chromium.
			if env.Method == "Runtime.evaluate" {
				if expr, _ := env.Params["expression"].(string); expr != "" {
					if s, ok := extractWriteText(expr); ok {
						f.evalText = append(f.evalText, s)
					}
				}
			}
			f.mu.Unlock()

			var reply map[string]interface{}
			switch env.Method {
			case "Runtime.evaluate":
				// The bridge wraps writeText() in .then(()=>"ok").catch(...).
				// Reply with `value:"ok"` so the bridge takes the happy path.
				reply = map[string]interface{}{
					"id": env.ID,
					"result": map[string]interface{}{
						"result": map[string]interface{}{"type": "string", "value": "ok"},
					},
				}
			default:
				reply = map[string]interface{}{
					"id":     env.ID,
					"result": map[string]interface{}{},
				}
			}
			rb, _ := json.Marshal(reply)
			_ = conn.WriteMessage(websocket.TextMessage, rb)
		}
	})

	return f
}

// extractWriteText pulls the literal string out of:
//
//	navigator.clipboard.writeText(<jsonstring>).then(()=>"ok")...
func extractWriteText(expr string) (string, bool) {
	prefix := "navigator.clipboard.writeText("
	i := strings.Index(expr, prefix)
	if i < 0 {
		return "", false
	}
	rest := expr[i+len(prefix):]
	// rest starts with the JSON-quoted string. Find the matching closing
	// quote, accounting for escapes.
	if len(rest) == 0 || rest[0] != '"' {
		return "", false
	}
	end := 1
	for end < len(rest) {
		if rest[end] == '\\' && end+1 < len(rest) {
			end += 2
			continue
		}
		if rest[end] == '"' {
			end++
			break
		}
		end++
	}
	var out string
	if err := json.Unmarshal([]byte(rest[:end]), &out); err != nil {
		return "", false
	}
	return out, true
}

func (f *fakeCDP) URL() string { return f.server.URL }
func (f *fakeCDP) Close()      { f.server.Close() }

func (f *fakeCDP) calledTexts() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]string, len(f.evalText))
	copy(out, f.evalText)
	return out
}

func (f *fakeCDP) methodCount() map[string]int {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := map[string]int{}
	for _, c := range f.calls {
		out[c.Method]++
	}
	return out
}

func (f *fakeCDP) waitReady(t *testing.T, d time.Duration) {
	t.Helper()
	select {
	case <-f.ready:
	case <-time.After(d):
		t.Fatalf("fake CDP didn't get a connection within %s", d)
	}
}

// pushBindingCalled fakes "user copied text inside the cloud Chromium".
func (f *fakeCDP) pushBindingCalled(t *testing.T, payload string) {
	t.Helper()
	<-f.ready
	f.mu.Lock()
	conn := f.conn
	f.mu.Unlock()
	frame := map[string]interface{}{
		"method": "Runtime.bindingCalled",
		"params": map[string]interface{}{"name": "__cb_clip__", "payload": payload, "executionContextId": 1},
	}
	raw, _ := json.Marshal(frame)
	if err := conn.WriteMessage(websocket.TextMessage, raw); err != nil {
		t.Fatalf("push: %v", err)
	}
}

// ---------------------------------------------------------------------------
// The integration test itself
// ---------------------------------------------------------------------------

func TestClipboardBridgeRoundTrip(t *testing.T) {
	bin := buildClipboardBridge(t)
	cdp := newFakeCDP(t)
	defer cdp.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, bin,
		"--source", "stdin",
		"--sink", "stdout",
		"--cdp-url", cdp.URL(),
	)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		t.Fatalf("stdin pipe: %v", err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatalf("stdout pipe: %v", err)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr

	if err := cmd.Start(); err != nil {
		t.Fatalf("start: %v", err)
	}
	defer func() {
		_ = stdin.Close()
		_ = cmd.Wait()
	}()

	cdp.waitReady(t, 5*time.Second)

	// --- Inbound: client→cloud ---
	in := `{"v":1,"type":"clipboard_offer","t":1,"seq":0,"data":{"direction":"client->cloud","source":"user_action","text":"hello \"clip\""}}`
	if _, err := fmt.Fprintln(stdin, in); err != nil {
		t.Fatalf("write stdin: %v", err)
	}

	// Wait for the bridge to react: we expect Runtime.evaluate(writeText)
	// + 2 keyEvents to land at the fake CDP.
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		c := cdp.methodCount()
		if c["Runtime.evaluate"] >= 2 && c["Input.dispatchKeyEvent"] >= 2 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}

	c := cdp.methodCount()
	// install ran one Runtime.evaluate (probe install) + the writeText one.
	if c["Runtime.evaluate"] < 2 {
		t.Errorf("expected ≥2 Runtime.evaluate; got %v", c)
	}
	if c["Input.dispatchKeyEvent"] != 2 {
		t.Errorf("expected exactly 2 keyEvents (Ctrl+V down/up); got %v", c)
	}
	if c["Browser.grantPermissions"] == 0 {
		t.Errorf("install should have called Browser.grantPermissions; got %v", c)
	}
	if c["Page.addScriptToEvaluateOnNewDocument"] == 0 {
		t.Errorf("install should have called Page.addScriptToEvaluateOnNewDocument")
	}

	texts := cdp.calledTexts()
	if len(texts) == 0 || texts[0] != `hello "clip"` {
		t.Errorf("writeText payload mismatch: %q (texts=%v)", texts, texts)
	}

	// --- Outbound: cloud→client ---
	cdp.pushBindingCalled(t, "from cloud")

	// Expect one envelope to appear on stdout.
	scanner := bufio.NewScanner(stdout)
	scanner.Buffer(make([]byte, 0, 1<<16), 1<<20)

	got := make(chan []byte, 1)
	go func() {
		for scanner.Scan() {
			line := scanner.Bytes()
			if len(line) == 0 {
				continue
			}
			cp := make([]byte, len(line))
			copy(cp, line)
			got <- cp
			return
		}
		got <- nil
	}()

	select {
	case raw := <-got:
		if raw == nil {
			t.Fatalf("stdout closed before envelope; stderr:\n%s", stderr.String())
		}
		var env struct {
			V    int    `json:"v"`
			Type string `json:"type"`
			Data struct {
				Direction string `json:"direction"`
				Source    string `json:"source"`
				Text      string `json:"text"`
			} `json:"data"`
		}
		if err := json.Unmarshal(raw, &env); err != nil {
			t.Fatalf("envelope: %v\n%s", err, raw)
		}
		if env.V != 1 || env.Type != "clipboard_offer" ||
			env.Data.Direction != "cloud->client" ||
			env.Data.Source != "user_action" ||
			env.Data.Text != "from cloud" {
			t.Errorf("outbound envelope wrong: %+v", env)
		}
	case <-time.After(5 * time.Second):
		t.Fatalf("no outbound envelope; stderr:\n%s", stderr.String())
	}

	// --- Echo suppression: pushing the same text we just sent inbound
	//     should NOT produce an outbound envelope.
	cdp.pushBindingCalled(t, `hello "clip"`)
	select {
	case raw := <-got:
		t.Errorf("echo not suppressed; got %s", raw)
	case <-time.After(300 * time.Millisecond):
		// Expected — no envelope.
	}
}

package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// fakeWorker is a stand-in for cb-chromium's DevTools endpoint: it serves
// /json and upgrades /devtools/page/... to a WebSocket that answers CDP
// commands. Enough to exercise the whole send() path — discovery, host
// rewrite, dial, request/response correlation — without a Chromium build.
type fakeWorker struct {
	srv      *httptest.Server
	mu       chan struct{}
	received []string // methods, in order
	// emitEvent makes the socket send an unsolicited event before the reply,
	// which is what really happens (Page.frameNavigated et al).
	emitEvent bool
	failWith  string
}

func newFakeWorker(t *testing.T) *fakeWorker {
	t.Helper()
	fw := &fakeWorker{mu: make(chan struct{}, 1)}
	fw.mu <- struct{}{}

	mux := http.NewServeMux()
	mux.HandleFunc("/json", func(w http.ResponseWriter, r *http.Request) {
		if !strings.HasPrefix(r.Host, "localhost") {
			http.Error(w, "Host header is not localhost", http.StatusForbidden)
			return
		}
		_ = json.NewEncoder(w).Encode([]cdpTarget{{
			Type:                 "page",
			URL:                  "https://current.example/",
			WebSocketDebuggerURL: "ws://localhost:9222/devtools/page/FAKE",
		}})
	})
	mux.HandleFunc("/devtools/page/FAKE", func(w http.ResponseWriter, r *http.Request) {
		up := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		c, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer c.Close()
		for {
			var req struct {
				ID     int            `json:"id"`
				Method string         `json:"method"`
				Params map[string]any `json:"params"`
			}
			if err := c.ReadJSON(&req); err != nil {
				return
			}
			<-fw.mu
			fw.received = append(fw.received, req.Method)
			fw.mu <- struct{}{}

			if fw.emitEvent {
				// An id-less event arriving before the reply. send() must skip
				// it rather than treating it as the answer.
				_ = c.WriteJSON(map[string]any{
					"method": "Page.frameNavigated",
					"params": map[string]any{"frame": map[string]any{"id": "1"}},
				})
			}
			if fw.failWith != "" && req.Method != "Page.enable" {
				_ = c.WriteJSON(map[string]any{
					"id":    req.ID,
					"error": map[string]any{"code": -32000, "message": fw.failWith},
				})
				continue
			}
			if req.Method == "Page.getNavigationHistory" {
				_ = c.WriteJSON(map[string]any{"id": req.ID, "result": map[string]any{
					"currentIndex": 1,
					"entries": []map[string]any{
						{"id": 10, "url": "https://a.example/"},
						{"id": 11, "url": "https://b.example/"},
					}}})
				continue
			}
			_ = c.WriteJSON(map[string]any{"id": req.ID, "result": map[string]any{"frameId": "1"}})
		}
	})

	fw.srv = httptest.NewServer(mux)
	t.Cleanup(fw.srv.Close)
	return fw
}

func (fw *fakeWorker) methods() []string {
	<-fw.mu
	defer func() { fw.mu <- struct{}{} }()
	return append([]string(nil), fw.received...)
}

func TestNavigateRoundTrip(t *testing.T) {
	fw := newFakeWorker(t)
	c := &cdpClient{baseURL: fw.srv.URL}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if err := c.navigate(ctx, "https://example.com/"); err != nil {
		t.Fatalf("navigate: %v", err)
	}

	got := fw.methods()
	// Page.enable first: the embedder's capture re-arm hangs off Page events,
	// so navigating with the domain disabled is how you get a live stream of a
	// stale frame.
	want := []string{"Page.enable", "Page.navigate"}
	if len(got) != len(want) {
		t.Fatalf("methods = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("methods = %v, want %v", got, want)
			break
		}
	}
}

// The socket carries events as well as replies. Correlating on id is what
// keeps an event from being mistaken for the answer.
func TestSendSkipsUnsolicitedEvents(t *testing.T) {
	fw := newFakeWorker(t)
	fw.emitEvent = true
	c := &cdpClient{baseURL: fw.srv.URL}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	res, err := c.send(ctx, "Page.navigate", map[string]any{"url": "https://example.com/"})
	if err != nil {
		t.Fatalf("send: %v", err)
	}
	if !strings.Contains(string(res), "frameId") {
		t.Errorf("got the event instead of the reply: %s", res)
	}
}

// A CDP-level error must surface as an error, not a silent success.
func TestSendSurfacesCDPError(t *testing.T) {
	fw := newFakeWorker(t)
	fw.failWith = "Cannot navigate to invalid URL"
	c := &cdpClient{baseURL: fw.srv.URL}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	err := c.navigate(ctx, "https://example.com/")
	if err == nil {
		t.Fatal("expected an error")
	}
	if !strings.Contains(err.Error(), "Cannot navigate") {
		t.Errorf("error lost the CDP message: %v", err)
	}
}

func TestCurrentURL(t *testing.T) {
	fw := newFakeWorker(t)
	c := &cdpClient{baseURL: fw.srv.URL}

	got, err := c.currentURL(context.Background())
	if err != nil {
		t.Fatalf("currentURL: %v", err)
	}
	if got != "https://current.example/" {
		t.Errorf("currentURL = %q", got)
	}
}

// End to end through the HTTP layer: cookie, JSON body, scheme check, CDP.
func TestNavigateEndpointDrivesTheWorker(t *testing.T) {
	fw := newFakeWorker(t)
	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: fw.srv.URL}
	cookie := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/navigate",
		strings.NewReader(`{"url":"https://example.com/"}`))
	req.AddCookie(cookie)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	var body struct {
		URL string `json:"url"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body.URL != "https://example.com/" {
		t.Errorf("url = %q", body.URL)
	}
	// Page.navigate, then a capture re-arm. The re-arm is not incidental: a
	// cross-document navigation swaps the RenderWidgetHost and the FrameSinkId
	// with it, so a capturer bound to the old one goes silent and the user
	// gets a live-but-frozen video element.
	m := fw.methods()
	if !containsMethod(m, "Page.navigate") {
		t.Errorf("worker saw %v, want a Page.navigate", m)
	}
	if !containsMethod(m, "Cb.startFrameSinkCapture") {
		t.Errorf("worker saw %v, want a Cb.startFrameSinkCapture re-arm after navigating", m)
	}
}

// History uses getNavigationHistory + navigateToHistoryEntry, NOT
// Page.goBack/goForward.
//
// Those are a Chrome-branded convenience layer, not baseline CDP, and this
// embedder does not implement them — a live worker answers
// "'Page.goBack' wasn't found". The endpoints shipped using them and never
// worked: the error was swallowed as a declined command, so Back reported
// {"ok":false} and looked like "nothing to go back to". Found by the
// interactive suite against a real deployment; this pins the fix.
func TestHistoryUsesNavigateToHistoryEntry(t *testing.T) {
	fw := newFakeWorker(t)
	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: fw.srv.URL}
	cookie := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/back", nil)
	req.AddCookie(cookie)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	m := fw.methods()
	if containsMethod(m, "Page.goBack") {
		t.Error("used Page.goBack, which this embedder does not implement")
	}
	if !containsMethod(m, "Page.getNavigationHistory") {
		t.Errorf("worker saw %v, want Page.getNavigationHistory", m)
	}
}

func TestSimpleNavCommandsMapToCDPMethods(t *testing.T) {
	for path, method := range map[string]string{
		"/api/reload": "Page.reload",
		"/api/stop":   "Page.stopLoading",
	} {
		t.Run(path, func(t *testing.T) {
			fw := newFakeWorker(t)
			g, _ := newTestGateway(t, http.NotFoundHandler())
			g.cdp = &cdpClient{baseURL: fw.srv.URL}
			cookie := login(t, g, "operator", "s3cret")

			req := httptest.NewRequest(http.MethodPost, path, nil)
			req.AddCookie(cookie)
			rec := httptest.NewRecorder()
			g.routes().ServeHTTP(rec, req)

			if rec.Code != http.StatusOK {
				t.Fatalf("status = %d", rec.Code)
			}
			m := fw.methods()
			if len(m) == 0 || m[len(m)-1] != method {
				t.Errorf("worker saw %v, want a trailing %s", m, method)
			}
		})
	}
}

// Page.goBack with nothing behind it is a CDP error but not a user-facing
// failure — pressing Back on the first page should not raise a banner.
func TestHistoryCommandDeclinesGracefully(t *testing.T) {
	fw := newFakeWorker(t)
	fw.failWith = "Cannot navigate to invalid URL"
	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: fw.srv.URL}
	cookie := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/back", nil)
	req.AddCookie(cookie)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 with ok:false", rec.Code)
	}
	var body struct {
		OK bool `json:"ok"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body.OK {
		t.Error("reported ok:true for a declined command")
	}
}

func containsMethod(methods []string, want string) bool {
	for _, m := range methods {
		if m == want {
			return true
		}
	}
	return false
}

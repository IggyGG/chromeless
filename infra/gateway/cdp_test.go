package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// This is the security boundary between a logged-in caller and the browser
// process, so each rejected scheme is listed for a concrete reason.
func TestValidateNavigationURL(t *testing.T) {
	t.Run("accepts http and https", func(t *testing.T) {
		for _, in := range []string{
			"https://example.com",
			"http://example.com/path?q=1#frag",
			"https://example.com:8443/",
			"  https://example.com  ", // trimmed
		} {
			if _, err := validateNavigationURL(in); err != nil {
				t.Errorf("rejected %q: %v", in, err)
			}
		}
	})

	t.Run("rejects everything else", func(t *testing.T) {
		// file:  reads the worker's filesystem into the video stream —
		//        /etc/passwd, mounted secrets, the gateway's own TLS key.
		// javascript: executes in whatever page is loaded.
		// chrome:/devtools: privileged surfaces well beyond "browse the web".
		// data:  renders attacker-controlled markup.
		for _, in := range []string{
			"file:///etc/passwd",
			"file:///data/certs/key.pem",
			"javascript:alert(1)",
			"chrome://net-internals",
			"devtools://devtools/bundled/inspector.html",
			"data:text/html,<script>alert(1)</script>",
			"about:blank",
			"ftp://example.com",
			"", "   ",
		} {
			if _, err := validateNavigationURL(in); err == nil {
				t.Errorf("ACCEPTED %q — this is a way out of the sandbox", in)
			}
		}
	})

	// Scheme comparison must be case-insensitive in both directions: an
	// allowlist that misses "HTTPS" is broken, and one that misses "FILE:" is
	// dangerous.
	t.Run("scheme match is case-insensitive", func(t *testing.T) {
		if _, err := validateNavigationURL("HTTPS://example.com"); err != nil {
			t.Errorf("rejected uppercase https: %v", err)
		}
		for _, in := range []string{"FILE:///etc/passwd", "JavaScript:alert(1)"} {
			if _, err := validateNavigationURL(in); err == nil {
				t.Errorf("ACCEPTED %q — case-varied scheme slipped past the allowlist", in)
			}
		}
	})

	t.Run("requires a host", func(t *testing.T) {
		for _, in := range []string{"http://", "https:///path"} {
			if _, err := validateNavigationURL(in); err == nil {
				t.Errorf("accepted hostless %q", in)
			}
		}
	})

	t.Run("bounds the length", func(t *testing.T) {
		if _, err := validateNavigationURL("https://e.com/" + strings.Repeat("a", 5000)); err == nil {
			t.Error("accepted an over-long URL")
		}
	})
}

// The advertised webSocketDebuggerUrl points at localhost — that is what
// chromium bound — so dialing it verbatim from another container reaches
// nothing, or something else listening locally.
func TestRewriteWSHost(t *testing.T) {
	cases := []struct{ ws, base, want string }{
		{"ws://localhost:9222/devtools/page/AB", "http://chromium:9222", "ws://chromium:9222/devtools/page/AB"},
		// Chromium sometimes advertises no port at all; default to 9222 rather
		// than producing a portless ws:// URL that dials 80.
		{"ws://localhost/devtools/page/AB", "http://chromium", "ws://chromium:9222/devtools/page/AB"},
		{"ws://127.0.0.1:9222/devtools/browser/XY", "http://10.0.0.5:9333", "ws://10.0.0.5:9333/devtools/browser/XY"},
	}
	for _, c := range cases {
		got, err := rewriteWSHost(c.ws, c.base)
		if err != nil {
			t.Errorf("%s + %s: %v", c.ws, c.base, err)
			continue
		}
		if got != c.want {
			t.Errorf("%s + %s = %s, want %s", c.ws, c.base, got, c.want)
		}
	}
}

// The embedder enables chromium's DNS-rebinding protection, so /json answers
// 403 unless the Host header says localhost — and the body does not say why.
func TestPageTargetSendsLocalhostHostHeader(t *testing.T) {
	var gotHost string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotHost = r.Host
		if !strings.HasPrefix(r.Host, "localhost") {
			// Mirror the real embedder's behaviour so this test fails the same
			// way production would.
			http.Error(w, "Host header is not localhost", http.StatusForbidden)
			return
		}
		_ = json.NewEncoder(w).Encode([]cdpTarget{
			{Type: "page", URL: "about:blank", WebSocketDebuggerURL: "ws://localhost:9222/devtools/page/AB"},
		})
	}))
	defer srv.Close()

	c := &cdpClient{baseURL: srv.URL}
	ws, err := c.pageTarget(context.Background())
	if err != nil {
		t.Fatalf("pageTarget: %v (Host seen: %q)", err, gotHost)
	}
	if !strings.HasPrefix(gotHost, "localhost") {
		t.Errorf("Host header = %q, want localhost", gotHost)
	}
	// And the returned URL must point back at the server we actually reached.
	if strings.Contains(ws, "localhost:9222") {
		t.Errorf("webSocketDebuggerUrl was not rewritten: %s", ws)
	}
}

func TestPageTargetSkipsNonPageTargets(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode([]cdpTarget{
			{Type: "background_page", WebSocketDebuggerURL: "ws://localhost:9222/devtools/page/BG"},
			{Type: "page", WebSocketDebuggerURL: "ws://localhost:9222/devtools/page/REAL"},
		})
	}))
	defer srv.Close()

	ws, err := (&cdpClient{baseURL: srv.URL}).pageTarget(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(ws, "/REAL") {
		t.Errorf("picked the wrong target: %s", ws)
	}
}

// A booting worker has no page target yet. The error has to say so rather than
// surfacing as a nil dereference or an empty-URL dial.
func TestPageTargetErrorsWhenNoPageExists(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_ = json.NewEncoder(w).Encode([]cdpTarget{})
	}))
	defer srv.Close()

	if _, err := (&cdpClient{baseURL: srv.URL}).pageTarget(context.Background()); err == nil {
		t.Fatal("expected an error when no page target exists")
	}
}

func TestPageTargetReportsUnreachableWorker(t *testing.T) {
	// Nothing listening: the message should name the address rather than
	// leaking a bare dial error.
	c := &cdpClient{baseURL: "http://127.0.0.1:1"}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	_, err := c.pageTarget(ctx)
	if err == nil {
		t.Fatal("expected an error")
	}
	if !strings.Contains(err.Error(), "127.0.0.1:1") {
		t.Errorf("error does not name the endpoint: %v", err)
	}
}

// Navigation must be gated like everything else — an unauthenticated caller
// reaching /api/navigate could drive the browser anywhere.
func TestNavigationRoutesRequireSession(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	for _, path := range []string{"/api/navigate", "/api/back", "/api/forward", "/api/reload", "/api/stop", "/api/current-url"} {
		t.Run(path, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(`{"url":"https://example.com"}`))
			rec := httptest.NewRecorder()
			g.routes().ServeHTTP(rec, req)
			if rec.Code != http.StatusUnauthorized && rec.Code != http.StatusSeeOther {
				t.Fatalf("status = %d, want 401 or 303", rec.Code)
			}
		})
	}
}

// The scheme check must run BEFORE anything is sent to the worker, so a
// rejected URL is a 400 and never reaches CDP.
func TestNavigateRejectsBadSchemeWithoutCallingCDP(t *testing.T) {
	reached := false
	worker := httptest.NewServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		reached = true
	}))
	defer worker.Close()

	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: worker.URL}
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/navigate",
		strings.NewReader(`{"url":"file:///etc/passwd"}`))
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
	if reached {
		t.Error("a file:// URL reached the worker")
	}
}

func TestNavigateRejectsMalformedBody(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/navigate", strings.NewReader("not json"))
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Errorf("status = %d, want 400", rec.Code)
	}
}

func TestNavigateRejectsGET(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodGet, "/api/navigate", nil)
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusMethodNotAllowed {
		t.Errorf("status = %d, want 405", rec.Code)
	}
}

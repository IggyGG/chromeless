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

func TestClampViewportMatchesTheGuest(t *testing.T) {
	cases := []struct{ w, h, wantW, wantH int }{
		{1280, 720, 1280, 720},
		{1281, 721, 1280, 720},   // odd → down to even
		{100, 100, 200, 200},     // below the floor
		{9999, 9999, 2560, 2560}, // above the ceiling
		{2561, 2559, 2560, 2558}, // clamp then align, never back over the ceiling
	}
	for _, c := range cases {
		gotW, gotH := clampViewport(c.w, c.h)
		if gotW != c.wantW || gotH != c.wantH {
			t.Errorf("clampViewport(%d,%d) = %dx%d, want %dx%d", c.w, c.h, gotW, gotH, c.wantW, c.wantH)
		}
	}
}

// The fake worker answers every unknown method with {"frameId":"1"}, which
// has no geometry — exactly what an old guest without Cb.setViewport would
// NOT return (it returns -32601), but also what a misrouted reply looks like.
// Either way the client must not be told a size the guest never confirmed.
func TestSetViewportRejectsAnAckWithoutGeometry(t *testing.T) {
	fw := newFakeWorker(t)
	c := &cdpClient{baseURL: fw.srv.URL}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	if _, err := c.setViewport(ctx, 1280, 720); err == nil {
		t.Fatal("expected an error for an ack without width/height")
	}
	got := fw.methods()
	if len(got) != 1 || got[0] != "Cb.setViewport" {
		t.Fatalf("methods = %v, want [Cb.setViewport]", got)
	}
}

func TestViewportRouteRequiresSession(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	req := httptest.NewRequest(http.MethodPost, "/api/viewport", strings.NewReader(`{"width":800,"height":600}`))
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)
	if rec.Code != http.StatusUnauthorized && rec.Code != http.StatusSeeOther {
		t.Fatalf("status = %d, want 401 or 303", rec.Code)
	}
}

// A hostile or buggy client must never reach the worker with a size the guest
// would have to refuse: the clamp runs before CDP, so 8192x8192 arrives at
// the worker as 2560x2560.
func TestViewportClampsBeforeCallingCDP(t *testing.T) {
	var seen map[string]any
	worker := newFakeWorkerWithHandler(t, func(method string, params map[string]any) map[string]any {
		if method == "Cb.setViewport" {
			seen = params
			return map[string]any{"width": params["width"], "height": params["height"], "deviceScaleFactor": 1.0}
		}
		return map[string]any{}
	})
	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: worker.URL}
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/viewport", strings.NewReader(`{"width":8192,"height":8193}`))
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, body = %s", rec.Code, rec.Body.String())
	}
	if seen == nil {
		t.Fatal("Cb.setViewport never reached the worker")
	}
	if w, h := seen["width"], seen["height"]; w != float64(2560) || h != float64(2560) {
		t.Errorf("worker saw %vx%v, want 2560x2560", w, h)
	}
	var applied viewportApplied
	if err := json.Unmarshal(rec.Body.Bytes(), &applied); err != nil {
		t.Fatalf("decode ack: %v", err)
	}
	if applied.Width != 2560 || applied.Height != 2560 || applied.DeviceScaleFactor != 1.0 {
		t.Errorf("ack = %+v, want 2560x2560@1.0", applied)
	}
}

func TestViewportRejectsNonPositiveAndMalformed(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")
	for _, body := range []string{`{"width":0,"height":600}`, `{"width":-1,"height":600}`, `not json`, `{}`} {
		req := httptest.NewRequest(http.MethodPost, "/api/viewport", strings.NewReader(body))
		req.AddCookie(c)
		rec := httptest.NewRecorder()
		g.routes().ServeHTTP(rec, req)
		if rec.Code != http.StatusBadRequest {
			t.Errorf("body %q: status = %d, want 400", body, rec.Code)
		}
	}
}

// An old guest answers -32601 (method not found). That must surface as a 502
// with the CDP message, so the client can stop asking, not as a 200 with a
// fabricated size.
func TestViewportSurfacesMethodNotFound(t *testing.T) {
	fw := newFakeWorker(t)
	fw.failWith = "'Cb.setViewport' wasn't found"
	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: fw.srv.URL}
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodPost, "/api/viewport", strings.NewReader(`{"width":1024,"height":768}`))
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want 502; body %s", rec.Code, rec.Body.String())
	}
	if !strings.Contains(rec.Body.String(), "wasn't found") {
		t.Errorf("body should carry the CDP error, got %s", rec.Body.String())
	}
}

// CHROMELESS_VIEWPORT_FOLLOW=0 must answer 501 WITHOUT calling the guest, and
// with a JSON body the client can read as "unsupported" — a bare 501 with an
// HTML body would still stop the follower, but it would log `HTTP 501` where
// the operator's own setting should be named.
func TestViewportFollowSwitchRefusesBeforeCDP(t *testing.T) {
	called := false
	worker := newFakeWorkerWithHandler(t, func(method string, params map[string]any) map[string]any {
		if method == "Cb.setViewport" {
			called = true
		}
		return map[string]any{"width": 800, "height": 600, "deviceScaleFactor": 1}
	})
	g, _ := newTestGateway(t, http.NotFoundHandler())
	g.cdp = &cdpClient{baseURL: worker.URL}
	g.cfg.viewportFollow = false

	c := login(t, g, "operator", "s3cret")
	req := httptest.NewRequest(http.MethodPost, "/api/viewport", strings.NewReader(`{"width":800,"height":600}`))
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusNotImplemented {
		t.Fatalf("status = %d, want 501; body %s", rec.Code, rec.Body.String())
	}
	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("body is not JSON: %v (%s)", err, rec.Body.String())
	}
	if !strings.Contains(body["error"].(string), "CHROMELESS_VIEWPORT_FOLLOW") {
		t.Fatalf("error should name the switch, got %q", body["error"])
	}
	if called {
		t.Fatal("Cb.setViewport reached the guest with the follow switch off")
	}
}

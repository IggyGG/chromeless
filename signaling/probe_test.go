package main

import (
	"bytes"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func newProbeServer() *httptest.Server {
	logger := slog.New(slog.NewTextHandler(io.Discard, nil))
	return httptest.NewServer(probeHandler(logger))
}

// withAuthDisabled flips globalAuth off for the duration of the
// surrounding test and restores it on cleanup. Mirrors the technique
// used in auth_test.go.
func withAuthDisabled(t *testing.T) {
	t.Helper()
	prev := globalAuth
	globalAuth = authConfig{enabled: false}
	t.Cleanup(func() { globalAuth = prev })
}

func TestProbeGet_ReturnsRequestedSize(t *testing.T) {
	withAuthDisabled(t)
	srv := newProbeServer()
	defer srv.Close()

	resp, err := http.Get(srv.URL + "?size=1024")
	if err != nil {
		t.Fatalf("GET: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status: %d", resp.StatusCode)
	}
	got, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("read body: %v", err)
	}
	if len(got) != 1024 {
		t.Fatalf("want 1024 bytes, got %d", len(got))
	}
	// Body should be all zeros.
	if !bytes.Equal(got, make([]byte, 1024)) {
		t.Fatal("body is not all-zeros")
	}
	if ct := resp.Header.Get("Content-Type"); ct != "application/octet-stream" {
		t.Errorf("content-type: %q", ct)
	}
}

func TestProbeGet_ClampsLargeSize(t *testing.T) {
	withAuthDisabled(t)
	srv := newProbeServer()
	defer srv.Close()

	// Ask for 10× the cap; expect to receive exactly probeMaxQuerySize.
	resp, err := http.Get(srv.URL + "?size=10485760")
	if err != nil {
		t.Fatalf("GET: %v", err)
	}
	defer resp.Body.Close()
	got, _ := io.ReadAll(resp.Body)
	if len(got) != probeMaxQuerySize {
		t.Fatalf("want %d (clamped), got %d", probeMaxQuerySize, len(got))
	}
}

func TestProbeGet_RejectsBadSize(t *testing.T) {
	withAuthDisabled(t)
	srv := newProbeServer()
	defer srv.Close()

	for _, q := range []string{"", "?size=0", "?size=-1", "?size=abc"} {
		resp, err := http.Get(srv.URL + q)
		if err != nil {
			t.Fatalf("GET %q: %v", q, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusBadRequest {
			t.Errorf("query %q: want 400, got %d", q, resp.StatusCode)
		}
	}
}

func TestProbePost_EchoesByteCount(t *testing.T) {
	withAuthDisabled(t)
	srv := newProbeServer()
	defer srv.Close()

	body := bytes.Repeat([]byte{'x'}, 4096)
	resp, err := http.Post(srv.URL, "application/octet-stream", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("status: %d", resp.StatusCode)
	}
	var got struct {
		Received int64 `json:"received"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got.Received != int64(len(body)) {
		t.Errorf("received: want %d, got %d", len(body), got.Received)
	}
}

func TestProbePost_RejectsOversizedBody(t *testing.T) {
	withAuthDisabled(t)
	srv := newProbeServer()
	defer srv.Close()

	body := bytes.Repeat([]byte{'x'}, probeMaxBytes+1024)
	resp, err := http.Post(srv.URL, "application/octet-stream", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("POST: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusRequestEntityTooLarge {
		t.Errorf("oversized: want 413, got %d", resp.StatusCode)
	}
}

func TestProbe_RejectsOtherMethods(t *testing.T) {
	withAuthDisabled(t)
	srv := newProbeServer()
	defer srv.Close()

	req, _ := http.NewRequest("DELETE", srv.URL, nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("DELETE: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("want 405, got %d", resp.StatusCode)
	}
	if a := resp.Header.Get("Allow"); !strings.Contains(a, "GET") {
		t.Errorf("Allow header missing GET: %q", a)
	}
}

func TestProbe_OPTIONS_PreflightDoesNotRequireAuth(t *testing.T) {
	// Even when auth is enabled, CORS preflight should pass through —
	// browsers don't send the token on the preflight OPTIONS.
	prev := globalAuth
	globalAuth = authConfig{enabled: true} // pubKey nil; would reject any token
	t.Cleanup(func() { globalAuth = prev })

	srv := newProbeServer()
	defer srv.Close()

	req, _ := http.NewRequest(http.MethodOptions, srv.URL, nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("OPTIONS: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		t.Errorf("want 204, got %d", resp.StatusCode)
	}
	if methods := resp.Header.Get("Access-Control-Allow-Methods"); !strings.Contains(methods, "POST") {
		t.Errorf("preflight missing POST in Allow-Methods: %q", methods)
	}
}

func TestProbe_AuthEnabled_RejectsMissingToken(t *testing.T) {
	prev := globalAuth
	globalAuth = authConfig{enabled: true}
	t.Cleanup(func() { globalAuth = prev })

	srv := newProbeServer()
	defer srv.Close()

	resp, err := http.Get(srv.URL + "?size=10")
	if err != nil {
		t.Fatalf("GET: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Errorf("want 401, got %d", resp.StatusCode)
	}
}

func TestProbe_AuthEnabled_RejectsMalformedToken(t *testing.T) {
	prev := globalAuth
	globalAuth = authConfig{enabled: true}
	t.Cleanup(func() { globalAuth = prev })

	srv := newProbeServer()
	defer srv.Close()

	resp, err := http.Get(srv.URL + "?size=10&token=not-a-jwt")
	if err != nil {
		t.Fatalf("GET: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Errorf("want 401, got %d", resp.StatusCode)
	}
}

// Package main — fixture.go
//
// A test-only endpoint that stores one HTML document in memory and serves it
// back, so an interaction test can put a known page in front of the worker.
//
// WHY THIS HAS TO EXIST HERE. Testing input needs a page with known
// coordinates and observable state (a click counter, a text input). Getting
// such a page in front of the worker is harder than it looks:
//
//   - `data:` URLs are refused by the navigation allowlist, deliberately —
//     data:, file: and javascript: are exactly what it exists to block, and
//     poking a hole for tests would defeat the check being tested.
//   - A server on the test machine is unreachable: the worker is a cluster pod
//     behind NAT and cannot dial back to a laptop.
//   - A third-party page works for "does the web render", but its coordinates
//     and DOM change without warning, so it cannot anchor an input assertion.
//
// The gateway is already reachable from the worker and already serves static
// content, so it is the natural place to host a fixture.
//
// SERVED OVER PLAIN HTTP ON A SEPARATE PORT, not through the TLS listener.
// The worker is Chromium and it VALIDATES certificates — it cannot click
// through the gateway's self-signed one the way a human can. Navigating it to
// the https:// fixture fails the handshake with net_error -202
// (ERR_CERT_AUTHORITY_INVALID) and renders an error page, so the test finds no
// elements and reports a mouse failure that has nothing to do with the mouse.
// (The same limitation is why infra/compose.worker.yaml tells split-host
// deployments to mount a real certificate.)
//
// GATED OFF BY DEFAULT, enabled only with CHROMELESS_ENABLE_TEST_FIXTURE=1.
// The listener is cluster-internal and unauthenticated: its consumer is the
// worker, which has no session cookie, and what it serves is content the
// operator just uploaded through the authenticated port.

package main

import (
	"io"
	"log/slog"
	"net/http"
	"sync"
	"time"
)

// fixturePort is the plain-HTTP listener. Deliberately not 8443: that one is
// TLS-only and host-published, and this must be neither.
const fixturePort = "8081"

// serveFixtureListener starts the plain-HTTP fixture server. No-op when the
// feature is off.
func (g *gateway) serveFixtureListener() {
	if !g.cfg.enableTestFixture {
		return
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/api/test-fixture", g.handleFixture)
	srv := &http.Server{Addr: ":" + fixturePort, Handler: mux,
		ReadHeaderTimeout: 5 * time.Second}
	go func() {
		g.log.Warn("TEST FIXTURE endpoint enabled on plain HTTP — "+
			"do not enable this in a deployment you care about",
			slog.String("addr", ":"+fixturePort))
		if err := srv.ListenAndServe(); err != nil {
			g.log.Error("fixture listener stopped", slog.Any("err", err))
		}
	}()
}

type fixtureStore struct {
	mu   sync.RWMutex
	html []byte
}

// handleFixture stores a document on POST and serves it on GET.
func (g *gateway) handleFixture(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodPost:
		body, err := io.ReadAll(http.MaxBytesReader(w, r.Body, 512<<10))
		if err != nil {
			writeJSONError(w, http.StatusBadRequest, "unreadable body")
			return
		}
		g.fixture.mu.Lock()
		g.fixture.html = body
		g.fixture.mu.Unlock()
		writeJSON(w, map[string]any{"stored": len(body)})

	case http.MethodGet:
		g.fixture.mu.RLock()
		body := g.fixture.html
		g.fixture.mu.RUnlock()
		if len(body) == 0 {
			http.Error(w, "no fixture stored", http.StatusNotFound)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		// The worker must re-fetch on every navigation, or a second test run
		// silently interacts with the first run's page.
		w.Header().Set("Cache-Control", "no-store")
		_, _ = w.Write(body)

	default:
		w.Header().Set("Allow", "GET, POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

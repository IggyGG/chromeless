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
// GATED OFF BY DEFAULT. Enabled only with CHROMELESS_ENABLE_TEST_FIXTURE=1,
// and still behind the session cookie. Left on it is a stored-content endpoint
// on an authenticated origin — not a serious hole, but not something a
// production deployment should carry either.

package main

import (
	"io"
	"net/http"
	"sync"
)

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

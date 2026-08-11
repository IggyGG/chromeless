// Package main — navigate.go
//
// The address-bar endpoints. Named after physics' `/ops/*` verbs
// (chemistry/elements/tools/chromeless/.triform/ops.yaml) so that a portal
// pointed at a standalone deployment is a re-point rather than a redesign.
//
//	POST /api/navigate      {"url": "..."}   → Page.navigate
//	POST /api/back                           → Page.goBack
//	POST /api/forward                        → Page.goForward
//	POST /api/reload                         → Page.reload
//	POST /api/stop                           → Page.stopLoading
//	GET  /api/current-url                    → {"url": "..."}
//
// Back and forward drive real CDP history rather than a client-side vector.
// The portal learned that the hard way: a local vector only sees the
// navigations the UI performed, so it diverges the moment the page navigates
// itself — a client-side router, a redirect, a link click — and then Back goes
// somewhere the user never was.

package main

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"time"
)

// navTimeout bounds a CDP round trip. Page.navigate returns when the
// navigation is COMMITTED, not when the page has loaded, so this does not need
// to cover a slow site — only a wedged or still-booting worker.
const navTimeout = 15 * time.Second

type navigateRequest struct {
	URL string `json:"url"`
}

func (g *gateway) handleNavigate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var body navigateRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 8<<10)).Decode(&body); err != nil {
		writeJSONError(w, http.StatusBadRequest, "malformed JSON body")
		return
	}

	target, err := validateNavigationURL(body.URL)
	if err != nil {
		// 400 with the reason: this one is genuinely the caller's fault and
		// the address bar shows the message.
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), navTimeout)
	defer cancel()

	if err := g.cdp.navigate(ctx, target); err != nil {
		g.log.Error("navigate failed", slog.String("url", target), slog.Any("err", err))
		writeJSONError(w, http.StatusBadGateway, err.Error())
		return
	}
	g.log.Info("navigated", slog.String("url", target))
	writeJSON(w, map[string]any{"url": target})
}

// navCommand builds a handler for a parameterless Page verb.
func (g *gateway) navCommand(method string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", "POST")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), navTimeout)
		defer cancel()

		if err := g.cdp.simpleCommand(ctx, method); err != nil {
			// Page.goBack with no history behind it is an error from CDP but
			// not from the user — the button is simply a no-op there. Log it
			// and report success rather than surfacing a scary banner for
			// pressing Back on the first page.
			g.log.Info("nav command declined", slog.String("method", method), slog.Any("err", err))
			writeJSON(w, map[string]any{"ok": false, "reason": err.Error()})
			return
		}
		writeJSON(w, map[string]any{"ok": true})
	}
}

func (g *gateway) handleCurrentURL(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), navTimeout)
	defer cancel()

	url, err := g.cdp.currentURL(ctx)
	if err != nil {
		writeJSONError(w, http.StatusBadGateway, err.Error())
		return
	}
	writeJSON(w, map[string]any{"url": url})
}

// navigateAtStartup honours CHROMIUM_START_URL, which until now was a dead
// knob: infra/launch-chromeless.sh passes it as --app=<url>, and the embedder
// reads only --remote-debugging-{port,address}, so it has never had any effect.
//
// Best-effort and retried, because the worker's DevTools endpoint is usually
// not up yet when the gateway starts. Failure is logged and dropped: a start
// page that could not be reached is not a reason to refuse to serve the UI.
func (g *gateway) navigateAtStartup(rawURL string) {
	target, err := validateNavigationURL(rawURL)
	if err != nil {
		g.log.Error("CHROMIUM_START_URL rejected", slog.String("url", rawURL), slog.Any("err", err))
		return
	}

	go func() {
		// ~60s of attempts. cold-start.sh + Xvfb + Chromium take a while, and
		// compose's own healthcheck allows a 45s start period.
		for attempt := 1; attempt <= 20; attempt++ {
			ctx, cancel := context.WithTimeout(context.Background(), navTimeout)
			err := g.cdp.navigate(ctx, target)
			cancel()
			if err == nil {
				g.log.Info("start url loaded", slog.String("url", target))
				return
			}
			time.Sleep(3 * time.Second)
		}
		g.log.Warn("gave up loading the start url; the worker never became reachable",
			slog.String("url", target))
	}()
}

func writeJSON(w http.ResponseWriter, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(body)
}

func writeJSONError(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

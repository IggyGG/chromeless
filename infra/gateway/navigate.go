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

// navTimeout bounds a CDP round trip.
//
// The original 15s rested on "Page.navigate returns when the navigation is
// COMMITTED, not when the page has loaded, so this does not need to cover a
// slow site". That is true of a site that ANSWERS. It is not true of one that
// misbehaves: measured 2026-08-17 against httpbin.org while it was flapping
// between 200 and 503, Page.navigate did not return within 15s at all, and —
// because the renderer was still working on it — the NEXT navigation timed out
// too, at exactly 15.0s. Two failed navigations, one misbehaving third party,
// no defect in this gateway or the worker.
//
// 45s covers a site that is retrying or slow to commit, while still bounding a
// genuinely wedged worker to well under a minute. The cost of being too
// generous here is a slower error; the cost of being too tight is a false
// failure attributed to chromeless, which is strictly worse.
//
// Note this bounds a SINGLE round trip. The renderer commits navigations
// serially, so a request queued behind a slow one waits for it — a caller
// issuing back-to-back navigations to unreliable hosts should expect the
// second to inherit the first's delay.
const navTimeout = 45 * time.Second

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

	// Re-arm capture after the navigation. A cross-document navigation swaps
	// the RenderWidgetHost and with it the FrameSinkId, so a capturer bound to
	// the old one goes silent — the exact bug fixed in b482176 on the embedder
	// side ("nav swaps RVH+FSID, capturer never re-armed"). The embedder
	// re-arms on RenderViewHostChanged, but arming again here is cheap and
	// covers the case where capture was never armed at all.
	//
	// Best-effort: a navigation that worked should not be reported as failed
	// because the re-arm raced the swap.
	if err := g.cdp.armCapture(ctx); err != nil {
		g.log.Warn("capture re-arm after navigate failed",
			slog.String("url", target), slog.Any("err", err))
	}

	g.log.Info("navigated", slog.String("url", target))
	writeJSON(w, map[string]any{"url": target})
}

// historyCommand builds a handler for back (-1) / forward (+1).
func (g *gateway) historyCommand(delta int) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", "POST")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), navTimeout)
		defer cancel()

		if err := g.cdp.historyStep(ctx, delta); err != nil {
			// Running off the end of the history is a no-op, not an error;
			// reported as declined so Back on the first page raises nothing.
			g.log.Info("history step declined",
				slog.Int("delta", delta), slog.Any("err", err))
			writeJSON(w, map[string]any{"ok": false, "reason": err.Error()})
			return
		}
		// Re-arm capture: a history navigation swaps the RenderWidgetHost and
		// the FrameSinkId exactly as a fresh navigation does.
		if err := g.cdp.armCapture(ctx); err != nil {
			g.log.Warn("capture re-arm after history step failed", slog.Any("err", err))
		}
		writeJSON(w, map[string]any{"ok": true})
	}
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

// armCaptureWhenReady arms FrameSink capture as soon as the worker is
// reachable, and keeps trying while it is not.
//
// This is the difference between "a video track was negotiated" and "you can
// see the browser". Nothing else in a standalone deployment issues
// Cb.startFrameSinkCapture: in triform that is physics's job, and here the
// gateway is what stands in its place. Left unarmed, the worker's compositor
// ticks at 30fps into nothing, the client shows a black video element, and
// after 30 seconds the worker declares CV2-GPU-DEATH and kills itself.
//
// Retried rather than fired once, because the gateway usually wins the startup
// race: the worker needs ~15s for Xvfb + Chromium before DevTools answers at
// all. Keeps going after success too — the worker self-terminates on some
// failure paths and supervisord restarts it, and an unarmed replacement is
// exactly as blind as an unarmed original.
func (g *gateway) armCaptureWhenReady() {
	go func() {
		armed := false
		for {
			ctx, cancel := context.WithTimeout(context.Background(), navTimeout)
			err := g.cdp.armCapture(ctx)
			cancel()

			switch {
			case err == nil && !armed:
				g.log.Info("framesink capture armed")
				armed = true
			case err != nil && armed:
				// Lost it — most likely the worker restarted underneath us.
				g.log.Warn("capture arm failed after previously succeeding; "+
					"worker may have restarted", slog.Any("err", err))
				armed = false
			}
			// 15s idle / 3s while trying to (re)establish. Cheap either way:
			// one CDP round trip.
			if armed {
				time.Sleep(15 * time.Second)
			} else {
				time.Sleep(3 * time.Second)
			}
		}
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

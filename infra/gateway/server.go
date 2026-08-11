// Package main — server.go
//
// Route table, auth middleware, and the reverse proxy to the signaling broker.
//
// Everything the browser needs arrives on ONE origin: the page, the signaling
// WebSocket, the ICE config, and (later) navigation. That is not just tidiness
// — it is what makes the self-signed certificate workable, since accepting it
// once covers the wss:// dial too. It also means client/src/config.ts can
// derive its endpoint from the page origin and be right by construction.

package main

import (
	_ "embed"
	"html"
	"log/slog"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
)

//go:embed login.html
var loginHTML string

type gateway struct {
	cfg      *config
	sessions *sessionStore
	log      *slog.Logger
	proxy    *httputil.ReverseProxy
}

func newGateway(cfg *config, logger *slog.Logger) (*gateway, error) {
	target, err := url.Parse(cfg.signalingURL)
	if err != nil {
		return nil, err
	}
	proxy := httputil.NewSingleHostReverseProxy(target)

	// A broker that is down must not look like a gateway bug. The default
	// ErrorHandler logs to the standard logger and returns a bare 502.
	proxy.ErrorHandler = func(w http.ResponseWriter, r *http.Request, err error) {
		logger.Error("signaling proxy failed",
			slog.String("path", r.URL.Path), slog.Any("err", err))
		http.Error(w, "signaling backend unavailable", http.StatusBadGateway)
	}

	return &gateway{cfg: cfg, sessions: newSessionStore(cfg.sessionTTL), log: logger, proxy: proxy}, nil
}

func (g *gateway) routes() http.Handler {
	mux := http.NewServeMux()

	// Unauthenticated: the login form itself, and a liveness probe. /healthz
	// reports that the gateway is up, deliberately saying nothing about the
	// broker or the worker — it is a container healthcheck, not a status page.
	mux.HandleFunc("/login", g.handleLogin)
	mux.HandleFunc("/logout", g.handleLogout)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})

	// Proxied to the broker. /ws/ and /api/webrtc/signaling/ are the two
	// prefixes signaling/server.go routes on: the client dials the former, a
	// native worker the latter. Both are here so a worker on another host can
	// come in through the same TLS port.
	mux.Handle("/ws/", g.requireSession(http.HandlerFunc(g.handleProxy)))
	mux.Handle("/api/webrtc/signaling/", g.requireSession(http.HandlerFunc(g.handleProxy)))
	mux.Handle("/turn-credentials", g.requireSession(http.HandlerFunc(g.handleProxy)))

	// /probe carries its own JWT in the query string (client/src/probe.ts
	// resolveProbeURL), and the broker verifies it. Wrapping it in a cookie
	// check as well would be redundant, and would break a non-browser caller
	// that legitimately holds a token.
	mux.HandleFunc("/probe", g.handleProxy)

	// Everything else is the client bundle, behind the session.
	mux.Handle("/", g.requireSession(http.HandlerFunc(g.handleStatic)))

	return mux
}

// requireSession gates a handler on a valid session cookie.
//
// The unauthenticated response differs by request kind on purpose. A browser
// navigating to / should land on the login form; an XHR or a WebSocket
// upgrade must get a 401, because redirecting those produces a confusing
// failure (the fetch succeeds with an HTML body, or the WS handshake fails
// with no explanation).
func (g *gateway) requireSession(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if g.sessions.valid(sessionIDFromRequest(r)) {
			next.ServeHTTP(w, r)
			return
		}
		if wantsHTML(r) {
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}
		http.Error(w, "unauthorized", http.StatusUnauthorized)
	})
}

// wantsHTML reports whether this looks like a top-level browser navigation
// rather than a programmatic request.
func wantsHTML(r *http.Request) bool {
	if strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		return false
	}
	// Fetch metadata is the reliable signal where it exists; Accept is the
	// fallback for older clients.
	if mode := r.Header.Get("Sec-Fetch-Mode"); mode != "" {
		return mode == "navigate"
	}
	return strings.Contains(r.Header.Get("Accept"), "text/html")
}

func (g *gateway) handleLogin(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		// Already signed in — skip the form rather than inviting a pointless
		// re-entry of the password.
		if g.sessions.valid(sessionIDFromRequest(r)) {
			http.Redirect(w, r, "/", http.StatusSeeOther)
			return
		}
		g.renderLogin(w, http.StatusOK, "")
	case http.MethodPost:
		g.doLogin(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (g *gateway) doLogin(w http.ResponseWriter, r *http.Request) {
	key := clientKey(r)
	if g.sessions.lockedOut(key) {
		g.log.Warn("login locked out", slog.String("remote", key))
		g.renderLogin(w, http.StatusTooManyRequests,
			"Too many failed attempts. Try again later.")
		return
	}

	if err := r.ParseForm(); err != nil {
		g.renderLogin(w, http.StatusBadRequest, "Malformed form submission.")
		return
	}

	if !checkCredentials(g.cfg, r.PostFormValue("username"), r.PostFormValue("password")) {
		g.sessions.recordFailure(key)
		g.log.Warn("login failed", slog.String("remote", key))
		// One message for both wrong-user and wrong-password: distinguishing
		// them would confirm which usernames exist.
		g.renderLogin(w, http.StatusUnauthorized, "Incorrect username or password.")
		return
	}

	id, err := g.sessions.create()
	if err != nil {
		g.log.Error("session creation failed", slog.Any("err", err))
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	g.sessions.clearFailures(key)
	setSessionCookie(w, id, g.cfg.sessionTTL)
	g.log.Info("login ok", slog.String("remote", key))
	http.Redirect(w, r, "/", http.StatusSeeOther)
}

func (g *gateway) handleLogout(w http.ResponseWriter, r *http.Request) {
	g.sessions.destroy(sessionIDFromRequest(r))
	clearSessionCookie(w)
	http.Redirect(w, r, "/login", http.StatusSeeOther)
}

// renderLogin writes the embedded form with an optional error message.
func (g *gateway) renderLogin(w http.ResponseWriter, status int, errMsg string) {
	// Escaped even though every current message is a literal: the moment one
	// of them interpolates user input, an unescaped substitution here becomes
	// reflected XSS on the one page served pre-authentication.
	body := strings.Replace(loginHTML, "__ERROR__", html.EscapeString(errMsg), 1)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_, _ = w.Write([]byte(body))
}

func (g *gateway) handleProxy(w http.ResponseWriter, r *http.Request) {
	g.proxy.ServeHTTP(w, r)
}

// handleStatic serves the built client bundle.
func (g *gateway) handleStatic(w http.ResponseWriter, r *http.Request) {
	http.FileServer(http.Dir(g.cfg.staticDir)).ServeHTTP(w, r)
}

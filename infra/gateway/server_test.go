package main

import (
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

// newTestGateway wires a gateway against a stub "broker" and returns both, so
// tests can assert on what actually reached the backend.
func newTestGateway(t *testing.T, backend http.Handler) (*gateway, *httptest.Server) {
	t.Helper()
	upstream := httptest.NewServer(backend)
	t.Cleanup(upstream.Close)

	cfg := &config{
		user:         "operator",
		pass:         "s3cret",
		signalingURL: upstream.URL,
		staticDir:    t.TempDir(),
		sessionTTL:   time.Hour,
	}
	g, err := newGateway(cfg, quietLogger())
	if err != nil {
		t.Fatalf("newGateway: %v", err)
	}
	return g, upstream
}

// login performs a real form POST and returns the session cookie.
func login(t *testing.T, g *gateway, user, pass string) *http.Cookie {
	t.Helper()
	form := url.Values{"username": {user}, "password": {pass}}
	req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	for _, c := range rec.Result().Cookies() {
		if c.Name == sessionCookieName && c.Value != "" {
			return c
		}
	}
	return nil
}

func TestLoginSuccessSetsCookie(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	c := login(t, g, "operator", "s3cret")
	if c == nil {
		t.Fatal("no session cookie issued on a correct login")
	}
	// A cookie that can travel in the clear is one downgrade away from being
	// handed over; one reachable from JS is one XSS away.
	if !c.HttpOnly {
		t.Error("session cookie is not HttpOnly")
	}
	if !c.Secure {
		t.Error("session cookie is not Secure")
	}
}

func TestLoginRejectsWrongCredentials(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	for _, tc := range []struct{ name, user, pass string }{
		{"wrong password", "operator", "nope"},
		{"wrong username", "someone", "s3cret"},
		{"both wrong", "someone", "nope"},
		{"empty", "", ""},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if c := login(t, g, tc.user, tc.pass); c != nil {
				t.Fatal("issued a session cookie for bad credentials")
			}
		})
	}
}

// The failure message must not distinguish a bad username from a bad password,
// or it confirms which usernames exist.
func TestLoginFailureDoesNotRevealWhichHalfWasWrong(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	body := func(user, pass string) string {
		form := url.Values{"username": {user}, "password": {pass}}
		req := httptest.NewRequest(http.MethodPost, "/login", strings.NewReader(form.Encode()))
		req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		rec := httptest.NewRecorder()
		g.routes().ServeHTTP(rec, req)
		return rec.Body.String()
	}

	if body("operator", "wrong") != body("nosuchuser", "wrong") {
		t.Error("login response differs between a known and an unknown username")
	}
}

func TestProtectedRoutesRequireSession(t *testing.T) {
	reached := false
	g, _ := newTestGateway(t, http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		reached = true
	}))

	// If any of these leaks, the whole login is decorative: /ws/ is the
	// signaling path and / serves the client that drives the browser.
	for _, path := range []string{"/", "/ws/dev", "/api/webrtc/signaling/dev", "/turn-credentials"} {
		t.Run(path, func(t *testing.T) {
			reached = false
			req := httptest.NewRequest(http.MethodGet, path, nil)
			rec := httptest.NewRecorder()
			g.routes().ServeHTTP(rec, req)

			if reached {
				t.Fatal("request reached the backend without a session")
			}
			if code := rec.Code; code != http.StatusUnauthorized && code != http.StatusSeeOther {
				t.Fatalf("status = %d, want 401 or 303", code)
			}
		})
	}
}

// A browser navigation should land on the login form; an XHR or a WebSocket
// upgrade must get a 401, since a redirect there fails opaquely.
func TestUnauthenticatedResponseKind(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	t.Run("navigation redirects", func(t *testing.T) {
		req := httptest.NewRequest(http.MethodGet, "/", nil)
		req.Header.Set("Sec-Fetch-Mode", "navigate")
		rec := httptest.NewRecorder()
		g.routes().ServeHTTP(rec, req)
		if rec.Code != http.StatusSeeOther {
			t.Fatalf("status = %d, want 303", rec.Code)
		}
		if loc := rec.Header().Get("Location"); loc != "/login" {
			t.Errorf("Location = %q, want /login", loc)
		}
	})

	t.Run("websocket upgrade gets 401", func(t *testing.T) {
		req := httptest.NewRequest(http.MethodGet, "/ws/dev", nil)
		req.Header.Set("Upgrade", "websocket")
		// Deliberately also claim to accept HTML: a real WS upgrade from a
		// browser does, and matching on Accept alone would misroute it.
		req.Header.Set("Accept", "text/html")
		rec := httptest.NewRecorder()
		g.routes().ServeHTTP(rec, req)
		if rec.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want 401", rec.Code)
		}
	})

	t.Run("xhr gets 401", func(t *testing.T) {
		req := httptest.NewRequest(http.MethodGet, "/turn-credentials", nil)
		req.Header.Set("Sec-Fetch-Mode", "cors")
		rec := httptest.NewRecorder()
		g.routes().ServeHTTP(rec, req)
		if rec.Code != http.StatusUnauthorized {
			t.Fatalf("status = %d, want 401", rec.Code)
		}
	})
}

func TestSessionCookieGrantsAccess(t *testing.T) {
	var gotPath string
	g, _ := newTestGateway(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		_, _ = w.Write([]byte("relayed"))
	}))

	c := login(t, g, "operator", "s3cret")
	if c == nil {
		t.Fatal("login failed")
	}

	req := httptest.NewRequest(http.MethodGet, "/turn-credentials", nil)
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if gotPath != "/turn-credentials" {
		t.Errorf("backend saw %q, want /turn-credentials", gotPath)
	}
}

// The path must survive proxying unchanged: the broker routes on the /ws/
// prefix and treats the remainder as the session id (signaling/server.go).
func TestProxyPreservesPathAndQuery(t *testing.T) {
	var gotURL string
	g, _ := newTestGateway(t, http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		gotURL = r.URL.String()
	}))

	c := login(t, g, "operator", "s3cret")
	req := httptest.NewRequest(http.MethodGet, "/ws/my-session?token=abc", nil)
	req.AddCookie(c)
	g.routes().ServeHTTP(httptest.NewRecorder(), req)

	if gotURL != "/ws/my-session?token=abc" {
		t.Errorf("backend saw %q, want the path and query intact", gotURL)
	}
}

// The WebSocket handshake headers must reach the broker, or the upgrade fails
// with no useful diagnostic.
func TestProxyForwardsUpgradeHeaders(t *testing.T) {
	var upgrade, connection string
	g, _ := newTestGateway(t, http.HandlerFunc(func(_ http.ResponseWriter, r *http.Request) {
		upgrade = r.Header.Get("Upgrade")
		connection = r.Header.Get("Connection")
	}))

	c := login(t, g, "operator", "s3cret")
	req := httptest.NewRequest(http.MethodGet, "/ws/dev", nil)
	req.Header.Set("Upgrade", "websocket")
	req.Header.Set("Connection", "Upgrade")
	req.AddCookie(c)
	g.routes().ServeHTTP(httptest.NewRecorder(), req)

	if !strings.EqualFold(upgrade, "websocket") {
		t.Errorf("Upgrade header = %q, want websocket", upgrade)
	}
	if !strings.Contains(strings.ToLower(connection), "upgrade") {
		t.Errorf("Connection header = %q, want it to contain Upgrade", connection)
	}
}

// /probe carries its own JWT and the broker verifies it; requiring a cookie
// too would break a legitimate non-browser caller holding a token.
func TestProbeIsNotCookieGated(t *testing.T) {
	reached := false
	g, _ := newTestGateway(t, http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		reached = true
	}))

	req := httptest.NewRequest(http.MethodGet, "/probe?size=1", nil)
	g.routes().ServeHTTP(httptest.NewRecorder(), req)

	if !reached {
		t.Error("/probe was blocked; it is token-authed at the broker, not cookie-authed")
	}
}

func TestHealthzIsUnauthenticated(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (container healthchecks have no cookie)", rec.Code)
	}
}

func TestLogoutInvalidatesTheSession(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")
	if c == nil {
		t.Fatal("login failed")
	}

	req := httptest.NewRequest(http.MethodPost, "/logout", nil)
	req.AddCookie(c)
	g.routes().ServeHTTP(httptest.NewRecorder(), req)

	// Server-side invalidation is the point of an opaque cookie: replaying
	// the same value must not work.
	check := httptest.NewRequest(http.MethodGet, "/turn-credentials", nil)
	check.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, check)
	if rec.Code == http.StatusOK {
		t.Error("the old cookie still works after logout")
	}
}

func TestForgedCookieIsRejected(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	req := httptest.NewRequest(http.MethodGet, "/turn-credentials", nil)
	req.AddCookie(&http.Cookie{Name: sessionCookieName, Value: "made-up-value"})
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code == http.StatusOK {
		t.Error("an unknown session id was accepted")
	}
}

// A published port is reachable by anything that can route to the host, so an
// unthrottled form is an offline-speed password oracle.
func TestLockoutAfterRepeatedFailures(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	for i := 0; i < maxFailedAttempts; i++ {
		if c := login(t, g, "operator", "wrong"); c != nil {
			t.Fatal("bad credentials produced a session")
		}
	}

	// The correct password must now be refused too — otherwise the lockout
	// only rate-limits an attacker who already knows the password.
	if c := login(t, g, "operator", "s3cret"); c != nil {
		t.Fatal("lockout did not engage: correct credentials still logged in")
	}
}

// A human who mistypes once and then succeeds must not accumulate toward a
// lockout on their next mistake.
func TestSuccessfulLoginClearsFailureCount(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	for i := 0; i < maxFailedAttempts-1; i++ {
		login(t, g, "operator", "wrong")
	}
	if c := login(t, g, "operator", "s3cret"); c == nil {
		t.Fatal("correct credentials rejected before the lockout threshold")
	}
	for i := 0; i < maxFailedAttempts-1; i++ {
		login(t, g, "operator", "wrong")
	}
	if c := login(t, g, "operator", "s3cret"); c == nil {
		t.Fatal("failure count was not reset by a successful login")
	}
}

func TestExpiredSessionIsRejected(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")
	if c == nil {
		t.Fatal("login failed")
	}

	g.sessions.now = func() time.Time { return time.Now().Add(2 * time.Hour) }

	req := httptest.NewRequest(http.MethodGet, "/turn-credentials", nil)
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)
	if rec.Code == http.StatusOK {
		t.Error("an expired session was accepted")
	}
}

// The error message is interpolated into the embedded page. Every current
// message is a literal, but the moment one carries user input an unescaped
// substitution is reflected XSS on the one pre-auth page.
func TestLoginErrorIsHTMLEscaped(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	rec := httptest.NewRecorder()
	g.renderLogin(rec, http.StatusUnauthorized, `<script>alert(1)</script>`)

	if strings.Contains(rec.Body.String(), "<script>alert(1)</script>") {
		t.Error("login error message is not HTML-escaped")
	}
}

func TestLoginPageServedWithoutSession(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/login", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if !strings.Contains(rec.Body.String(), `name="password"`) {
		t.Error("login form not rendered")
	}
}

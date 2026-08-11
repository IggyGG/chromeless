// Package main — session.go
//
// Login, the cookie session store, and the per-IP throttle on failed attempts.
//
// Scope: ONE operator credential, held in the environment. This is not a user
// system — there are no accounts, no roles, and no registration. That matches
// what the rest of the stack can actually support (one session per worker
// process; see the README), and pretending otherwise would be a security
// theatre layer over a single-tenant deployment.
//
// The cookie carries an opaque random id, not a signed claim. A server-side
// map means logout and restart both genuinely invalidate — with a signed
// cookie, "log out" would only be a client-side suggestion.

package main

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const (
	sessionCookieName = "chromeless_session"

	// A published port is reachable by anything that can route to the host, so
	// an unthrottled login form is an offline-speed password oracle. These
	// numbers stop automated guessing without locking out a human who
	// fat-fingers a password a few times.
	maxFailedAttempts = 5
	lockoutWindow     = 15 * time.Minute
)

type sessionStore struct {
	mu       sync.Mutex
	sessions map[string]time.Time // session id -> expiry
	failures map[string]*failureRecord
	ttl      time.Duration
	now      func() time.Time // injectable for tests
}

type failureRecord struct {
	count int
	until time.Time
}

func newSessionStore(ttl time.Duration) *sessionStore {
	return &sessionStore{
		sessions: make(map[string]time.Time),
		failures: make(map[string]*failureRecord),
		ttl:      ttl,
		now:      time.Now,
	}
}

// create mints a session id and records its expiry.
func (s *sessionStore) create() (string, error) {
	var buf [32]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	id := base64.RawURLEncoding.EncodeToString(buf[:])

	s.mu.Lock()
	defer s.mu.Unlock()
	s.sessions[id] = s.now().Add(s.ttl)
	// Opportunistic sweep. Without it the map grows unbounded across a long
	// uptime; there is no other reaper and no reason to run a goroutine for
	// something this cheap.
	for k, exp := range s.sessions {
		if s.now().After(exp) {
			delete(s.sessions, k)
		}
	}
	return id, nil
}

// valid reports whether id names a live session, deleting it if it has aged out.
func (s *sessionStore) valid(id string) bool {
	if id == "" {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	exp, ok := s.sessions[id]
	if !ok {
		return false
	}
	if s.now().After(exp) {
		delete(s.sessions, id)
		return false
	}
	return true
}

func (s *sessionStore) destroy(id string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.sessions, id)
}

// lockedOut reports whether key has burned through its attempt budget.
func (s *sessionStore) lockedOut(key string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	rec, ok := s.failures[key]
	if !ok {
		return false
	}
	if s.now().After(rec.until) {
		delete(s.failures, key)
		return false
	}
	return rec.count >= maxFailedAttempts
}

// recordFailure counts a bad password against key. Each failure re-arms the
// window, so a slow-drip attacker cannot wait out the lockout while still
// making progress.
func (s *sessionStore) recordFailure(key string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rec, ok := s.failures[key]
	if !ok || s.now().After(rec.until) {
		rec = &failureRecord{}
		s.failures[key] = rec
	}
	rec.count++
	rec.until = s.now().Add(lockoutWindow)
}

func (s *sessionStore) clearFailures(key string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.failures, key)
}

// checkCredentials compares in constant time. Both halves are always compared
// even when the username is wrong: bailing early on a username mismatch leaks,
// through timing, which half was wrong.
func checkCredentials(cfg *config, user, pass string) bool {
	userOK := subtle.ConstantTimeCompare([]byte(user), []byte(cfg.user)) == 1
	passOK := subtle.ConstantTimeCompare([]byte(pass), []byte(cfg.pass)) == 1
	return userOK && passOK
}

// clientKey identifies a caller for throttling. Port is stripped so an
// attacker cannot reset their budget by opening a new connection.
func clientKey(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// sessionIDFromRequest reads the session cookie, if present.
func sessionIDFromRequest(r *http.Request) string {
	c, err := r.Cookie(sessionCookieName)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(c.Value)
}

// setSessionCookie writes the session cookie.
//
// Secure is unconditional: the gateway only ever serves TLS, and a cookie that
// could be sent in the clear is one downgrade away from being handed over.
// SameSite=Lax rather than Strict — Strict withholds the cookie on a top-level
// navigation from another origin, so following a link to the browser page
// would bounce you to the login form despite an active session. Lax still
// blocks cross-site POSTs, which is what matters for /api/navigate.
func setSessionCookie(w http.ResponseWriter, id string, ttl time.Duration) {
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookieName,
		Value:    id,
		Path:     "/",
		HttpOnly: true,
		Secure:   true,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   int(ttl.Seconds()),
	})
}

func clearSessionCookie(w http.ResponseWriter) {
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookieName,
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   true,
		SameSite: http.SameSiteLaxMode,
		MaxAge:   -1,
	})
}

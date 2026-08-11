// Package main — config.go
//
// Startup configuration for the standalone gateway, read entirely from the
// environment. Mirrors infra/turn-issuer's shape: a loadConfig that returns an
// error, and a main that os.Exit(2)s on it. Nothing here has a "safe" default
// that silently weakens the deployment — a missing credential is a hard stop,
// not a warning.
//
// Why this service exists at all: before it, running chromeless outside
// Kubernetes meant publishing the signaling broker and Chromium's DevTools
// port straight onto the host, with no TLS and no authentication of any kind
// (signaling/auth.go logs "auth disabled ... any caller can connect" when
// CHROMELESS_AUTH_PUBKEY is unset, which was every compose deployment). The
// gateway is the single host-facing port that terminates TLS, holds the login,
// and keeps the broker and DevTools internal.

package main

import (
	"fmt"
	"os"
	"strings"
	"time"
)

const (
	envUser        = "CHROMELESS_USER"
	envPass        = "CHROMELESS_PASS"
	envPort        = "CHROMELESS_PORT"
	envTLSCert     = "CHROMELESS_TLS_CERT"
	envTLSKey      = "CHROMELESS_TLS_KEY"
	envTLSDir      = "CHROMELESS_TLS_DIR"
	envTLSHosts    = "CHROMELESS_TLS_HOSTS"
	envSignalingURL = "CHROMELESS_SIGNALING_URL"
	envCDPURL      = "CHROMELESS_CDP_URL"
	envStaticDir   = "CHROMELESS_STATIC_DIR"
	envSessionTTL  = "CHROMELESS_SESSION_TTL"

	defaultPort       = "8443"
	defaultTLSDir     = "/data/certs"
	defaultStaticDir  = "/srv/client"
	defaultSignaling  = "http://signaling:8080"
	defaultCDP        = "http://chromium:9222"
	defaultSessionTTL = 12 * time.Hour
)

type config struct {
	user string
	pass string

	// addr is the ":port" the TLS listener binds. Only the port is
	// configurable: the gateway is meant to be the one host-facing surface,
	// so binding it to a single interface is the container runtime's job.
	addr string

	// certFile/keyFile point at operator-supplied PEMs. Both empty means
	// "generate a self-signed pair into tlsDir and reuse it".
	certFile string
	keyFile  string
	tlsDir   string
	tlsHosts []string

	// signalingURL is the broker's base, as an http(s) URL even though the
	// traffic is mostly WebSocket — httputil.ReverseProxy wants a URL it can
	// dial, and the ws:// scheme is not one. The scheme is normalised in
	// loadConfig so operators can write either.
	signalingURL string
	cdpURL       string

	staticDir  string
	sessionTTL time.Duration
}

func loadConfig() (*config, error) {
	user := strings.TrimSpace(os.Getenv(envUser))
	pass := os.Getenv(envPass) // NOT trimmed: spaces are legal in a password
	if user == "" {
		return nil, fmt.Errorf("%s is required", envUser)
	}
	if pass == "" {
		return nil, fmt.Errorf("%s is required", envPass)
	}

	c := &config{
		user:         user,
		pass:         pass,
		addr:         ":" + envOr(envPort, defaultPort),
		certFile:     strings.TrimSpace(os.Getenv(envTLSCert)),
		keyFile:      strings.TrimSpace(os.Getenv(envTLSKey)),
		tlsDir:       envOr(envTLSDir, defaultTLSDir),
		tlsHosts:     splitCSV(os.Getenv(envTLSHosts)),
		signalingURL: envOr(envSignalingURL, defaultSignaling),
		cdpURL:       envOr(envCDPURL, defaultCDP),
		staticDir:    envOr(envStaticDir, defaultStaticDir),
		sessionTTL:   defaultSessionTTL,
	}

	// Half a TLS pair is a misconfiguration, not a fallback. Silently
	// generating a self-signed cert because the key path had a typo would
	// hand the operator a working-looking server with the wrong identity.
	if (c.certFile == "") != (c.keyFile == "") {
		return nil, fmt.Errorf("%s and %s must be set together (got cert=%q key=%q)",
			envTLSCert, envTLSKey, c.certFile, c.keyFile)
	}

	// ws:// and wss:// are what an operator naturally writes for a signaling
	// endpoint, and what the rest of this repo's env vars use. ReverseProxy
	// needs http(s). Accept both spellings rather than making the distinction
	// the operator's problem.
	c.signalingURL = wsToHTTP(c.signalingURL)

	if raw := strings.TrimSpace(os.Getenv(envSessionTTL)); raw != "" {
		d, err := time.ParseDuration(raw)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", envSessionTTL, err)
		}
		if d <= 0 {
			return nil, fmt.Errorf("%s must be positive, got %s", envSessionTTL, d)
		}
		c.sessionTTL = d
	}

	return c, nil
}

// wsToHTTP rewrites a ws(s):// scheme to http(s)://, leaving anything else
// alone. Case-insensitive because URL schemes are.
func wsToHTTP(u string) string {
	switch {
	case strings.HasPrefix(strings.ToLower(u), "wss://"):
		return "https://" + u[len("wss://"):]
	case strings.HasPrefix(strings.ToLower(u), "ws://"):
		return "http://" + u[len("ws://"):]
	}
	return u
}

func envOr(k, fallback string) string {
	if v, ok := os.LookupEnv(k); ok && strings.TrimSpace(v) != "" {
		return strings.TrimSpace(v)
	}
	return fallback
}

func splitCSV(s string) []string {
	if strings.TrimSpace(s) == "" {
		return nil
	}
	parts := strings.Split(s, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

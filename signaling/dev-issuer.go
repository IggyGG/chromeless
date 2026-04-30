// Package main — dev-issuer.go
//
// Dev-only token issuer. Signs Ed25519 JWTs with an in-memory keypair
// generated at startup. **Never run this in production** — it has no
// authentication of its own and will hand out a token to anyone who
// asks.
//
// Activation:
//
//	CBWRTC_DEV_ISSUER=1   enables /issue-token + publishes the
//	                      generated public key in CBWRTC_AUTH_PUBKEY.
//
// When `CBWRTC_DEV_ISSUER=1` and `CBWRTC_AUTH_PUBKEY` is unset, this
// file generates a fresh keypair on startup, hands the public half to
// initAuth() through the env, and serves /issue-token using the
// private half. This is the path the integration test uses.
//
// If `CBWRTC_AUTH_PUBKEY` is already set, the dev issuer refuses to
// start — that combination only happens by accident in production
// configs and we want it to fail loudly.

package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"log/slog"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// newJTI returns a fresh, opaque token identifier (T89). 16 random bytes
// → 32 hex chars. Collisions are statistically impossible across the
// token-issuance volumes we expect; the denylist treats jti as opaque.
func newJTI() (string, error) {
	var buf [16]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf[:]), nil
}

const (
	devIssuerEnv = "CBWRTC_DEV_ISSUER"
	devTokenTTL  = 1 * time.Hour
)

type devIssuer struct {
	mu     sync.Mutex
	priv   ed25519.PrivateKey
	pub    ed25519.PublicKey
	logger *slog.Logger
}

var globalDevIssuer *devIssuer

// initDevIssuer wires up the dev-only issuer if `CBWRTC_DEV_ISSUER=1`.
// Must be called BEFORE initAuth so the env var it sets is visible.
func initDevIssuer(logger *slog.Logger) {
	if os.Getenv(devIssuerEnv) != "1" {
		return
	}
	if existing := os.Getenv(authPubkeyEnv); existing != "" {
		logger.Error("dev issuer refusing to start: " + authPubkeyEnv +
			" is already set; remove it or unset " + devIssuerEnv)
		os.Exit(2)
	}
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		logger.Error("dev issuer keygen failed", slog.Any("err", err))
		os.Exit(2)
	}
	if err := os.Setenv(authPubkeyEnv, base64.StdEncoding.EncodeToString(pub)); err != nil {
		logger.Error("dev issuer setenv failed", slog.Any("err", err))
		os.Exit(2)
	}
	globalDevIssuer = &devIssuer{priv: priv, pub: pub, logger: logger}
	logger.Warn("DEV issuer enabled — never run this in production")
}

// devIssuerHandler is the http.Handler installed at /issue-token when
// the dev issuer is active. Returns 404 otherwise so production
// deployments don't accidentally expose it.
func devIssuerHandler(w http.ResponseWriter, r *http.Request) {
	if globalDevIssuer == nil {
		http.NotFound(w, r)
		return
	}
	q := r.URL.Query()
	role := strings.TrimSpace(q.Get("role"))
	sid := strings.TrimSpace(q.Get("session_id"))
	tenant := strings.TrimSpace(q.Get("tenant"))
	if tenant == "" {
		tenant = "dev"
	}
	if role != "client" && role != "browser" {
		http.Error(w, `role must be "client" or "browser"`, http.StatusBadRequest)
		return
	}
	if sid == "" {
		http.Error(w, "session_id required", http.StatusBadRequest)
		return
	}
	now := time.Now()
	jti, err := newJTI()
	if err != nil {
		http.Error(w, "jti gen: "+err.Error(), http.StatusInternalServerError)
		return
	}
	c := Claims{
		Sub: tenant,
		Sid: sid,
		Role: role,
		Iat: now.Unix(),
		Nbf: now.Add(-30 * time.Second).Unix(),
		Exp: now.Add(devTokenTTL).Unix(),
		Jti: jti,
	}
	tok := signToken(globalDevIssuer.priv, c)
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"token": tok,
		"exp":   c.Exp,
		"role":  role,
		"sid":   sid,
		"sub":   tenant,
	})
}

// Package main — auth.go
//
// T48: Phase-3 prep — verify Ed25519-signed session tokens before
// upgrading the websocket. Tokens are JWT-shaped (header.payload.sig)
// using `alg=EdDSA`, signed with an Ed25519 keypair.
//
// Verifier key is loaded once from `CBWRTC_AUTH_PUBKEY` (base64-encoded
// raw 32-byte Ed25519 public key). If the env var is unset or empty,
// auth is **disabled** with a single startup warning — this is the
// current Phase-0/1 behaviour and must remain a deliberate, observable
// opt-out.
//
// Token claims:
//
//	sub   tenant id (string)
//	sid   session id, must equal the path segment on /ws/{sid}
//	role  "client" | "browser", must equal the `from` of every envelope
//	exp   unix-seconds expiry; tokens past exp are rejected
//	iat   issued-at (informational)
//	nbf   not-before; tokens with nbf > now are rejected
//
// Threat model: this protects against random session-id guessing and
// role spoofing. It does NOT protect against compromised tokens
// (anyone holding the token can connect until exp). Sub-token
// revocation, per-tenant key separation, and refresh land in Phase 3+.
//
// Token transport: the browser cannot set custom headers on a WS
// upgrade, so we accept the token in the `?token=...` query string.
// (Alternatives — `Sec-WebSocket-Protocol`, first-frame embedding —
// were considered; query-string is the simplest path that works in
// every WebSocket implementation we care about.)
//
// See docs/security/auth.md for the threat model in detail and the
// Phase-3+ roadmap.

package main

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"strings"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
)

// Claims is the JWT payload we expect in a session token.
type Claims struct {
	Sub  string `json:"sub"`
	Sid  string `json:"sid"`
	Role string `json:"role"`
	Exp  int64  `json:"exp"`
	Iat  int64  `json:"iat"`
	Nbf  int64  `json:"nbf,omitempty"`
	// Jti (T89) — opaque token identifier used by the denylist for
	// per-token revocation. Optional: pre-T89 tokens omit it; the
	// denylist still works as tenant-wide.
	Jti  string `json:"jti,omitempty"`
}

// authConfig is mutated only by initAuth(); read-only after init.
type authConfig struct {
	enabled bool
	pubKey  ed25519.PublicKey
}

var globalAuth authConfig

// timeNow is a package-level indirection so tests can fake the clock.
var timeNow = time.Now

// authPubkeyEnv is the environment variable consulted to enable auth.
const authPubkeyEnv = "CBWRTC_AUTH_PUBKEY"

// mAuthFailures is exported via /metrics. Each rejection records one
// increment with a low-cardinality `reason` label.
var mAuthFailures = promauto.NewCounterVec(prometheus.CounterOpts{
	Name: "cb_signaling_auth_failures_total",
	Help: "Total auth failures, by reason.",
}, []string{"reason"})

// initAuth reads the verifier key from the environment exactly once.
// Idempotent; safe to call from main and from tests via resetAuthForTest.
func initAuth(logger *slog.Logger) {
	raw := os.Getenv(authPubkeyEnv)
	if raw == "" {
		globalAuth = authConfig{enabled: false}
		logger.Warn("auth disabled: " + authPubkeyEnv + " is unset; any caller can connect")
		return
	}
	decoded, err := base64.StdEncoding.DecodeString(strings.TrimSpace(raw))
	if err != nil {
		logger.Error("auth disabled: failed to base64-decode "+authPubkeyEnv,
			slog.Any("err", err))
		globalAuth = authConfig{enabled: false}
		return
	}
	if len(decoded) != ed25519.PublicKeySize {
		logger.Error("auth disabled: pubkey wrong length",
			slog.Int("len", len(decoded)),
			slog.Int("expected", ed25519.PublicKeySize))
		globalAuth = authConfig{enabled: false}
		return
	}
	globalAuth = authConfig{enabled: true, pubKey: ed25519.PublicKey(decoded)}
	logger.Info("auth enabled (Ed25519)")
	// Pre-register label combos so /metrics is well-formed at boot.
	for _, r := range []string{"missing", "malformed", "bad_signature", "expired", "not_yet_valid", "sid_mismatch", "role_mismatch", "tenant_missing", "revoked"} {
		mAuthFailures.WithLabelValues(r)
	}
}

// authEnabled reports whether token verification is active.
func authEnabled() bool { return globalAuth.enabled }

// verifyToken parses and verifies a session token, returning the
// claims if valid. `expectedSid` is the path segment from the URL;
// `expectedRole` is the role the caller will assume.
//
// Errors are categorised by the metric label they record. The error
// message is intentionally terse so it can also be sent in close-frame
// reason (which is capped at 123 bytes).
func verifyToken(token, expectedSid, expectedRole string) (*Claims, error) {
	if token == "" {
		mAuthFailures.WithLabelValues("missing").Inc()
		return nil, errors.New("missing token")
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		mAuthFailures.WithLabelValues("malformed").Inc()
		return nil, errors.New("malformed token")
	}
	signingInput := []byte(parts[0] + "." + parts[1])
	sig, err := base64URLDecode(parts[2])
	if err != nil || len(sig) != ed25519.SignatureSize {
		mAuthFailures.WithLabelValues("malformed").Inc()
		return nil, errors.New("malformed signature")
	}
	if !ed25519.Verify(globalAuth.pubKey, signingInput, sig) {
		mAuthFailures.WithLabelValues("bad_signature").Inc()
		return nil, errors.New("signature mismatch")
	}
	payloadBytes, err := base64URLDecode(parts[1])
	if err != nil {
		mAuthFailures.WithLabelValues("malformed").Inc()
		return nil, errors.New("malformed payload")
	}
	var c Claims
	if err := json.Unmarshal(payloadBytes, &c); err != nil {
		mAuthFailures.WithLabelValues("malformed").Inc()
		return nil, errors.New("malformed claims")
	}
	if c.Sub == "" {
		mAuthFailures.WithLabelValues("tenant_missing").Inc()
		return nil, errors.New("tenant claim missing")
	}
	now := timeNow().Unix()
	if c.Exp != 0 && now >= c.Exp {
		mAuthFailures.WithLabelValues("expired").Inc()
		return nil, errors.New("token expired")
	}
	if c.Nbf != 0 && now < c.Nbf {
		mAuthFailures.WithLabelValues("not_yet_valid").Inc()
		return nil, errors.New("token not yet valid")
	}
	if c.Sid != expectedSid {
		mAuthFailures.WithLabelValues("sid_mismatch").Inc()
		return nil, errors.New("sid mismatch")
	}
	if c.Role != expectedRole {
		mAuthFailures.WithLabelValues("role_mismatch").Inc()
		return nil, fmt.Errorf("role mismatch: token=%q caller=%q", c.Role, expectedRole)
	}
	return &c, nil
}

// checkRevoked is called by wsHandler after verifyToken succeeds. It
// consults the global denylist for either a tenant-wide ban or a
// (tenant, jti) ban on this specific token.
//
// Failure mode: if the denylist backend errors (e.g., Redis
// unreachable), we fail OPEN (return nil) and let the connection
// proceed. The denylist is a defence-in-depth layer on top of token
// expiry; refusing connects on every Redis hiccup would cause more
// outages than it prevents. The error is logged separately so an
// operator can spot misbehaving infrastructure.
func checkRevoked(ctx context.Context, c *Claims, log *slog.Logger) (revoked bool, err error) {
	if c == nil || globalDenylist == nil {
		return false, nil
	}
	hit, err := globalDenylist.Contains(ctx, c.Sub, c.Jti)
	if err != nil {
		log.Warn("denylist lookup failed; failing open",
			slog.String("tenant", c.Sub),
			slog.String("jti", c.Jti),
			slog.Any("err", err))
		return false, err
	}
	if hit {
		mAuthFailures.WithLabelValues("revoked").Inc()
		return true, nil
	}
	return false, nil
}

// recordRoleMismatch is called when a subsequent envelope's `from`
// disagrees with the token's role. The first-envelope role check is
// already covered by verifyToken via the role passed in.
func recordRoleMismatch() {
	mAuthFailures.WithLabelValues("role_mismatch").Inc()
}

// base64URLDecode handles both raw and padded forms.
func base64URLDecode(s string) ([]byte, error) {
	if rem := len(s) % 4; rem != 0 {
		s += strings.Repeat("=", 4-rem)
	}
	return base64.URLEncoding.DecodeString(s)
}

// signTokenForTesting produces a signed JWT-style token for use from
// tests and the dev issuer. It deliberately lives in the same package
// so tests don't need to duplicate header/payload encoding logic.
//
// Production code MUST NOT call this with a hardcoded key; the dev
// issuer (signaling/dev-issuer.go) gates it behind an env var.
func signToken(priv ed25519.PrivateKey, c Claims) string {
	header := []byte(`{"alg":"EdDSA","typ":"JWT"}`)
	payload, _ := json.Marshal(c)
	headerB64 := base64URLEncode(header)
	payloadB64 := base64URLEncode(payload)
	signingInput := headerB64 + "." + payloadB64
	sig := ed25519.Sign(priv, []byte(signingInput))
	return signingInput + "." + base64URLEncode(sig)
}

func base64URLEncode(b []byte) string {
	return strings.TrimRight(base64.URLEncoding.EncodeToString(b), "=")
}

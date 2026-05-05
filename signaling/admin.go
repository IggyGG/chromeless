// Package main — admin.go
//
// T89: small admin HTTP API for revoking session tokens.
//
//	POST /admin/revoke
//	Authorization: Bearer <admin-token>
//	{ "tenant": "alice", "jti": "abc123" (optional), "reason": "..." }
//
// The admin token is signed by a SEPARATE Ed25519 keypair from the
// session-token keypair (T48). Public verification key in
// `CHROMELESS_ADMIN_PUBKEY` (base64-encoded raw 32 bytes). Tokens carry
// the same JWT-shape as session tokens but with an explicit
// `role: "admin"` claim that is not accepted by /ws/.
//
// When `CHROMELESS_ADMIN_PUBKEY` is unset, the admin endpoint refuses to
// register the route at all — the failure mode for "I forgot to
// configure auth" should be 404, not "anonymous revoke." Likewise
// when no denylist is configured (initDenylist returned a static
// instance with no env seed), revokes still apply but only to the
// local process.

package main

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"strings"
)

const adminPubkeyEnv = "CHROMELESS_ADMIN_PUBKEY"

type adminConfig struct {
	enabled bool
	pubKey  ed25519.PublicKey
}

var globalAdmin adminConfig

func initAdmin(logger *slog.Logger) {
	raw := strings.TrimSpace(os.Getenv(adminPubkeyEnv))
	if raw == "" {
		globalAdmin = adminConfig{enabled: false}
		logger.Info("admin endpoint disabled: " + adminPubkeyEnv + " is unset")
		return
	}
	decoded, err := base64.StdEncoding.DecodeString(raw)
	if err != nil {
		logger.Error("admin endpoint disabled: failed to base64-decode "+adminPubkeyEnv, slog.Any("err", err))
		globalAdmin = adminConfig{enabled: false}
		return
	}
	if len(decoded) != ed25519.PublicKeySize {
		logger.Error("admin endpoint disabled: pubkey wrong length",
			slog.Int("len", len(decoded)))
		globalAdmin = adminConfig{enabled: false}
		return
	}
	globalAdmin = adminConfig{enabled: true, pubKey: ed25519.PublicKey(decoded)}
	logger.Info("admin endpoint enabled (Ed25519)")
}

func adminEnabled() bool { return globalAdmin.enabled }

// verifyAdminToken validates an admin-scoped JWT and returns the
// claims. Different keypair from session tokens; role MUST be "admin".
func verifyAdminToken(token string) (*Claims, error) {
	if !globalAdmin.enabled {
		return nil, errors.New("admin endpoint disabled")
	}
	if token == "" {
		return nil, errors.New("missing admin token")
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed admin token")
	}
	signingInput := []byte(parts[0] + "." + parts[1])
	sig, err := base64URLDecode(parts[2])
	if err != nil || len(sig) != ed25519.SignatureSize {
		return nil, errors.New("malformed admin signature")
	}
	if !ed25519.Verify(globalAdmin.pubKey, signingInput, sig) {
		return nil, errors.New("admin signature mismatch")
	}
	payloadBytes, err := base64URLDecode(parts[1])
	if err != nil {
		return nil, errors.New("malformed admin payload")
	}
	var c Claims
	if err := json.Unmarshal(payloadBytes, &c); err != nil {
		return nil, errors.New("malformed admin claims")
	}
	now := timeNow().Unix()
	if c.Exp != 0 && now >= c.Exp {
		return nil, errors.New("admin token expired")
	}
	if c.Nbf != 0 && now < c.Nbf {
		return nil, errors.New("admin token not yet valid")
	}
	if c.Role != "admin" {
		return nil, fmt.Errorf("admin role required, got %q", c.Role)
	}
	return &c, nil
}

type revokeRequest struct {
	Tenant string `json:"tenant"`
	Jti    string `json:"jti,omitempty"`
	Reason string `json:"reason,omitempty"`
}

type revokeResponse struct {
	OK     bool   `json:"ok"`
	Tenant string `json:"tenant"`
	Jti    string `json:"jti,omitempty"`
	Scope  string `json:"scope"` // "tenant" or "jti"
}

// adminRevokeHandler returns the http.Handler for POST /admin/revoke.
// It writes through to whichever denylist the process initialised.
func adminRevokeHandler(deny adminDenylist, logger *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		// Bearer-auth header.
		auth := r.Header.Get("Authorization")
		if !strings.HasPrefix(auth, "Bearer ") {
			http.Error(w, "missing bearer token", http.StatusUnauthorized)
			return
		}
		token := strings.TrimSpace(strings.TrimPrefix(auth, "Bearer "))
		if _, err := verifyAdminToken(token); err != nil {
			logger.Warn("admin revoke rejected", slog.Any("err", err), slog.String("remote", r.RemoteAddr))
			http.Error(w, "unauthorized: "+err.Error(), http.StatusUnauthorized)
			return
		}

		// Body.
		var req revokeRequest
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, "decode: "+err.Error(), http.StatusBadRequest)
			return
		}
		req.Tenant = strings.TrimSpace(req.Tenant)
		req.Jti = strings.TrimSpace(req.Jti)
		if req.Tenant == "" {
			http.Error(w, "tenant required", http.StatusBadRequest)
			return
		}
		var (
			scope string
			err   error
		)
		if req.Jti == "" {
			err = deny.AddTenant(r.Context(), req.Tenant, req.Reason)
			scope = "tenant"
		} else {
			err = deny.AddJTI(r.Context(), req.Tenant, req.Jti, req.Reason)
			scope = "jti"
		}
		if err != nil {
			logger.Error("denylist write failed", slog.Any("err", err),
				slog.String("scope", scope),
				slog.String("tenant", req.Tenant))
			http.Error(w, "store: "+err.Error(), http.StatusInternalServerError)
			return
		}
		logger.Info("admin revoke",
			slog.String("scope", scope),
			slog.String("tenant", req.Tenant),
			slog.String("jti", req.Jti),
			slog.String("reason", req.Reason),
			slog.String("remote", r.RemoteAddr),
		)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(revokeResponse{OK: true, Tenant: req.Tenant, Jti: req.Jti, Scope: scope})
	}
}

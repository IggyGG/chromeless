// Package workertoken mints the long-lived browser-role token a standalone
// worker authenticates with.
//
// It exists so that `cmd/keygen` and `cmd/worker-token` cannot drift. Both
// mint the same kind of token with the same key, and a divergence between them
// would present as the failure this package was extracted to fix: a token that
// parses, verifies, and is then refused for a claim nobody thought to compare.
// signaling/token's Claims is aliased rather than copied for exactly this
// reason; this is the same argument one level up.
//
// WHY THE TOKEN IS LONG-LIVED, when the gateway's own /issue-token is not:
// the gateway mints 15-minute tokens so that ceasing to issue is an effective
// revocation, and client/src/auth.ts rolls them over. A worker cannot do that.
// The embedder reads WEBRTC_SIGNALING_TOKEN once at launch and
// capture/signaling/cb_signaling_reconnect.h is explicit that token refresh is
// out of scope — a worker whose token expires exits and expects an
// orchestrator to restart it with a fresh one.
package workertoken

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/iggy/chromeless/signaling/token"
)

// TTL is the lifetime of the DEPLOYMENT, not of a user session. See the
// package comment for why a worker cannot use a short one.
const TTL = 30 * 24 * time.Hour

// Tenant matches what the gateway's issuer stamps on client tokens. Both peers
// land in the same broker namespace only if these agree.
const Tenant = "standalone"

// ParsePrivKey decodes a base64 Ed25519 private key, rejecting the 32-byte
// seed. That mistake yields a key that signs happily and produces signatures
// nothing can verify.
func ParsePrivKey(b64 string) (ed25519.PrivateKey, error) {
	raw, err := base64.StdEncoding.DecodeString(strings.TrimSpace(b64))
	if err != nil {
		return nil, fmt.Errorf("not valid base64: %w", err)
	}
	if len(raw) != ed25519.PrivateKeySize {
		return nil, fmt.Errorf(
			"want a %d-byte Ed25519 private key, got %d bytes "+
				"(the full key, not the 32-byte seed)",
			ed25519.PrivateKeySize, len(raw))
	}
	return ed25519.PrivateKey(raw), nil
}

// Mint returns a signed browser-role token for sessionID.
func Mint(priv ed25519.PrivateKey, sessionID string) (string, error) {
	if len(priv) != ed25519.PrivateKeySize {
		return "", errors.New("private key is not an Ed25519 private key")
	}
	if strings.TrimSpace(sessionID) == "" {
		return "", errors.New("session id is required and must match the worker's")
	}

	var jti [16]byte
	if _, err := rand.Read(jti[:]); err != nil {
		return "", fmt.Errorf("jti: %w", err)
	}

	now := time.Now()
	return token.Sign(priv, token.Claims{
		Sub:  Tenant,
		Sid:  strings.TrimSpace(sessionID),
		Role: "browser",
		Iat:  now.Unix(),
		// Backdated: the worker and the broker are different machines in a
		// split-host deployment, and a token minted "in the future" by a few
		// seconds of clock skew is rejected outright.
		Nbf: now.Add(-30 * time.Second).Unix(),
		Exp: now.Add(TTL).Unix(),
		Jti: hex.EncodeToString(jti[:]),
	}), nil
}

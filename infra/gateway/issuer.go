// Package main — issuer.go
//
// Session-token minting. The gateway generates an Ed25519 keypair at startup,
// serves `/issue-token` to logged-in callers, and prints the public half so
// the broker can be started with it as CHROMELESS_AUTH_PUBKEY.
//
// This is what turns the login from a door on the page into actual protection.
// Without it the broker keeps logging "auth disabled: any caller can connect"
// and anyone who can reach the WebSocket bypasses the gateway entirely; the
// cookie would only be guarding the HTML.
//
// The keypair is ephemeral by design. It never touches disk, so a restart
// invalidates every outstanding token — which is the correct blast radius for
// a single-operator deployment, and removes a private key from the list of
// things an operator has to store safely. The cost is that the broker must be
// handed the new public key on each boot; compose does that by generating the
// pair up front (see CHROMELESS_AUTH_PUBKEY in compose.yaml) and passing the
// private half in, which is why CHROMELESS_AUTH_PRIVKEY exists.

package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"strings"
	"time"

	"github.com/iggy/chromeless/signaling/token"
)

// tokenTTL is deliberately short. client/src/auth.ts ships a TokenRefresher
// for exactly this shape, and a short window means "stop issuing" is an
// effective revocation without a denylist.
const tokenTTL = 15 * time.Minute

// tenant is the `sub` claim. The broker rejects a token with no tenant, and a
// standalone deployment has exactly one.
const tenant = "standalone"

type issuer struct {
	priv ed25519.PrivateKey
	pub  ed25519.PublicKey
}

// newIssuer loads the signing key from the environment, or generates one.
//
// Supplying the key matters for the compose case: the broker needs the PUBLIC
// half at ITS startup, and it cannot ask a service that has not booted yet.
// Generating the pair outside and passing each half to the service that needs
// it is the only ordering that works without a shared volume or a restart.
func newIssuer(privB64 string) (*issuer, error) {
	if privB64 == "" {
		pub, priv, err := ed25519.GenerateKey(rand.Reader)
		if err != nil {
			return nil, fmt.Errorf("generate signing key: %w", err)
		}
		return &issuer{priv: priv, pub: pub}, nil
	}

	// Try hex FIRST, and only when the string is entirely hex digits of the
	// right length. The two encodings genuinely collide: a 64-byte key is 128
	// hex characters, and a 128-character hex string is also valid base64 —
	// it just decodes to 96 meaningless bytes. Trying base64 first therefore
	// "succeeds" on every hex key and rejects it for being the wrong length,
	// so the hex branch is unreachable. Hex is the shape openssl prints, so
	// this is not a hypothetical input.
	trimmed := strings.TrimSpace(privB64)
	var raw []byte
	if len(trimmed) == hex.EncodedLen(ed25519.PrivateKeySize) && isHex(trimmed) {
		decoded, err := hex.DecodeString(trimmed)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", envAuthPrivkey, err)
		}
		raw = decoded
	} else {
		decoded, err := base64.StdEncoding.DecodeString(trimmed)
		if err != nil {
			return nil, fmt.Errorf("%s must be base64 or hex", envAuthPrivkey)
		}
		raw = decoded
	}

	if len(raw) != ed25519.PrivateKeySize {
		// Named explicitly because the seed (32 bytes) is the other thing an
		// operator is likely to have, and "wrong length" alone does not
		// suggest which half they pasted.
		return nil, fmt.Errorf("%s must decode to %d bytes (a full Ed25519 private key, "+
			"not the 32-byte seed), got %d", envAuthPrivkey, ed25519.PrivateKeySize, len(raw))
	}
	priv := ed25519.PrivateKey(raw)
	return &issuer{priv: priv, pub: priv.Public().(ed25519.PublicKey)}, nil
}

// pubKeyBase64 is the value to hand the broker as CHROMELESS_AUTH_PUBKEY.
func (i *issuer) pubKeyBase64() string {
	return base64.StdEncoding.EncodeToString(i.pub)
}

// checkPubkeyMatches verifies that the CHROMELESS_AUTH_PUBKEY the operator
// gave the broker really is the public half of our signing key.
//
// This is the one configuration mistake with no useful symptom. A mismatched
// pair produces tokens the broker rejects as "signature mismatch" on every
// single connection, and nothing in either log says the keys differ — the
// gateway is happily minting, the broker is happily verifying, and they simply
// disagree. Since compose hands both halves to the two services from one
// keygen run, we can just compare and refuse to start.
//
// Empty is not an error: an operator may deliberately run the broker
// anonymously, and the startup log already warns loudly about that.
func (i *issuer) checkPubkeyMatches(declared string) error {
	declared = strings.TrimSpace(declared)
	if declared == "" {
		return nil
	}
	if declared != i.pubKeyBase64() {
		return fmt.Errorf(
			"%s does not match the public half of %s — every token this gateway "+
				"mints would be rejected by signaling as a bad signature. Regenerate "+
				"both with `cd infra/gateway && go run ./cmd/keygen`",
			envAuthPubkey, envAuthPrivkey)
	}
	return nil
}

// handleIssueToken mints a token for the calling session.
//
// Shape and query parameters match signaling/dev-issuer.go's /issue-token, so
// client/src/auth.ts talks to this with no changes at all.
func (g *gateway) handleIssueToken(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	role := strings.TrimSpace(q.Get("role"))
	sid := strings.TrimSpace(q.Get("session_id"))

	// The broker checks `role` against every envelope's `from`, and only these
	// two exist on the wire (cb_wire_envelope.h).
	if role != "client" && role != "browser" {
		http.Error(w, `role must be "client" or "browser"`, http.StatusBadRequest)
		return
	}
	if sid == "" {
		http.Error(w, "session_id required", http.StatusBadRequest)
		return
	}

	jti, err := newJTI()
	if err != nil {
		g.log.Error("jti generation failed", slog.Any("err", err))
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	now := time.Now()
	claims := token.Claims{
		Sub:  tenant,
		Sid:  sid,
		Role: role,
		Iat:  now.Unix(),
		// Backdated: the browser and the broker are different machines in a
		// split-host deployment, and a token minted "in the future" by a few
		// seconds of clock skew is rejected outright.
		Nbf: now.Add(-30 * time.Second).Unix(),
		Exp: now.Add(tokenTTL).Unix(),
		Jti: jti,
	}

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(map[string]any{
		"token": token.Sign(g.issuer.priv, claims),
		"exp":   claims.Exp,
		"role":  role,
		"sid":   sid,
		"sub":   tenant,
	})
}

// isHex reports whether s is entirely hexadecimal digits.
func isHex(s string) bool {
	for _, r := range s {
		switch {
		case r >= '0' && r <= '9', r >= 'a' && r <= 'f', r >= 'A' && r <= 'F':
		default:
			return false
		}
	}
	return len(s) > 0
}

// newJTI returns an opaque token identifier for the broker's denylist.
func newJTI() (string, error) {
	var buf [16]byte
	if _, err := rand.Read(buf[:]); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf[:]), nil
}

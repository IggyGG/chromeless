// Package token holds the session-token wire format shared by everything that
// mints or verifies one.
//
// It exists because the format has three independent implementations in this
// repo — signaling/auth.go verifies, infra/turn-issuer/main.go verifies, and
// the gateway mints — and the claim names and the exact bytes of the signing
// input have to agree to the letter. They are not checked by anything at build
// time: a drifted field name produces a token that parses fine and then fails
// verification with "signature mismatch" or "tenant claim missing", minutes
// away from the code that got it wrong.
//
// turn-issuer's copy carries the comment "Mirrors signaling/auth.go's
// verification path ... If you change the JWT shape there, change it here",
// which is exactly the instruction this package removes the need for.
//
// Format: a JWT-shaped `header.payload.signature`, `alg=EdDSA`, Ed25519 over
// the ASCII `base64url(header) + "." + base64url(payload)`, all segments
// unpadded base64url. This is what signaling/auth.go has always produced and
// accepted; the encoding here is byte-identical to it, deliberately.

package token

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
)

// Claims is the JWT payload.
//
// Field tags are wire contract. Renaming one silently invalidates every token
// in flight, and the failure surfaces at the verifier as a generic rejection.
type Claims struct {
	Sub  string `json:"sub"`  // tenant id
	Sid  string `json:"sid"`  // session id; must equal the /ws/{sid} path segment
	Role string `json:"role"` // "client" | "browser"; must equal the envelope `from`
	Exp  int64  `json:"exp"`  // unix seconds; tokens at or past this are rejected
	Iat  int64  `json:"iat"`  // issued-at, informational
	Nbf  int64  `json:"nbf,omitempty"`

	// Jti (T89) — opaque token id used by the denylist for per-token
	// revocation. Optional: pre-T89 tokens omit it and the denylist still
	// works tenant-wide.
	Jti string `json:"jti,omitempty"`

	// Aud — regions this token is valid in. Optional; when present, a server
	// whose CHROMELESS_REGION is not listed rejects with close code 1008.
	//
	// Encoded as an array. RFC 7519 §4.1.3 also permits a bare string for a
	// single audience, which UnmarshalJSON accepts.
	Aud []string `json:"aud,omitempty"`
}

// UnmarshalJSON accepts the RFC 7519 §4.1.3 shorthand where a single audience
// may be a bare string rather than a one-element array. Without this, a
// spec-legal `"aud":"eu-west-1"` from a third-party issuer fails to unmarshal
// and is reported as malformed claims.
func (c *Claims) UnmarshalJSON(data []byte) error {
	type rawClaims Claims // avoid recursing into this method
	var probe struct {
		*rawClaims
		Aud json.RawMessage `json:"aud,omitempty"`
	}
	probe.rawClaims = (*rawClaims)(c)
	if err := json.Unmarshal(data, &probe); err != nil {
		return err
	}
	c.Aud = nil
	if len(probe.Aud) == 0 || string(probe.Aud) == "null" {
		return nil
	}
	var list []string
	if err := json.Unmarshal(probe.Aud, &list); err == nil {
		c.Aud = list
		return nil
	}
	var single string
	if err := json.Unmarshal(probe.Aud, &single); err != nil {
		return errors.New("aud must be a string or array of strings")
	}
	c.Aud = []string{single}
	return nil
}

// Header is the fixed JWT header. A literal rather than a marshalled struct
// because the signature covers these exact bytes: Go's map ordering or a
// future field reordering would change the signing input and invalidate
// tokens that are otherwise correct.
const Header = `{"alg":"EdDSA","typ":"JWT"}`

// Sign produces a signed token for c.
//
// Callers are responsible for the key. Production code must never call this
// with a hardcoded one — the dev issuer (signaling/dev-issuer.go) gates it
// behind an env var, and the gateway generates a fresh keypair per boot.
func Sign(priv ed25519.PrivateKey, c Claims) string {
	payload, _ := json.Marshal(c)
	signingInput := Base64URLEncode([]byte(Header)) + "." + Base64URLEncode(payload)
	sig := ed25519.Sign(priv, []byte(signingInput))
	return signingInput + "." + Base64URLEncode(sig)
}

// Parse splits and verifies a token's signature, returning its claims.
//
// It deliberately does NOT check exp/nbf/role/sid — those are policy, and each
// verifier applies its own (signaling additionally checks the region audience
// and the revocation denylist). This is the cryptographic half only.
func Parse(pub ed25519.PublicKey, tok string) (*Claims, error) {
	if tok == "" {
		return nil, errors.New("missing token")
	}
	parts := strings.Split(tok, ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed token")
	}
	sig, err := Base64URLDecode(parts[2])
	if err != nil || len(sig) != ed25519.SignatureSize {
		return nil, errors.New("malformed signature")
	}
	if !ed25519.Verify(pub, []byte(parts[0]+"."+parts[1]), sig) {
		return nil, errors.New("signature mismatch")
	}
	payload, err := Base64URLDecode(parts[1])
	if err != nil {
		return nil, errors.New("malformed payload")
	}
	var c Claims
	if err := json.Unmarshal(payload, &c); err != nil {
		return nil, errors.New("malformed claims")
	}
	return &c, nil
}

// Base64URLEncode emits the unpadded form JWT requires.
func Base64URLEncode(b []byte) string {
	return strings.TrimRight(base64.URLEncoding.EncodeToString(b), "=")
}

// Base64URLDecode accepts both the raw and padded forms. Unpadded is what JWT
// specifies and what we emit, but third-party issuers do send padded segments
// and rejecting those would be a gratuitous interop failure.
func Base64URLDecode(s string) ([]byte, error) {
	if rem := len(s) % 4; rem != 0 {
		s += strings.Repeat("=", 4-rem)
	}
	return base64.URLEncoding.DecodeString(s)
}

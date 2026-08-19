package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/iggy/chromeless/signaling/token"
)

// The response shape is consumed by client/src/auth.ts, which returns null and
// connects unauthenticated if any field is missing or mistyped — so a drift
// here surfaces as an unexplained WebSocket close, not an auth error.
func TestIssueTokenResponseShape(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodGet, "/issue-token?role=client&session_id=dev", nil)
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}

	var body struct {
		Token string `json:"token"`
		Exp   int64  `json:"exp"`
		Role  string `json:"role"`
		Sid   string `json:"sid"`
		Sub   string `json:"sub"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Token == "" {
		t.Error("no token in response")
	}
	if body.Role != "client" || body.Sid != "dev" {
		t.Errorf("role/sid = %q/%q, want client/dev", body.Role, body.Sid)
	}
	if body.Sub == "" {
		t.Error("empty sub: the broker rejects a token with no tenant claim")
	}
	if body.Exp <= time.Now().Unix() {
		t.Error("token is already expired")
	}
}

// THE test for this step. The gateway mints and the broker verifies, in two
// different modules; if the claim names or the signing input drift, every
// connection fails at the broker with a generic rejection. Parsing with the
// shared package is what proves they agree.
func TestMintedTokenVerifiesWithBrokerFormat(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodGet, "/issue-token?role=client&session_id=my-session", nil)
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	var body struct {
		Token string `json:"token"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode: %v", err)
	}

	claims, err := token.Parse(g.issuer.pub, body.Token)
	if err != nil {
		t.Fatalf("broker-side parse rejected a gateway-minted token: %v", err)
	}

	// Each of these is separately fatal at the broker: a missing Sub is
	// "tenant claim missing"; a mismatched Sid is "sid_mismatch" against the
	// /ws/{sid} path segment; a mismatched Role is "role_mismatch" against
	// the envelope's `from`.
	if claims.Sub == "" {
		t.Error("Sub empty")
	}
	if claims.Sid != "my-session" {
		t.Errorf("Sid = %q, want my-session", claims.Sid)
	}
	if claims.Role != "client" {
		t.Errorf("Role = %q, want client", claims.Role)
	}

	now := time.Now().Unix()
	if claims.Exp <= now {
		t.Error("Exp is in the past")
	}
	// Nbf is backdated on purpose: in a split-host deployment a few seconds of
	// clock skew would otherwise make a fresh token "not yet valid".
	if claims.Nbf > now {
		t.Errorf("Nbf = %d is in the future (now %d)", claims.Nbf, now)
	}
	if claims.Jti == "" {
		t.Error("Jti empty: the broker's denylist keys per-token revocation on it")
	}
}

func TestIssueTokenRequiresSession(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())

	req := httptest.NewRequest(http.MethodGet, "/issue-token?role=client&session_id=dev", nil)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code == http.StatusOK {
		t.Fatal("minted a token without a session — the login would be decorative")
	}
}

func TestIssueTokenValidatesParams(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")

	for _, tc := range []struct{ name, query string }{
		// Only these two roles exist on the wire (cb_wire_envelope.h), and the
		// broker checks the claim against every envelope's `from`.
		{"bad role", "?role=admin&session_id=dev"},
		{"missing role", "?session_id=dev"},
		{"missing session_id", "?role=client"},
		{"blank session_id", "?role=client&session_id=%20"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodGet, "/issue-token"+tc.query, nil)
			req.AddCookie(c)
			rec := httptest.NewRecorder()
			g.routes().ServeHTTP(rec, req)
			if rec.Code != http.StatusBadRequest {
				t.Errorf("status = %d, want 400", rec.Code)
			}
		})
	}
}

// The browser role must be issuable too: a worker on another host authenticates
// through the same gateway.
func TestIssueTokenAcceptsBrowserRole(t *testing.T) {
	g, _ := newTestGateway(t, http.NotFoundHandler())
	c := login(t, g, "operator", "s3cret")

	req := httptest.NewRequest(http.MethodGet, "/issue-token?role=browser&session_id=dev", nil)
	req.AddCookie(c)
	rec := httptest.NewRecorder()
	g.routes().ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
}

func TestNewIssuerAcceptsSuppliedKey(t *testing.T) {
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}

	// Both encodings an operator plausibly produces. Silently failing to parse
	// one would present as "every token is rejected" at the broker.
	for name, encoded := range map[string]string{
		"base64": base64.StdEncoding.EncodeToString(priv),
		"hex":    hex.EncodeToString(priv),
	} {
		t.Run(name, func(t *testing.T) {
			iss, err := newIssuer(encoded)
			if err != nil {
				t.Fatalf("newIssuer: %v", err)
			}
			if !iss.priv.Equal(priv) {
				t.Error("supplied key was not used")
			}
			want := base64.StdEncoding.EncodeToString(priv.Public().(ed25519.PublicKey))
			if iss.pubKeyBase64() != want {
				t.Errorf("pubKeyBase64 = %q, want %q", iss.pubKeyBase64(), want)
			}
		})
	}
}

// The two encodings collide, and the naive order gets it wrong: a 64-byte key
// is 128 hex characters, and a 128-character hex string is ALSO valid base64 —
// it just decodes to 96 meaningless bytes. Trying base64 first therefore
// "succeeds" on every hex key and then rejects it for being the wrong length,
// making the hex branch unreachable. Caught by the test above; this pins it.
func TestHexKeyIsNotMisreadAsBase64(t *testing.T) {
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	encoded := hex.EncodeToString(priv)

	if _, err := base64.StdEncoding.DecodeString(encoded); err != nil {
		t.Skip("this hex string is not also valid base64; the collision cannot occur")
	}

	iss, err := newIssuer(encoded)
	if err != nil {
		t.Fatalf("hex key rejected: %v", err)
	}
	if !iss.priv.Equal(priv) {
		t.Error("hex key decoded to the wrong bytes")
	}
}

// A malformed or truncated key must fail at startup. Falling back to a
// generated one would leave the broker verifying against a public key nobody
// holds the private half of — every connection rejected, no clue why.
func TestNewIssuerRejectsBadKey(t *testing.T) {
	for _, bad := range []string{"not-base64-or-hex!!", "c2hvcnQ="} {
		if _, err := newIssuer(bad); err == nil {
			t.Errorf("accepted bad key %q", bad)
		}
	}
}

func TestNewIssuerGeneratesWhenUnset(t *testing.T) {
	a, err := newIssuer("")
	if err != nil {
		t.Fatal(err)
	}
	b, err := newIssuer("")
	if err != nil {
		t.Fatal(err)
	}
	if a.pubKeyBase64() == b.pubKeyBase64() {
		t.Error("two generated issuers share a key")
	}
}

// A mismatched pair is the one misconfiguration with no useful symptom: the
// gateway mints happily, the broker verifies happily, they disagree, and every
// connection dies as "signature mismatch" with nothing saying the keys differ.
func TestPubkeyMismatchIsRejectedAtStartup(t *testing.T) {
	iss, err := newIssuer("")
	if err != nil {
		t.Fatal(err)
	}

	t.Run("matching pair accepted", func(t *testing.T) {
		if err := iss.checkPubkeyMatches(iss.pubKeyBase64()); err != nil {
			t.Errorf("rejected its own public key: %v", err)
		}
	})

	t.Run("whitespace tolerated", func(t *testing.T) {
		if err := iss.checkPubkeyMatches("  " + iss.pubKeyBase64() + "\n"); err != nil {
			t.Errorf("rejected a padded but correct key: %v", err)
		}
	})

	t.Run("empty accepted", func(t *testing.T) {
		// An operator may deliberately run the broker anonymously; the startup
		// log already warns about that.
		if err := iss.checkPubkeyMatches(""); err != nil {
			t.Errorf("empty pubkey should not be an error: %v", err)
		}
	})

	t.Run("mismatch rejected", func(t *testing.T) {
		other, err := newIssuer("")
		if err != nil {
			t.Fatal(err)
		}
		if err := iss.checkPubkeyMatches(other.pubKeyBase64()); err == nil {
			t.Error("accepted a public key from a different keypair")
		}
	})
}

// The check has to run during construction, or it is decorative.
func TestNewGatewayFailsOnPubkeyMismatch(t *testing.T) {
	other, err := newIssuer("")
	if err != nil {
		t.Fatal(err)
	}
	cfg := &config{
		user: "operator", pass: "s3cret",
		signalingURL: "http://127.0.0.1:1",
		staticDir:    t.TempDir(),
		sessionTTL:   time.Hour,
		authPubkey:   other.pubKeyBase64(), // privkey unset ⇒ a fresh, different key
	}
	if _, err := newGateway(cfg, quietLogger()); err == nil {
		t.Fatal("newGateway accepted a mismatched keypair")
	}
}

// A short TTL is what makes "stop issuing" an effective revocation without a
// denylist, and client/src/auth.ts ships TokenRefresher for exactly this.
func TestTokenTTLIsShort(t *testing.T) {
	if tokenTTL > time.Hour {
		t.Errorf("tokenTTL = %v; a long-lived token cannot be revoked by ceasing to issue", tokenTTL)
	}
}

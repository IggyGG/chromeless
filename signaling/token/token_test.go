package token

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"strings"
	"testing"
)

func mustKey(t *testing.T) (ed25519.PublicKey, ed25519.PrivateKey) {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return pub, priv
}

func TestSignParseRoundTrip(t *testing.T) {
	pub, priv := mustKey(t)
	want := Claims{Sub: "tenant", Sid: "sess", Role: "client", Exp: 1_700_000_000, Iat: 1_699_999_000}

	got, err := Parse(pub, Sign(priv, want))
	if err != nil {
		t.Fatalf("Parse: %v", err)
	}
	if got.Sub != want.Sub || got.Sid != want.Sid || got.Role != want.Role || got.Exp != want.Exp {
		t.Errorf("round trip lost data: %+v", got)
	}
}

// The signature covers the header bytes, so this literal is wire contract:
// re-ordering the two fields would invalidate every token in flight while
// still looking correct.
func TestHeaderIsTheExpectedLiteral(t *testing.T) {
	if Header != `{"alg":"EdDSA","typ":"JWT"}` {
		t.Fatalf("header changed: %s", Header)
	}
	_, priv := mustKey(t)
	seg := strings.Split(Sign(priv, Claims{Sub: "t"}), ".")[0]
	decoded, err := Base64URLDecode(seg)
	if err != nil {
		t.Fatal(err)
	}
	if string(decoded) != Header {
		t.Errorf("first segment = %s, want %s", decoded, Header)
	}
}

func TestTokenHasThreeUnpaddedSegments(t *testing.T) {
	_, priv := mustKey(t)
	tok := Sign(priv, Claims{Sub: "t", Sid: "s", Role: "client"})

	parts := strings.Split(tok, ".")
	if len(parts) != 3 {
		t.Fatalf("got %d segments, want 3", len(parts))
	}
	// JWT specifies unpadded base64url. A stray '=' also breaks the query-param
	// transport the client uses (client/src/auth.ts withToken).
	if strings.Contains(tok, "=") {
		t.Error("token carries base64 padding")
	}
}

func TestParseRejectsBadSignature(t *testing.T) {
	pub, priv := mustKey(t)
	otherPub, _ := mustKey(t)
	tok := Sign(priv, Claims{Sub: "t", Sid: "s", Role: "client"})

	if _, err := Parse(otherPub, tok); err == nil {
		t.Error("a token verified against the wrong public key")
	}

	// Tamper with the payload but keep the original signature: the classic
	// forgery attempt, and the one the signing input exists to defeat.
	parts := strings.Split(tok, ".")
	forged := Base64URLEncode([]byte(`{"sub":"attacker","sid":"s","role":"client"}`))
	if _, err := Parse(pub, parts[0]+"."+forged+"."+parts[2]); err == nil {
		t.Error("a tampered payload was accepted")
	}
}

func TestParseRejectsMalformed(t *testing.T) {
	pub, _ := mustKey(t)
	for _, tok := range []string{"", "one.two", "a.b.c.d", "not-a-token", "a.b.!!!"} {
		if _, err := Parse(pub, tok); err == nil {
			t.Errorf("accepted malformed token %q", tok)
		}
	}
}

// RFC 7519 §4.1.3 permits a bare string for a single audience. A third-party
// issuer emitting the spec-legal form must not be reported as malformed.
func TestAudAcceptsStringOrArray(t *testing.T) {
	cases := map[string][]string{
		`{"sub":"t","aud":"eu-west-1"}`:                  {"eu-west-1"},
		`{"sub":"t","aud":["eu-west-1","eu-central-1"]}`: {"eu-west-1", "eu-central-1"},
		`{"sub":"t"}`:            nil,
		`{"sub":"t","aud":null}`: nil,
	}
	for raw, want := range cases {
		var c Claims
		if err := json.Unmarshal([]byte(raw), &c); err != nil {
			t.Fatalf("%s: %v", raw, err)
		}
		if len(c.Aud) != len(want) {
			t.Errorf("%s: aud = %v, want %v", raw, c.Aud, want)
			continue
		}
		for i := range want {
			if c.Aud[i] != want[i] {
				t.Errorf("%s: aud = %v, want %v", raw, c.Aud, want)
			}
		}
	}

	var c Claims
	if err := json.Unmarshal([]byte(`{"sub":"t","aud":42}`), &c); err == nil {
		t.Error("a numeric aud was accepted")
	}
}

// Optional claims must stay absent from the JSON rather than serialise as
// empty values — a verifier distinguishing "no jti" from "empty jti" would
// otherwise see the wrong thing.
func TestOptionalClaimsAreOmitted(t *testing.T) {
	b, err := json.Marshal(Claims{Sub: "t", Sid: "s", Role: "client"})
	if err != nil {
		t.Fatal(err)
	}
	for _, field := range []string{"jti", "aud", "nbf"} {
		if strings.Contains(string(b), `"`+field+`"`) {
			t.Errorf("unset %q was serialised: %s", field, b)
		}
	}
	// exp/iat are NOT omitempty: a verifier treats exp==0 as "no expiry", so
	// the field must always be present and explicit.
	for _, field := range []string{"sub", "sid", "role", "exp", "iat"} {
		if !strings.Contains(string(b), `"`+field+`"`) {
			t.Errorf("required field %q missing: %s", field, b)
		}
	}
}

// Unpadded is what we emit, but third-party issuers do send padded segments
// and rejecting those would be a gratuitous interop failure.
func TestBase64URLDecodeAcceptsPadding(t *testing.T) {
	// Lengths chosen so the encoding needs 1, 2, and 0 padding characters —
	// the padded form is only reachable when len(raw)%3 != 0.
	for _, raw := range []string{"hello!", "hello", "hel"} {
		unpadded := Base64URLEncode([]byte(raw))
		padded := unpadded
		if rem := len(unpadded) % 4; rem != 0 {
			padded += strings.Repeat("=", 4-rem)
		}

		for _, in := range []string{unpadded, padded} {
			got, err := Base64URLDecode(in)
			if err != nil {
				t.Fatalf("%q: %v", in, err)
			}
			if string(got) != raw {
				t.Errorf("%q decoded to %q, want %q", in, got, raw)
			}
		}
	}
}

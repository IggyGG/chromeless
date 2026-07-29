package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/json"
	"io"
	"log/slog"
	"strings"
	"testing"
	"time"
)

// quietLogger discards output; tests that want to assert log content
// instead use a captureLogger.
func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func captureLogger(buf io.Writer) *slog.Logger {
	return slog.New(slog.NewTextHandler(buf, nil))
}

// resetAuthForTest swaps the global auth config to a deterministic
// keypair, so each test runs with a known signer + verifier.
func resetAuthForTest(t *testing.T) ed25519.PrivateKey {
	t.Helper()
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatalf("keygen: %v", err)
	}
	prev := globalAuth
	globalAuth = authConfig{enabled: true, pubKey: pub}
	t.Cleanup(func() { globalAuth = prev })
	return priv
}

func freezeTime(t *testing.T, secs int64) {
	t.Helper()
	prev := timeNow
	timeNow = func() time.Time { return time.Unix(secs, 0) }
	t.Cleanup(func() { timeNow = prev })
}

func TestInitAuth_DisabledWhenEnvUnset(t *testing.T) {
	t.Setenv(authPubkeyEnv, "")
	var sb strings.Builder
	initAuth(captureLogger(&sb))
	if authEnabled() {
		t.Fatal("authEnabled() should be false")
	}
	if !strings.Contains(sb.String(), "auth disabled") {
		t.Fatalf("expected disable warning in log, got: %q", sb.String())
	}
}

func TestInitAuth_DisabledWhenBase64Bad(t *testing.T) {
	t.Setenv(authPubkeyEnv, "not-valid-base64!!!")
	var sb strings.Builder
	initAuth(captureLogger(&sb))
	if authEnabled() {
		t.Fatal("authEnabled() should be false on bad base64")
	}
	if !strings.Contains(sb.String(), "failed to base64-decode") {
		t.Fatalf("expected decode error in log, got: %q", sb.String())
	}
}

func TestInitAuth_DisabledWhenKeyWrongLength(t *testing.T) {
	short := base64.StdEncoding.EncodeToString([]byte("too-short"))
	t.Setenv(authPubkeyEnv, short)
	var sb strings.Builder
	initAuth(captureLogger(&sb))
	if authEnabled() {
		t.Fatal("authEnabled() should be false for short key")
	}
	if !strings.Contains(sb.String(), "wrong length") {
		t.Fatalf("expected length error in log, got: %q", sb.String())
	}
}

func TestInitAuth_EnabledWithValidKey(t *testing.T) {
	pub, _, _ := ed25519.GenerateKey(rand.Reader)
	t.Setenv(authPubkeyEnv, base64.StdEncoding.EncodeToString(pub))
	var sb strings.Builder
	initAuth(captureLogger(&sb))
	if !authEnabled() {
		t.Fatal("authEnabled() should be true")
	}
	if !strings.Contains(sb.String(), "auth enabled") {
		t.Fatalf("expected enable log, got: %q", sb.String())
	}
}

func TestVerifyToken_HappyPath(t *testing.T) {
	priv := resetAuthForTest(t)
	freezeTime(t, 1_700_000_000)
	tok := signToken(priv, Claims{
		Sub: "tenant-a", Sid: "sess-1", Role: "client",
		Iat: 1_700_000_000 - 60,
		Exp: 1_700_000_000 + 60,
		Nbf: 1_700_000_000 - 60,
	})
	c, err := verifyToken(tok, "sess-1", "client")
	if err != nil {
		t.Fatalf("unexpected err: %v", err)
	}
	if c.Sub != "tenant-a" {
		t.Fatalf("sub: got %q want tenant-a", c.Sub)
	}
}

func TestVerifyToken_Rejects(t *testing.T) {
	priv := resetAuthForTest(t)
	freezeTime(t, 1_700_000_000)
	good := Claims{Sub: "t", Sid: "s", Role: "client", Exp: 1_700_000_000 + 60}

	cases := []struct {
		name      string
		tok       func() string
		sid, role string
		wantSub   string
	}{
		{
			name: "missing token",
			tok:  func() string { return "" },
			sid:  "s", role: "client", wantSub: "missing",
		},
		{
			name: "malformed (not 3 parts)",
			tok:  func() string { return "aaa.bbb" },
			sid:  "s", role: "client", wantSub: "malformed",
		},
		{
			name: "bad signature",
			tok: func() string {
				_, otherPriv, _ := ed25519.GenerateKey(rand.Reader)
				return signToken(otherPriv, good)
			},
			sid: "s", role: "client", wantSub: "signature",
		},
		{
			name: "expired",
			tok:  func() string { return signToken(priv, Claims{Sub: "t", Sid: "s", Role: "client", Exp: 1_699_999_900}) },
			sid:  "s", role: "client", wantSub: "expired",
		},
		{
			name: "not yet valid (nbf in future)",
			tok: func() string {
				return signToken(priv, Claims{Sub: "t", Sid: "s", Role: "client", Exp: 1_700_001_000, Nbf: 1_700_000_500})
			},
			sid: "s", role: "client", wantSub: "not yet valid",
		},
		{
			name: "sid mismatch",
			tok:  func() string { return signToken(priv, good) },
			sid:  "other-sid", role: "client", wantSub: "sid mismatch",
		},
		{
			name: "role mismatch",
			tok:  func() string { return signToken(priv, good) },
			sid:  "s", role: "browser", wantSub: "role mismatch",
		},
		{
			name: "tenant missing",
			tok:  func() string { return signToken(priv, Claims{Sid: "s", Role: "client", Exp: 1_700_001_000}) },
			sid:  "s", role: "client", wantSub: "tenant",
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := verifyToken(tc.tok(), tc.sid, tc.role)
			if err == nil {
				t.Fatalf("want error, got nil")
			}
			if !strings.Contains(err.Error(), tc.wantSub) {
				t.Fatalf("err %q does not contain %q", err.Error(), tc.wantSub)
			}
		})
	}
}

func TestSignAndVerifyRoundtrip(t *testing.T) {
	priv := resetAuthForTest(t)
	freezeTime(t, 1_700_000_000)
	for _, role := range []string{"client", "browser"} {
		tok := signToken(priv, Claims{Sub: "tenant", Sid: "sess", Role: role, Exp: 1_700_001_000})
		if _, err := verifyToken(tok, "sess", role); err != nil {
			t.Fatalf("role=%s: %v", role, err)
		}
	}
}

// ---------- T89 revocation ----------

func TestCheckRevoked_NotInDenylist(t *testing.T) {
	prev := globalDenylist
	globalDenylist = NewStaticDenylist()
	t.Cleanup(func() { globalDenylist = prev })
	revoked, err := checkRevoked(context.Background(), &Claims{Sub: "alice", Jti: "abc"}, quietLogger())
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	if revoked {
		t.Error("clean denylist should not revoke")
	}
}

func TestCheckRevoked_TenantWide(t *testing.T) {
	d := NewStaticDenylist()
	_ = d.AddTenant(context.Background(), "alice", "compromised")
	prev := globalDenylist
	globalDenylist = d
	t.Cleanup(func() { globalDenylist = prev })

	revoked, _ := checkRevoked(context.Background(), &Claims{Sub: "alice", Jti: "anyJtiAtAll"}, quietLogger())
	if !revoked {
		t.Error("tenant-wide ban should match any jti for the tenant")
	}
	revoked, _ = checkRevoked(context.Background(), &Claims{Sub: "bob", Jti: "abc"}, quietLogger())
	if revoked {
		t.Error("ban on alice should not catch bob")
	}
}

func TestCheckRevoked_PerJTI(t *testing.T) {
	d := NewStaticDenylist()
	_ = d.AddJTI(context.Background(), "alice", "stolen-token", "")
	prev := globalDenylist
	globalDenylist = d
	t.Cleanup(func() { globalDenylist = prev })

	revoked, _ := checkRevoked(context.Background(), &Claims{Sub: "alice", Jti: "stolen-token"}, quietLogger())
	if !revoked {
		t.Error("the specific jti should be blocked")
	}
	revoked, _ = checkRevoked(context.Background(), &Claims{Sub: "alice", Jti: "different"}, quietLogger())
	if revoked {
		t.Error("a different jti for the same tenant should pass")
	}
}

func TestCheckRevoked_NilClaims(t *testing.T) {
	revoked, err := checkRevoked(context.Background(), nil, quietLogger())
	if revoked || err != nil {
		t.Errorf("nil claims: revoked=%v err=%v want false/nil", revoked, err)
	}
}

// --- region scoping (aud) --------------------------------------------------
//
// `processRegion` was initialised at startup but never consulted, so an
// aud-scoped token was accepted by a server in ANY region — the claim was
// inert and the integration test asserting a 1008 close had been failing.

func TestRegionPermitted(t *testing.T) {
	cases := []struct {
		name   string
		aud    []string
		region string
		want   bool
	}{
		// Opt-in: no claim means no constraint.
		{"no aud, region set", nil, "us-east-1", true},
		{"empty aud, region set", []string{}, "us-east-1", true},
		// A server that doesn't know its own region can't enforce; failing
		// closed here would break every single-region deployment.
		{"aud set, region unset", []string{"eu-west-1"}, "", true},
		{"aud set, region unspecified", []string{"eu-west-1"}, regionUnspecified, true},
		// The actual check.
		{"match", []string{"us-east-1"}, "us-east-1", true},
		{"match among several", []string{"eu-west-1", "us-east-1"}, "us-east-1", true},
		{"mismatch", []string{"eu-west-1", "eu-central-1"}, "us-east-1", false},
		{"whitespace tolerated", []string{" us-east-1 "}, "us-east-1", true},
		// Region names are compared exactly — no prefix or case folding.
		{"case sensitive", []string{"US-EAST-1"}, "us-east-1", false},
		{"not a prefix match", []string{"us-east"}, "us-east-1", false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := regionPermitted(c.aud, c.region); got != c.want {
				t.Errorf("regionPermitted(%v, %q) = %v, want %v",
					c.aud, c.region, got, c.want)
			}
		})
	}
}

func TestVerifyToken_RegionScoping(t *testing.T) {
	priv := resetAuthForTest(t)
	freezeTime(t, 1_700_000_000)

	prevRegion := processRegion
	t.Cleanup(func() { processRegion = prevRegion })

	mint := func(aud []string) string {
		return signToken(priv, Claims{
			Sub: "tenant-a", Sid: "sess-1", Role: "client",
			Iat: 1_700_000_000 - 60,
			Exp: 1_700_000_000 + 60,
			Nbf: 1_700_000_000 - 60,
			Aud: aud,
		})
	}

	processRegion = "us-east-1"

	if _, err := verifyToken(mint([]string{"us-east-1"}), "sess-1", "client"); err != nil {
		t.Errorf("matching region should verify: %v", err)
	}
	if _, err := verifyToken(mint(nil), "sess-1", "client"); err != nil {
		t.Errorf("unscoped token should verify: %v", err)
	}
	if _, err := verifyToken(mint([]string{"eu-west-1"}), "sess-1", "client"); err == nil {
		t.Error("region mismatch should be rejected")
	}

	// An unspecified server region cannot enforce.
	processRegion = regionUnspecified
	if _, err := verifyToken(mint([]string{"eu-west-1"}), "sess-1", "client"); err != nil {
		t.Errorf("region-unspecified server should accept a scoped token: %v", err)
	}
}

// RFC 7519 §4.1.3 permits a bare string for a single audience. A
// third-party issuer emitting that shape must not be reported as malformed.
func TestClaims_AudAcceptsStringOrArray(t *testing.T) {
	cases := []struct {
		name string
		json string
		want []string
	}{
		{"array", `{"sub":"t","aud":["a","b"]}`, []string{"a", "b"}},
		{"bare string", `{"sub":"t","aud":"a"}`, []string{"a"}},
		{"absent", `{"sub":"t"}`, nil},
		{"null", `{"sub":"t","aud":null}`, nil},
		{"empty array", `{"sub":"t","aud":[]}`, nil},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			var got Claims
			if err := json.Unmarshal([]byte(c.json), &got); err != nil {
				t.Fatalf("unmarshal: %v", err)
			}
			if len(got.Aud) != len(c.want) {
				t.Fatalf("aud = %v, want %v", got.Aud, c.want)
			}
			for i := range c.want {
				if got.Aud[i] != c.want[i] {
					t.Fatalf("aud = %v, want %v", got.Aud, c.want)
				}
			}
			if got.Sub != "t" {
				t.Errorf("sibling field lost: sub = %q", got.Sub)
			}
		})
	}

	var bad Claims
	if err := json.Unmarshal([]byte(`{"sub":"t","aud":42}`), &bad); err == nil {
		t.Error("numeric aud should be rejected")
	}
}

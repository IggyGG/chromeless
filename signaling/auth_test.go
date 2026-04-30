package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
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
			name:    "missing token",
			tok:     func() string { return "" },
			sid:     "s", role: "client", wantSub: "missing",
		},
		{
			name:    "malformed (not 3 parts)",
			tok:     func() string { return "aaa.bbb" },
			sid:     "s", role: "client", wantSub: "malformed",
		},
		{
			name:    "bad signature",
			tok:     func() string {
				_, otherPriv, _ := ed25519.GenerateKey(rand.Reader)
				return signToken(otherPriv, good)
			},
			sid:  "s", role: "client", wantSub: "signature",
		},
		{
			name:    "expired",
			tok:     func() string { return signToken(priv, Claims{Sub: "t", Sid: "s", Role: "client", Exp: 1_699_999_900}) },
			sid:     "s", role: "client", wantSub: "expired",
		},
		{
			name:    "not yet valid (nbf in future)",
			tok:     func() string {
				return signToken(priv, Claims{Sub: "t", Sid: "s", Role: "client", Exp: 1_700_001_000, Nbf: 1_700_000_500})
			},
			sid:  "s", role: "client", wantSub: "not yet valid",
		},
		{
			name:    "sid mismatch",
			tok:     func() string { return signToken(priv, good) },
			sid:     "other-sid", role: "client", wantSub: "sid mismatch",
		},
		{
			name:    "role mismatch",
			tok:     func() string { return signToken(priv, good) },
			sid:     "s", role: "browser", wantSub: "role mismatch",
		},
		{
			name:    "tenant missing",
			tok:     func() string { return signToken(priv, Claims{Sid: "s", Role: "client", Exp: 1_700_001_000}) },
			sid:     "s", role: "client", wantSub: "tenant",
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

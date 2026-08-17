package main

import (
	"testing"
	"time"
)

func TestLoadConfigRequiresCredentials(t *testing.T) {
	// No default credential is the whole point: a gateway that boots without
	// one would publish the browser to anyone who can reach the port.
	t.Run("missing user", func(t *testing.T) {
		t.Setenv(envPass, "pw")
		if _, err := loadConfig(); err == nil {
			t.Fatal("expected an error when CHROMELESS_USER is unset")
		}
	})

	t.Run("missing pass", func(t *testing.T) {
		t.Setenv(envUser, "me")
		if _, err := loadConfig(); err == nil {
			t.Fatal("expected an error when CHROMELESS_PASS is unset")
		}
	})

	t.Run("blank user is missing", func(t *testing.T) {
		t.Setenv(envUser, "   ")
		t.Setenv(envPass, "pw")
		if _, err := loadConfig(); err == nil {
			t.Fatal("expected whitespace-only user to be rejected")
		}
	})
}

func TestLoadConfigDefaults(t *testing.T) {
	t.Setenv(envUser, "me")
	t.Setenv(envPass, "pw")

	c, err := loadConfig()
	if err != nil {
		t.Fatalf("loadConfig: %v", err)
	}
	if c.addr != ":"+defaultPort {
		t.Errorf("addr = %q, want :%s", c.addr, defaultPort)
	}
	if c.signalingURL != defaultSignaling {
		t.Errorf("signalingURL = %q, want %q", c.signalingURL, defaultSignaling)
	}
	if c.sessionTTL != defaultSessionTTL {
		t.Errorf("sessionTTL = %v, want %v", c.sessionTTL, defaultSessionTTL)
	}
}

// A password may legitimately contain leading or trailing spaces. Trimming it
// would silently authenticate a different string than the operator set.
func TestLoadConfigDoesNotTrimPassword(t *testing.T) {
	t.Setenv(envUser, "me")
	t.Setenv(envPass, "  spaced  ")

	c, err := loadConfig()
	if err != nil {
		t.Fatalf("loadConfig: %v", err)
	}
	if c.pass != "  spaced  " {
		t.Errorf("pass = %q, want it preserved verbatim", c.pass)
	}
}

// Half a TLS pair means a typo, not "fall back to self-signed" — that would
// hand the operator a server with an identity they did not choose.
func TestLoadConfigRejectsHalfTLSPair(t *testing.T) {
	for _, tc := range []struct{ name, cert, key string }{
		{"cert only", "/tmp/c.pem", ""},
		{"key only", "", "/tmp/k.pem"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Setenv(envUser, "me")
			t.Setenv(envPass, "pw")
			t.Setenv(envTLSCert, tc.cert)
			t.Setenv(envTLSKey, tc.key)
			if _, err := loadConfig(); err == nil {
				t.Fatal("expected an error for a half-specified TLS pair")
			}
		})
	}
}

func TestLoadConfigSessionTTL(t *testing.T) {
	t.Run("parses", func(t *testing.T) {
		t.Setenv(envUser, "me")
		t.Setenv(envPass, "pw")
		t.Setenv(envSessionTTL, "30m")
		c, err := loadConfig()
		if err != nil {
			t.Fatalf("loadConfig: %v", err)
		}
		if c.sessionTTL != 30*time.Minute {
			t.Errorf("sessionTTL = %v, want 30m", c.sessionTTL)
		}
	})

	// A zero or negative TTL would mint cookies that are already expired —
	// an infinite login loop with no error anywhere.
	for _, bad := range []string{"nonsense", "0s", "-5m"} {
		t.Run("rejects "+bad, func(t *testing.T) {
			t.Setenv(envUser, "me")
			t.Setenv(envPass, "pw")
			t.Setenv(envSessionTTL, bad)
			if _, err := loadConfig(); err == nil {
				t.Fatalf("expected %q to be rejected", bad)
			}
		})
	}
}

// Operators write ws:// for a signaling endpoint (every other env var in this
// repo does); ReverseProxy needs http://. Accept both rather than making that
// distinction the operator's problem.
func TestSignalingURLSchemeNormalisation(t *testing.T) {
	cases := map[string]string{
		"ws://signaling:8080":   "http://signaling:8080",
		"wss://signaling:8080":  "https://signaling:8080",
		"WSS://Signaling:8080":  "https://Signaling:8080",
		"http://signaling:8080": "http://signaling:8080",
	}
	for in, want := range cases {
		t.Run(in, func(t *testing.T) {
			t.Setenv(envUser, "me")
			t.Setenv(envPass, "pw")
			t.Setenv(envSignalingURL, in)
			c, err := loadConfig()
			if err != nil {
				t.Fatalf("loadConfig: %v", err)
			}
			if c.signalingURL != want {
				t.Errorf("signalingURL = %q, want %q", c.signalingURL, want)
			}
		})
	}
}

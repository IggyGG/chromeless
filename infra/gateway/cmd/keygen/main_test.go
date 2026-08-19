package main

import (
	"bytes"
	"crypto/ed25519"
	"encoding/base64"
	"strings"
	"testing"

	"github.com/iggy/chromeless/signaling/token"
)

// parseExports reads keygen's stdout the way `eval` does.
func parseExports(t *testing.T, out string) map[string]string {
	t.Helper()
	env := map[string]string{}
	for _, line := range strings.Split(strings.TrimSpace(out), "\n") {
		rest, ok := strings.CutPrefix(line, "export ")
		if !ok {
			t.Fatalf("line is not eval-able: %q", line)
		}
		k, v, ok := strings.Cut(rest, "=")
		if !ok {
			t.Fatalf("line has no value: %q", line)
		}
		env[k] = v
	}
	return env
}

// THE REGRESSION TEST FOR THE COMPOSE QUICKSTART.
//
// `eval "$(go run ./cmd/keygen)"` is step 1 of the documented quickstart, and
// it is what switches the broker from "any caller can connect" to verifying
// every token. Before 2026-08-19 it emitted only the keypair, so it armed the
// broker against a worker it had given no credential: the worker's socket was
// closed with `1008 missing token`, the browser process exited, supervisord
// respawned it every ~30s, and DevTools answered /json/version throughout —
// so the container looked healthy and Connect simply never connected.
//
// Assert on all three exports together. Any one of them alone still passes in
// the broken configuration.
func TestKeygenEmitsEverythingTheStackNeedsToAuthenticate(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if err := run(&stdout, &stderr); err != nil {
		t.Fatalf("run: %v", err)
	}

	env := parseExports(t, stdout.String())
	for _, want := range []string{
		"CHROMELESS_AUTH_PRIVKEY", // the gateway signs with this
		"CHROMELESS_AUTH_PUBKEY",  // the broker verifies with this
		"SIGNALING_TOKEN",         // ...and the worker presents this
	} {
		if env[want] == "" {
			t.Errorf("no %s in keygen's output; the quickstart's `eval` would "+
				"leave it unset", want)
		}
	}
	if t.Failed() {
		t.Fatalf("output was:\n%s", stdout.String())
	}

	// The three must be halves of ONE key. A mismatch is the misconfiguration
	// with no useful symptom: every connection is refused as a bad signature
	// and neither log says the keys differ.
	privRaw, err := base64.StdEncoding.DecodeString(env["CHROMELESS_AUTH_PRIVKEY"])
	if err != nil || len(privRaw) != ed25519.PrivateKeySize {
		t.Fatalf("private key is not a base64 Ed25519 private key (err=%v len=%d)", err, len(privRaw))
	}
	pubRaw, err := base64.StdEncoding.DecodeString(env["CHROMELESS_AUTH_PUBKEY"])
	if err != nil || len(pubRaw) != ed25519.PublicKeySize {
		t.Fatalf("public key is not a base64 Ed25519 public key (err=%v len=%d)", err, len(pubRaw))
	}
	if !bytes.Equal(ed25519.PrivateKey(privRaw).Public().(ed25519.PublicKey), pubRaw) {
		t.Fatal("the printed public key is not the private key's public half")
	}

	// The decisive check: the broker holds only CHROMELESS_AUTH_PUBKEY, so
	// verify the worker's token exactly as signaling/auth.go does.
	c, err := token.Parse(ed25519.PublicKey(pubRaw), env["SIGNALING_TOKEN"])
	if err != nil {
		t.Fatalf("the broker would refuse the worker's token: %v", err)
	}
	if c.Role != "browser" {
		t.Errorf("token role = %q, want browser — the worker connects as the "+
			"browser peer and the broker checks role against the envelope's `from`", c.Role)
	}
	if c.Sid != "dev" {
		t.Errorf("token sid = %q, want dev (compose.yaml's SESSION_ID default)", c.Sid)
	}
}

// SESSION_ID must reach the token, or a non-default session mints a credential
// the broker refuses as a sid mismatch.
func TestKeygenHonoursSessionID(t *testing.T) {
	t.Setenv("SESSION_ID", "workstation-2")

	var stdout, stderr bytes.Buffer
	if err := run(&stdout, &stderr); err != nil {
		t.Fatalf("run: %v", err)
	}
	env := parseExports(t, stdout.String())

	pubRaw, _ := base64.StdEncoding.DecodeString(env["CHROMELESS_AUTH_PUBKEY"])
	c, err := token.Parse(ed25519.PublicKey(pubRaw), env["SIGNALING_TOKEN"])
	if err != nil {
		t.Fatalf("token does not verify: %v", err)
	}
	if c.Sid != "workstation-2" {
		t.Errorf("sid = %q, want workstation-2", c.Sid)
	}
}

// stdout is consumed by `eval`. Anything human-readable on it becomes a shell
// command — so the summary line must go to stderr.
func TestKeygenStdoutIsPurelyEvalable(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if err := run(&stdout, &stderr); err != nil {
		t.Fatalf("run: %v", err)
	}
	for _, line := range strings.Split(strings.TrimSpace(stdout.String()), "\n") {
		if !strings.HasPrefix(line, "export ") {
			t.Errorf("stdout carries a non-export line, which `eval` would "+
				"execute as a command: %q", line)
		}
	}
	if stderr.Len() == 0 {
		t.Error("nothing on stderr; a 30-day credential appears in the " +
			"environment with no announcement")
	}
}

package workertoken

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"testing"
	"time"

	"github.com/iggy/chromeless/signaling/token"
)

// The broker verifies role against the `from` of the first envelope and sid
// against the URL path segment. A token that is correctly signed but carries
// either wrong is refused with a message that reads like a signing failure —
// which is what made the original defect so expensive to place.
func TestMintProducesABrowserTokenTheBrokerWillAccept(t *testing.T) {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}

	tok, err := Mint(priv, "dev")
	if err != nil {
		t.Fatalf("Mint: %v", err)
	}

	c, err := token.Parse(pub, tok)
	if err != nil {
		t.Fatalf("the token does not verify against the public half: %v", err)
	}
	if c.Role != "browser" {
		t.Errorf("role = %q, want %q — the worker connects as the browser peer", c.Role, "browser")
	}
	if c.Sid != "dev" {
		t.Errorf("sid = %q, want %q", c.Sid, "dev")
	}
	if c.Sub != Tenant {
		t.Errorf("sub = %q, want %q — a differing tenant puts the two peers in "+
			"different broker namespaces, where neither ever sees the other", c.Sub, Tenant)
	}
	if c.Jti == "" {
		t.Error("jti is empty; the denylist keys on it, so revocation would be impossible")
	}

	// Backdated nbf: the worker and the broker are different machines, and a
	// few seconds of clock skew otherwise rejects the token outright.
	now := time.Now().Unix()
	if c.Nbf > now {
		t.Errorf("nbf = %d is in the future (now %d); clock skew would reject this", c.Nbf, now)
	}
	// Long-lived on purpose — the embedder cannot refresh. A short token would
	// present as a worker that runs fine and then stops offering, days later.
	if got := time.Unix(c.Exp, 0).Sub(time.Unix(c.Iat, 0)); got < 7*24*time.Hour {
		t.Errorf("token lifetime %v is short; the worker cannot refresh "+
			"(capture/signaling/cb_signaling_reconnect.h)", got)
	}
}

func TestMintRejectsAnEmptySessionID(t *testing.T) {
	_, priv, _ := ed25519.GenerateKey(rand.Reader)
	// Minting for "" would produce a token the broker refuses as a sid
	// mismatch against whatever session the worker actually joined.
	if _, err := Mint(priv, "  "); err == nil {
		t.Fatal("Mint accepted a blank session id")
	}
}

// The 32-byte seed and the 64-byte private key are both "the key" colloquially,
// and ed25519.Sign panics on the former. Catch it with a message instead.
func TestParsePrivKeyRejectsTheSeed(t *testing.T) {
	_, priv, _ := ed25519.GenerateKey(rand.Reader)
	seed := base64.StdEncoding.EncodeToString(priv.Seed())

	if _, err := ParsePrivKey(seed); err == nil {
		t.Fatal("ParsePrivKey accepted a 32-byte seed as a private key")
	}
	full := base64.StdEncoding.EncodeToString(priv)
	if _, err := ParsePrivKey(full); err != nil {
		t.Fatalf("ParsePrivKey rejected a valid key: %v", err)
	}
	if _, err := ParsePrivKey("not base64 at all!"); err == nil {
		t.Fatal("ParsePrivKey accepted non-base64 input")
	}
}

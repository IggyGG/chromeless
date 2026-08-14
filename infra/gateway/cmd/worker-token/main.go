// Command worker-token mints a long-lived browser-role session token.
//
// The gateway's /issue-token deliberately mints SHORT tokens (15 minutes), so
// that ceasing to issue is an effective revocation and client/src/auth.ts's
// TokenRefresher can roll them over. A worker cannot do that: the embedder
// reads WEBRTC_SIGNALING_TOKEN once at launch, and
// capture/signaling/cb_signaling_reconnect.h is explicit that token refresh is
// out of scope — a worker whose token expires exits and expects an
// orchestrator to restart it with a fresh one.
//
// In Kubernetes the controller mints per-session tokens as it creates pods. A
// standalone deployment has no controller, so the worker's token is minted
// once, out of band, with the same key the gateway signs with.
//
//	CHROMELESS_AUTH_PRIVKEY=<base64> SESSION_ID=dev go run ./cmd/worker-token
//
// Prints the token on stdout and nothing else, so it can be captured directly
// into a Secret.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/iggy/chromeless/signaling/token"
)

// Long by design — see the package comment. This is the lifetime of the
// deployment, not of a user session.
const workerTokenTTL = 30 * 24 * time.Hour

func main() {
	raw, err := base64.StdEncoding.DecodeString(
		strings.TrimSpace(os.Getenv("CHROMELESS_AUTH_PRIVKEY")))
	if err != nil || len(raw) != ed25519.PrivateKeySize {
		fmt.Fprintln(os.Stderr,
			"CHROMELESS_AUTH_PRIVKEY must be a base64 Ed25519 private key "+
				"(64 bytes; the full key, not the 32-byte seed)")
		os.Exit(2)
	}
	sid := strings.TrimSpace(os.Getenv("SESSION_ID"))
	if sid == "" {
		fmt.Fprintln(os.Stderr, "SESSION_ID is required and must match the worker's")
		os.Exit(2)
	}

	var jti [16]byte
	if _, err := rand.Read(jti[:]); err != nil {
		fmt.Fprintln(os.Stderr, "jti:", err)
		os.Exit(1)
	}

	now := time.Now()
	fmt.Print(token.Sign(ed25519.PrivateKey(raw), token.Claims{
		Sub:  "standalone",
		Sid:  sid,
		Role: "browser",
		Iat:  now.Unix(),
		// Backdated: the worker and the broker are different machines, and a
		// token minted "in the future" by a few seconds of clock skew is
		// rejected outright.
		Nbf: now.Add(-30 * time.Second).Unix(),
		Exp: now.Add(workerTokenTTL).Unix(),
		Jti: hex.EncodeToString(jti[:]),
	}))
}

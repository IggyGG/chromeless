// Command keygen prints everything the standalone stack's three services need
// to authenticate each other: a matching Ed25519 keypair, and the worker's
// session token signed with it.
//
// The gateway mints session tokens and the broker verifies them, so they need
// the two halves of one key — and the broker needs its half at ITS startup,
// before the gateway exists. Neither can hand the other anything at boot, so
// the pair is generated here and each half is passed to the service that needs
// it.
//
//	eval "$(cd infra/gateway && go run ./cmd/keygen)"
//	docker compose -f infra/compose.yaml up
//
// The `cd` is required: this repo has no root Go module, so `go run` must be
// invoked from inside one.
//
// WHY THE WORKER'S TOKEN IS PRINTED HERE TOO
// ------------------------------------------
// Because omitting it silently broke the documented quickstart, and did so in
// the most confusing way available.
//
// The worker authenticates to the broker like any other peer. Turning auth on
// is exactly what `eval`ing this command does — CHROMELESS_AUTH_PUBKEY reaches
// the broker, which switches from "any caller can connect" to verifying every
// token. Before 2026-08-19 nothing then gave the worker one:
// infra/compose.yaml defaulted SIGNALING_TOKEN to empty and the quickstart
// never mentioned `cmd/worker-token`. The broker closed the worker's socket
// with `1008 missing token`.
//
// That failure is invisible from outside. A handshake failure EXITS the
// browser process, supervisord respawns it every ~30 s, and DevTools keeps
// answering /json/version throughout — so the container reports healthy, the
// page loads, and Connect simply never connects. docs/operations/standalone.md
// even told you to check for "auth enabled (Ed25519)" in the broker log and
// treat it as the good case; it is the precise condition that broke the
// worker.
//
// So the token is minted where the key is. Step 1 of the quickstart now
// produces a stack that works, rather than one that needs a second command
// nobody was told about. SESSION_ID selects the session id it is scoped to and
// must match the worker's (default `dev`, which is compose's default too).
//
// Output is shell-eval'able on purpose: copying base64 blobs by hand is
// exactly where a mismatched pair comes from, and a mismatch presents as every
// connection being rejected by the broker with no hint that the keys differ.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/iggy/chromeless/infra/gateway/internal/workertoken"
)

func main() {
	if err := run(os.Stdout, os.Stderr); err != nil {
		fmt.Fprintf(os.Stderr, "keygen: %v\n", err)
		os.Exit(1)
	}
}

// run is separate from main so a test can assert on the WHOLE output. The
// defect this guards against was not in any one value but in an absent line:
// the keypair was correct, and the worker had no token to present.
func run(stdout, stderr io.Writer) error {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return err
	}

	// Must match the worker's SESSION_ID. compose.yaml defaults that to `dev`,
	// so this defaults the same way: a token scoped to a session the worker
	// never joins is rejected as `sid mismatch`, which reads like a signing
	// problem rather than a naming one.
	sid := strings.TrimSpace(os.Getenv("SESSION_ID"))
	if sid == "" {
		sid = "dev"
	}

	tok, err := workertoken.Mint(priv, sid)
	if err != nil {
		return fmt.Errorf("minting the worker token: %w", err)
	}

	// The private key goes to the gateway (which signs) and the public key to
	// the broker (which verifies). Never the other way round: the broker only
	// ever needs the half that cannot mint.
	fmt.Fprintf(stdout, "export CHROMELESS_AUTH_PRIVKEY=%s\n", base64.StdEncoding.EncodeToString(priv))
	fmt.Fprintf(stdout, "export CHROMELESS_AUTH_PUBKEY=%s\n", base64.StdEncoding.EncodeToString(pub))
	// Read by infra/compose.yaml and passed to the worker as
	// WEBRTC_SIGNALING_TOKEN by infra/launch-chromeless.sh.
	fmt.Fprintf(stdout, "export SIGNALING_TOKEN=%s\n", tok)
	// stderr, so `eval "$(...)"` is unaffected. Without a line saying what
	// just happened, a 30-day credential appears in the environment with no
	// announcement at all.
	fmt.Fprintf(stderr,
		"keygen: keypair + a %d-day browser token for session %q "+
			"(set SESSION_ID to change it)\n",
		int(workertoken.TTL.Hours()/24), sid)
	return nil
}

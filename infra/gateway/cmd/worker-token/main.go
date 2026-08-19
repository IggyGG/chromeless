// Command worker-token mints a long-lived browser-role session token for an
// EXISTING keypair.
//
// `cmd/keygen` already prints one alongside the keypair it generates, and that
// is the path the compose quickstart takes. This command is for the case where
// the key already exists and only the token is wanted:
//
// Kubernetes, where infra/k8s/standalone/deploy.sh mints the keypair and the
// worker's token into two different Secrets; re-issuing after the 30-day token
// expires, without rotating the key and having to restart the gateway and
// broker as well; or a split-host deployment (infra/compose.worker.yaml),
// where the worker lives on another machine and needs its own token minted
// from the gateway's key.
//
//	CHROMELESS_AUTH_PRIVKEY=<base64> SESSION_ID=dev go run ./cmd/worker-token
//
// Prints the token on stdout and nothing else, so it can be captured directly
// into a Secret. The minting itself lives in internal/workertoken so this and
// keygen cannot drift — see that package for why the token is long-lived when
// the gateway's own /issue-token is not.
package main

import (
	"fmt"
	"os"

	"github.com/iggy/chromeless/infra/gateway/internal/workertoken"
)

func main() {
	priv, err := workertoken.ParsePrivKey(os.Getenv("CHROMELESS_AUTH_PRIVKEY"))
	if err != nil {
		fmt.Fprintf(os.Stderr, "CHROMELESS_AUTH_PRIVKEY: %v\n", err)
		os.Exit(2)
	}

	// No default here, unlike keygen. This command is used where a session id
	// already exists and is not necessarily `dev`; defaulting would mint a
	// token for the wrong session, which is refused as `sid mismatch` — a
	// failure that reads like a signing problem rather than a naming one.
	tok, err := workertoken.Mint(priv, os.Getenv("SESSION_ID"))
	if err != nil {
		fmt.Fprintln(os.Stderr, "worker-token:", err)
		os.Exit(2)
	}
	fmt.Print(tok)
}

// Command keygen prints a matching Ed25519 keypair for the standalone stack.
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
// Output is shell-eval'able on purpose: copying two base64 blobs by hand is
// exactly where a mismatched pair comes from, and a mismatch presents as every
// connection being rejected by the broker with no hint that the keys differ.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"fmt"
	"os"
)

func main() {
	pub, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		fmt.Fprintf(os.Stderr, "keygen: %v\n", err)
		os.Exit(1)
	}

	// The private key goes to the gateway (which signs) and the public key to
	// the broker (which verifies). Never the other way round: the broker only
	// ever needs the half that cannot mint.
	fmt.Printf("export CHROMELESS_AUTH_PRIVKEY=%s\n", base64.StdEncoding.EncodeToString(priv))
	fmt.Printf("export CHROMELESS_AUTH_PUBKEY=%s\n", base64.StdEncoding.EncodeToString(pub))
}

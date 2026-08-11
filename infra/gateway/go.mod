module github.com/iggy/chromeless/infra/gateway

go 1.22

// The session-token wire format is shared with the broker that verifies what
// this service mints. A `replace` rather than a published version because
// there is none — this repo has no module proxy and every directory is its
// own module. See infra/gateway/Dockerfile: the image must therefore build
// with the REPO ROOT as its context, not this directory.
require github.com/iggy/chromeless/signaling v0.0.0

replace github.com/iggy/chromeless/signaling => ../../signaling

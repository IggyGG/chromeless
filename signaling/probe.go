// Package main — probe.go
//
// T102: HTTP-based connection-quality probe endpoint.
//
//	GET  /probe?size=N    — returns N bytes of zeros.
//	POST /probe           — consumes the request body and returns
//	                        {"received": <bytes>}.
//
// Both bandwidth-cap their respective directions to 1 MiB per
// request. Auth (T48 ?token=) is required when CHROMELESS_AUTH_PUBKEY is
// set. Tokens are NOT region-checked here — probes are always
// allowed to flow against any signaling deployment so the client
// can pick a region from the dropdown if GeoDNS misroutes (see
// docs/operations/multi-region.md § 2). Tenant scoping is in the
// auth log though, so abuse is traceable.
//
// See docs/protocols/probe-protocol.md for the wire format.

package main

import (
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"strconv"
	"strings"
)

const (
	probeMaxBytes      = 1 << 20 // 1 MiB per direction per request
	probeMaxQuerySize  = 1 << 20 // ?size= clamp
	probeQueryParamKey = "size"
)

// probeHandler dispatches GET vs POST. Other methods get 405.
func probeHandler(log *slog.Logger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		// CORS — probes are typically cross-origin from the static
		// client host. Mirror /turn-credentials.
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Cache-Control", "no-store")

		// Handle CORS preflight before auth — preflights have no token.
		if r.Method == http.MethodOptions {
			w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
			w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
			w.WriteHeader(http.StatusNoContent)
			return
		}

		// Auth, when enabled, mirrors /ws/: ?token=… in the query.
		// Unlike /ws/ we don't have a (sid, role) to bind against, so
		// verifyProbeToken just checks signature + expiry + denylist.
		if authEnabled() {
			tok := r.URL.Query().Get("token")
			if err := verifyProbeToken(tok); err != nil {
				log.Warn("probe auth rejected", slog.Any("err", err))
				http.Error(w, "unauthorized: "+err.Error(), http.StatusUnauthorized)
				return
			}
		}

		switch r.Method {
		case http.MethodGet:
			probeServeGet(w, r, log)
		case http.MethodPost:
			probeServePost(w, r, log)
		default:
			w.Header().Set("Allow", "GET, POST, OPTIONS")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	}
}

// verifyProbeToken accepts any valid session token regardless of its
// (sid, role) — the probe isn't a session, so we don't bind to one.
// We still enforce signature, expiry, and denylist so revoked or
// forged tokens can't drive bandwidth.
func verifyProbeToken(token string) error {
	if token == "" {
		return errors.New("missing token")
	}
	parts := strings.SplitN(token, ".", 3)
	if len(parts) != 3 {
		return errors.New("malformed token")
	}
	sig, err := base64URLDecode(parts[2])
	if err != nil || len(sig) != ed25519.SignatureSize {
		return errors.New("malformed signature")
	}
	signingInput := []byte(parts[0] + "." + parts[1])
	if !ed25519.Verify(globalAuth.pubKey, signingInput, sig) {
		return errors.New("signature mismatch")
	}
	payloadBytes, err := base64URLDecode(parts[1])
	if err != nil {
		return errors.New("malformed payload")
	}
	var c Claims
	if err := json.Unmarshal(payloadBytes, &c); err != nil {
		return errors.New("malformed claims")
	}
	now := timeNow().Unix()
	if c.Exp != 0 && now >= c.Exp {
		return errors.New("token expired")
	}
	if c.Nbf != 0 && now < c.Nbf {
		return errors.New("token not yet valid")
	}
	// Denylist still applies so a revoked tenant can't probe either.
	if globalDenylist != nil {
		hit, _ := globalDenylist.Contains(nil, c.Sub, c.Jti)
		if hit {
			return errors.New("token revoked")
		}
	}
	return nil
}

// probeServeGet streams up to probeMaxBytes zero bytes back to the
// caller. Size comes from ?size=. Content-Length lets the client
// time exactly when the full payload is on the wire.
func probeServeGet(w http.ResponseWriter, r *http.Request, log *slog.Logger) {
	rawSize := r.URL.Query().Get(probeQueryParamKey)
	size, err := strconv.Atoi(rawSize)
	if err != nil || size < 1 {
		http.Error(w, "invalid size", http.StatusBadRequest)
		return
	}
	if size > probeMaxQuerySize {
		size = probeMaxQuerySize
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(size))
	w.WriteHeader(http.StatusOK)

	// Reuse a small zero-filled chunk; flushing per chunk costs nothing.
	const chunk = 32 * 1024
	buf := make([]byte, chunk)
	for remaining := size; remaining > 0; {
		n := chunk
		if remaining < n {
			n = remaining
		}
		if _, err := w.Write(buf[:n]); err != nil {
			log.Debug("probe GET write failed", slog.Any("err", err))
			return
		}
		remaining -= n
	}
}

// probeServePost reads up to probeMaxBytes from the body and replies
// with the byte count. Larger bodies get 413.
func probeServePost(w http.ResponseWriter, r *http.Request, log *slog.Logger) {
	limited := io.LimitReader(r.Body, int64(probeMaxBytes+1))
	n, err := io.Copy(io.Discard, limited)
	if err != nil {
		log.Debug("probe POST read failed", slog.Any("err", err))
		http.Error(w, "read: "+err.Error(), http.StatusBadRequest)
		return
	}
	if n > int64(probeMaxBytes) {
		http.Error(w, "body too large", http.StatusRequestEntityTooLarge)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]int64{"received": n})
}

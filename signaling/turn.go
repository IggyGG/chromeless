package main

// ICE / TURN helper endpoint.
//
// GET /turn-credentials returns a JSON document the browser can pass
// straight into `new RTCPeerConnection({ iceServers })`.
//
// v1 behaviour:
//   - Always emit one or more STUN entries. STUN_URLS env var (comma
//     separated) overrides the default Cloudflare/Google STUN list.
//   - If TURN_URLS is non-empty, append a TURN entry with TURN_USER and
//     TURN_PASS as static long-term credentials. Empty TURN_URLS means
//     TURN is omitted entirely (the v1 dev story: STUN-only).
//
// This is intentionally NOT yet RFC-7635 TURN-REST. Time-bounded
// credentials and HMAC-derived passwords land in Phase 3 alongside auth;
// this endpoint is the wiring point for that to plug into.
//
// See docs/ice-and-turn.md for the design rationale and operational
// roadmap.

import (
	"encoding/json"
	"net/http"
	"os"
	"strings"
)

// iceServer mirrors the W3C RTCIceServer dictionary — only the fields
// we actually populate here.
type iceServer struct {
	URLs       []string `json:"urls"`
	Username   string   `json:"username,omitempty"`
	Credential string   `json:"credential,omitempty"`
}

type iceConfig struct {
	IceServers []iceServer `json:"iceServers"`
}

// defaultStunURLs is used when STUN_URLS is unset. These are anycast
// STUN endpoints we can rely on without hosting our own.
var defaultStunURLs = []string{
	"stun:stun.cloudflare.com:3478",
	"stun:stun.l.google.com:19302",
}

// buildICEConfig assembles the response from environment lookups.
// `lookup` is a function variable so tests can inject a fake env.
func buildICEConfig(lookup func(string) string) iceConfig {
	cfg := iceConfig{IceServers: make([]iceServer, 0, 2)}

	stunURLs := splitCSV(lookup("STUN_URLS"))
	if len(stunURLs) == 0 {
		stunURLs = append([]string(nil), defaultStunURLs...)
	}
	cfg.IceServers = append(cfg.IceServers, iceServer{URLs: stunURLs})

	turnURLs := splitCSV(lookup("TURN_URLS"))
	if len(turnURLs) > 0 {
		cfg.IceServers = append(cfg.IceServers, iceServer{
			URLs:       turnURLs,
			Username:   lookup("TURN_USER"),
			Credential: lookup("TURN_PASS"),
		})
	}
	return cfg
}

// splitCSV trims and discards empty entries so " a , ,b " → ["a","b"].
func splitCSV(s string) []string {
	if s == "" {
		return nil
	}
	parts := strings.Split(s, ",")
	out := parts[:0]
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

// turnHandler is the HTTP handler installed at /turn-credentials.
func turnHandler(w http.ResponseWriter, _ *http.Request) {
	cfg := buildICEConfig(os.Getenv)
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	// Permit the browser client to fetch this from a different origin
	// (the static-server origin in compose). Tightened in Phase 3.
	w.Header().Set("Access-Control-Allow-Origin", "*")
	_ = json.NewEncoder(w).Encode(cfg)
}

// Package main is the chromeless TURN-credentials issuer
// (T76). It implements RFC 7635 / TURN-REST credential issuance for
// self-hosted coturn deployments — and for managed-TURN deployments
// that use the same shared-secret HMAC convention (Cloudflare TURN
// supports a compatible shape; metered.ca/twilio offer their own
// gateway).
//
// Contract:
//
//	POST /issue-turn-cred
//	  Authorization: Bearer <T48 session token>
//	  body: {"sessionId": "...", "ttlSeconds": 3600}    (sessionId optional;
//	                                                     defaults to token sid;
//	                                                     ttl bounded
//	                                                     [300, 86400])
//	  ->
//	  200 OK
//	  body: {"username":"<expiry>:<tenant>:<sid>",
//	         "credential":"<base64(hmac-sha1(secret, username))>",
//	         "ttl": 3600,
//	         "urls": ["turn:turn.example.com:3478?transport=udp", ...]}
//
// Secret rotation: the issuer accepts CHROMELESS_TURN_SHARED_SECRET (the
// current secret used to mint credentials) and optionally
// CHROMELESS_TURN_SHARED_SECRET_PREV (the previous secret kept active for
// validation overlap). During rotation, coturn must trust both for
// the duration of the overlap; see secret-rotation.md.
//
// The issuer never *validates* TURN credentials it issued — that's
// coturn's job. We only ever use the previous-secret value when an
// upstream hot-rolls and a credential issued with the old secret is
// still in flight at the next call (the credential's own TTL bounds
// how long that matters).

package main

import (
	"context"
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// ----- config / startup ----------------------------------------------------

const (
	envAuthPubkey       = "CHROMELESS_AUTH_PUBKEY"
	envSharedSecret     = "CHROMELESS_TURN_SHARED_SECRET"
	envSharedSecretPrev = "CHROMELESS_TURN_SHARED_SECRET_PREV"
	envTurnURLs         = "CHROMELESS_TURN_URLS" // comma-separated
	envSTUNURLs         = "CHROMELESS_STUN_URLS" // comma-separated; appended verbatim to iceServers

	defaultTTL = 3600  // 1 hour
	minTTL     = 300   // 5 minutes
	maxTTL     = 86400 // 24 hours
)

type config struct {
	pubKey       ed25519.PublicKey
	sharedSecret string // current — used to mint
	prevSecret   string // optional — kept for validation overlap during rotation
	turnURLs     []string
	stunURLs     []string
	listen       string
}

func loadConfig(logger *slog.Logger) (*config, error) {
	rawPub := os.Getenv(envAuthPubkey)
	if rawPub == "" {
		return nil, fmt.Errorf("%s is required", envAuthPubkey)
	}
	pubBytes, err := base64.StdEncoding.DecodeString(strings.TrimSpace(rawPub))
	if err != nil {
		return nil, fmt.Errorf("decode %s: %w", envAuthPubkey, err)
	}
	if len(pubBytes) != ed25519.PublicKeySize {
		return nil, fmt.Errorf("%s wrong length: %d (want %d)", envAuthPubkey, len(pubBytes), ed25519.PublicKeySize)
	}

	secret := strings.TrimSpace(os.Getenv(envSharedSecret))
	if secret == "" {
		return nil, fmt.Errorf("%s is required", envSharedSecret)
	}
	prev := strings.TrimSpace(os.Getenv(envSharedSecretPrev)) // optional

	turnRaw := strings.TrimSpace(os.Getenv(envTurnURLs))
	if turnRaw == "" {
		return nil, fmt.Errorf("%s is required", envTurnURLs)
	}
	stunRaw := strings.TrimSpace(os.Getenv(envSTUNURLs))

	c := &config{
		pubKey:       ed25519.PublicKey(pubBytes),
		sharedSecret: secret,
		prevSecret:   prev,
		turnURLs:     splitCSV(turnRaw),
		stunURLs:     splitCSV(stunRaw),
		listen:       envOr("LISTEN", ":8090"),
	}
	logger.Info("turn-issuer configured",
		slog.Int("turn_urls", len(c.turnURLs)),
		slog.Int("stun_urls", len(c.stunURLs)),
		slog.Bool("prev_secret_set", prev != ""),
	)
	return c, nil
}

func splitCSV(s string) []string {
	if s == "" {
		return nil
	}
	parts := strings.Split(s, ",")
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

func envOr(k, fallback string) string {
	if v, ok := os.LookupEnv(k); ok && v != "" {
		return v
	}
	return fallback
}

// ----- claims / token verification ----------------------------------------
//
// Mirrors signaling/auth.go's verification path so a single token works
// against both signaling and the issuer. If you change the JWT shape
// there, change it here. Tests in main_test.go use the same signing
// helper to keep these in sync.

type Claims struct {
	Sub  string `json:"sub"`
	Sid  string `json:"sid"`
	Role string `json:"role"`
	Exp  int64  `json:"exp"`
	Iat  int64  `json:"iat"`
	Nbf  int64  `json:"nbf,omitempty"`
}

// timeNow is package-level so tests can fake the clock.
var timeNow = time.Now

func verifyToken(pub ed25519.PublicKey, token string) (*Claims, error) {
	if token == "" {
		return nil, errors.New("missing token")
	}
	parts := strings.Split(token, ".")
	if len(parts) != 3 {
		return nil, errors.New("malformed token")
	}
	signingInput := []byte(parts[0] + "." + parts[1])
	sig, err := base64URLDecode(parts[2])
	if err != nil || len(sig) != ed25519.SignatureSize {
		return nil, errors.New("malformed signature")
	}
	if !ed25519.Verify(pub, signingInput, sig) {
		return nil, errors.New("signature mismatch")
	}
	payload, err := base64URLDecode(parts[1])
	if err != nil {
		return nil, errors.New("malformed payload")
	}
	var c Claims
	if err := json.Unmarshal(payload, &c); err != nil {
		return nil, errors.New("malformed claims")
	}
	if c.Sub == "" {
		return nil, errors.New("tenant claim missing")
	}
	now := timeNow().Unix()
	if c.Exp != 0 && now >= c.Exp {
		return nil, errors.New("token expired")
	}
	if c.Nbf != 0 && now < c.Nbf {
		return nil, errors.New("token not yet valid")
	}
	return &c, nil
}

func base64URLDecode(s string) ([]byte, error) {
	if rem := len(s) % 4; rem != 0 {
		s += strings.Repeat("=", 4-rem)
	}
	return base64.URLEncoding.DecodeString(s)
}

// ----- credential issuance -----------------------------------------------

// issueCredential returns the (username, credential) pair per RFC 7635.
//
// username = "<expiry_unix>:<tenant>:<sid>"
// credential = base64(hmac_sha1(secret, username))
func issueCredential(secret, tenant, sid string, expiryUnix int64) (string, string) {
	username := fmt.Sprintf("%d:%s:%s", expiryUnix, tenant, sid)
	mac := hmac.New(sha1.New, []byte(secret))
	mac.Write([]byte(username))
	credential := base64.StdEncoding.EncodeToString(mac.Sum(nil))
	return username, credential
}

// boundTTL clamps a requested ttl into [minTTL, maxTTL]. Zero or
// negative requests round up to defaultTTL.
func boundTTL(req int) int {
	if req <= 0 {
		return defaultTTL
	}
	if req < minTTL {
		return minTTL
	}
	if req > maxTTL {
		return maxTTL
	}
	return req
}

// ----- HTTP layer --------------------------------------------------------

type issueRequest struct {
	SessionID  string `json:"sessionId,omitempty"`
	TTLSeconds int    `json:"ttlSeconds,omitempty"`
}

type issueResponse struct {
	Username   string   `json:"username"`
	Credential string   `json:"credential"`
	TTL        int      `json:"ttl"`
	URLs       []string `json:"urls"`
	// IceServers mirrors the shape signaling/turn.go (T25) returns so
	// the client's existing fetchTurnConfig handling works without
	// change against either endpoint.
	IceServers []iceServer `json:"iceServers"`
}

type iceServer struct {
	URLs       []string `json:"urls"`
	Username   string   `json:"username,omitempty"`
	Credential string   `json:"credential,omitempty"`
}

var (
	mIssued = promauto.NewCounter(prometheus.CounterOpts{
		Name: "cb_turn_issuer_credentials_issued_total",
		Help: "Total successful credential issuances.",
	})
	mDenied = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_turn_issuer_denied_total",
		Help: "Total credential issuance denials, by reason.",
	}, []string{"reason"})
)

func init() {
	for _, r := range []string{"missing_token", "expired", "malformed", "bad_signature", "tenant_missing", "method", "body"} {
		mDenied.WithLabelValues(r) // pre-register
	}
}

type handler struct {
	cfg *config
	mu  sync.RWMutex // guards cfg.sharedSecret / prevSecret on rotation
	log *slog.Logger
}

// CurrentSecret / PreviousSecret are read-side accessors so tests and a
// future SIGHUP-reload path can swap secrets atomically.
func (h *handler) CurrentSecret() string {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.cfg.sharedSecret
}
func (h *handler) PreviousSecret() string {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.cfg.prevSecret
}

// SetSecrets replaces the current and previous secrets atomically.
// Used by SIGHUP reload (not implemented in this scaffold) and by
// tests.
func (h *handler) SetSecrets(current, previous string) {
	h.mu.Lock()
	h.cfg.sharedSecret = current
	h.cfg.prevSecret = previous
	h.mu.Unlock()
}

func (h *handler) issue(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		mDenied.WithLabelValues("method").Inc()
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}
	tok := bearerToken(r)
	if tok == "" {
		mDenied.WithLabelValues("missing_token").Inc()
		http.Error(w, "Bearer token required", http.StatusUnauthorized)
		return
	}
	claims, err := verifyToken(h.cfg.pubKey, tok)
	if err != nil {
		reason := classifyAuthErr(err)
		mDenied.WithLabelValues(reason).Inc()
		http.Error(w, "auth: "+err.Error(), http.StatusUnauthorized)
		return
	}

	var req issueRequest
	if r.Body != nil && r.ContentLength != 0 {
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			mDenied.WithLabelValues("body").Inc()
			http.Error(w, "decode body: "+err.Error(), http.StatusBadRequest)
			return
		}
	}

	sid := req.SessionID
	if sid == "" {
		sid = claims.Sid
	}
	if sid == "" {
		mDenied.WithLabelValues("body").Inc()
		http.Error(w, "sessionId required (in body or token sid claim)", http.StatusBadRequest)
		return
	}

	ttl := boundTTL(req.TTLSeconds)
	expiry := timeNow().Unix() + int64(ttl)

	secret := h.CurrentSecret()
	username, credential := issueCredential(secret, claims.Sub, sid, expiry)

	resp := issueResponse{
		Username:   username,
		Credential: credential,
		TTL:        ttl,
		URLs:       h.cfg.turnURLs,
		IceServers: buildIceServers(h.cfg.stunURLs, h.cfg.turnURLs, username, credential),
	}
	mIssued.Inc()

	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	if err := json.NewEncoder(w).Encode(resp); err != nil {
		h.log.Warn("write response failed", slog.Any("err", err))
	}
}

func buildIceServers(stunURLs, turnURLs []string, username, credential string) []iceServer {
	servers := make([]iceServer, 0, 2)
	if len(stunURLs) > 0 {
		servers = append(servers, iceServer{URLs: append([]string(nil), stunURLs...)})
	}
	if len(turnURLs) > 0 {
		servers = append(servers, iceServer{
			URLs:       append([]string(nil), turnURLs...),
			Username:   username,
			Credential: credential,
		})
	}
	return servers
}

func bearerToken(r *http.Request) string {
	auth := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if strings.HasPrefix(auth, prefix) {
		return strings.TrimSpace(auth[len(prefix):])
	}
	// Fallback: ?token=... query param. Keeps parity with the
	// signaling-server's WS upgrade path; harmless for HTTP because
	// caller will only ever attach a Bearer header in practice.
	if t := r.URL.Query().Get("token"); t != "" {
		return t
	}
	return ""
}

func classifyAuthErr(err error) string {
	switch err.Error() {
	case "missing token":
		return "missing_token"
	case "token expired":
		return "expired"
	case "tenant claim missing":
		return "tenant_missing"
	case "signature mismatch":
		return "bad_signature"
	default:
		return "malformed"
	}
}

// ----- main --------------------------------------------------------------

func main() {
	listen := flag.String("listen", "", "override LISTEN env (defaults :8090)")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	cfg, err := loadConfig(logger)
	if err != nil {
		logger.Error("startup config invalid", slog.Any("err", err))
		os.Exit(2)
	}
	if *listen != "" {
		cfg.listen = *listen
	}

	h := &handler{cfg: cfg, log: logger}

	mux := http.NewServeMux()
	mux.HandleFunc("/issue-turn-cred", h.issue)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})
	mux.Handle("/metrics", promhttp.Handler())

	srv := &http.Server{
		Addr:              cfg.listen,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	errCh := make(chan error, 1)
	go func() {
		logger.Info("turn-issuer listening", slog.String("addr", cfg.listen))
		errCh <- srv.ListenAndServe()
	}()

	select {
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("listen failed", slog.Any("err", err))
			os.Exit(1)
		}
	case <-ctx.Done():
		logger.Info("shutdown signal received")
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutdownCtx)
	logger.Info("bye")
}

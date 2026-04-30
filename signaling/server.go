// Package main implements the v0 cloud-browser-webrtc signaling server.
//
// It is intentionally minimal:
//   - one process, one in-memory map of sessions
//   - one session_id == exactly two peers, no fan-out
//   - JSON message envelope: {type, from, data}
//   - no auth, no TURN/STUN config, no session pool
//
// Phase-1 scope per docs/PROJECT_BRIEF.md and task T13. Multi-tenant,
// auth, TURN config, and session pools are explicitly Phase-3+.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

// ----- protocol -----

// peerRole identifies which side of the session a websocket connection
// represents. Exactly one "client" and one "browser" are allowed per
// session_id. The role is taken from the Envelope.From field on the
// first message a peer sends.
type peerRole string

const (
	roleClient  peerRole = "client"
	roleBrowser peerRole = "browser"
)

// Envelope is the JSON wire format exchanged over /ws/{session_id}.
//
//	{"type": "offer"|"answer"|"ice"|"bye", "from": "client"|"browser", "data": {...}}
//
// Data is opaque to the signaling server; it forwards the entire envelope
// verbatim to the other peer.
type Envelope struct {
	Type string          `json:"type"`
	From peerRole        `json:"from"`
	Data json.RawMessage `json:"data,omitempty"`
}

func (r peerRole) valid() bool {
	return r == roleClient || r == roleBrowser
}

func (r peerRole) other() peerRole {
	if r == roleClient {
		return roleBrowser
	}
	return roleClient
}

var validTypes = map[string]struct{}{
	"offer":               {},
	"answer":              {},
	"ice":                 {},
	"bye":                 {},
	"request_renegotiate": {}, // T37: peer asks the offerer to redo SDP w/ ICE restart.
}

// ----- session hub -----

// peer is one end of a session.
type peer struct {
	role peerRole
	conn *websocket.Conn
	send chan []byte
	log  *slog.Logger
	// claims is non-nil iff auth is enabled. Used to enforce role
	// consistency on subsequent envelopes (T48).
	claims *Claims
}

// anonymousTenant is the tenant id used when auth is disabled. T67 keys
// every session by (tenant, session_id); when there's no token there's
// no tenant claim, so we collapse all anonymous traffic into a single
// namespace and warn loudly via the auth-disabled startup log.
const anonymousTenant = "_anonymous"

// sessionKey is the composite map key — tenant first, then session id.
// Pre-T67 the key was just session_id; this turns "two tenants both
// running 'demo'" from a cross-talk into two independent rooms.
type sessionKey struct {
	tenant string
	id     string
}

// session holds at most two peers keyed by role.
//
// T96: `recent` is the replay buffer that fixes the "streamer's offer
// is lost when no client peer is connected yet" race. Whenever a peer
// forwards a replayable envelope (offer/answer/request_renegotiate),
// we keep the most recent one per (sender role, type). When the
// counterpart peer joins, register() replays whatever is buffered.
//
// Bounded by design: max 2 roles × 3 replayable types = 6 envelopes
// per session, regardless of throughput. ICE is intentionally NOT
// replayable: it's a stream where pre-join candidates are stale by
// the time the new peer arrives, and the streamer continues to
// trickle post-join candidates anyway.
type session struct {
	id     string
	tenant string
	mu     sync.Mutex
	peers  map[peerRole]*peer
	recent map[peerRole]map[string][]byte
}

// replayableTypes lists the envelope types whose most-recent value is
// buffered for a future-joining peer. The slice doubles as the replay
// order — offer must come before answer must come before
// request_renegotiate so the receiver can build its peer connection
// state in the right order.
var replayableTypes = []string{"offer", "answer", "request_renegotiate"}

func isReplayable(envType string) bool {
	for _, t := range replayableTypes {
		if t == envType {
			return true
		}
	}
	return false
}

// hub owns all live sessions.
type hub struct {
	mu       sync.Mutex
	sessions map[sessionKey]*session
	log      *slog.Logger
}

func newHub(log *slog.Logger) *hub {
	return &hub{
		sessions: make(map[sessionKey]*session),
		log:      log,
	}
}

// getOrCreate returns the session for (tenant, id), creating it if absent.
func (h *hub) getOrCreate(tenant, id string) *session {
	h.mu.Lock()
	defer h.mu.Unlock()
	k := sessionKey{tenant: tenant, id: id}
	s, ok := h.sessions[k]
	if !ok {
		s = &session{
			id:     id,
			tenant: tenant,
			peers:  make(map[peerRole]*peer, 2),
			recent: make(map[peerRole]map[string][]byte, 2), // T96 replay buffer
		}
		h.sessions[k] = s
		recordSessionCreated(tenant) // T38/T67 metrics
	}
	return s
}

// dropIfEmpty removes the session if both peers have left.
func (h *hub) dropIfEmpty(tenant, id string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	k := sessionKey{tenant: tenant, id: id}
	s, ok := h.sessions[k]
	if !ok {
		return
	}
	s.mu.Lock()
	empty := len(s.peers) == 0
	s.mu.Unlock()
	if empty {
		delete(h.sessions, k)
		recordSessionDropped(tenant) // T38/T67 metrics
	}
}

// register attempts to add p to the session. Returns an error if the role
// slot is already taken.
//
// T96: after registering, replay any buffered envelopes from the OTHER
// peer. This handles the "streamer joined and offered before the
// client connected" race — the offer is buffered, replayed when the
// client peer arrives. Replay happens under the same mutex so a
// concurrent forward() to the same peer cannot interleave with the
// replay (the new peer sees buffered envelopes first, then live).
//
// Replay only fires once per role-join. Subsequent live envelopes
// arrive via forward() in the normal way.
//
// Returns the count of replayed envelopes (informational; for the
// caller's structured log).
func (s *session) register(p *peer) (replayed int, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if existing, ok := s.peers[p.role]; ok {
		_ = existing
		return 0, fmt.Errorf("role %q already present in session %s/%s", p.role, s.tenant, s.id)
	}
	s.peers[p.role] = p

	// T96: replay buffered envelopes from the counterpart. Replay
	// order matches replayableTypes (offer before answer before
	// renegotiate) so the receiver builds its peer connection state
	// in the right order.
	other := p.role.other()
	if buf, ok := s.recent[other]; ok {
		for _, t := range replayableTypes {
			raw, has := buf[t]
			if !has {
				continue
			}
			select {
			case p.send <- raw:
				replayed++
			default:
				// New peer's send buffer is somehow already full —
				// shouldn't happen at register time (the writePump
				// hasn't started yet, but the channel has capacity).
				// Drop the replay rather than block; live forwards
				// will hit the same buffer and surface the same
				// problem with a better diagnostic.
				p.log.Warn("send buffer full during replay; dropping",
					slog.String("type", t))
			}
		}
	}
	return replayed, nil
}

// unregister removes p from the session.
func (s *session) unregister(p *peer) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if cur, ok := s.peers[p.role]; ok && cur == p {
		delete(s.peers, p.role)
	}
}

// forward sends raw to the peer in role; returns false if no such peer.
//
// T96: when envType is replayable, the most recent raw is also kept
// in the sender's slot of the replay buffer so that a later-joining
// counterpart receives it on register(). The buffer keeps a single
// envelope per (sender, type) — no growth.
func (s *session) forward(role peerRole, envType string, raw []byte) bool {
	s.mu.Lock()
	if isReplayable(envType) {
		sender := role.other()
		buf, ok := s.recent[sender]
		if !ok {
			buf = make(map[string][]byte, len(replayableTypes))
			s.recent[sender] = buf
		}
		// Copy raw because the caller's underlying buffer may be
		// reused across reads. Cheap (offers are ~5 KiB) and avoids a
		// data race with the read pump's next ReadMessage.
		dup := make([]byte, len(raw))
		copy(dup, raw)
		buf[envType] = dup
	}
	target, ok := s.peers[role]
	s.mu.Unlock()
	if !ok {
		return false
	}
	select {
	case target.send <- raw:
		return true
	default:
		// Slow consumer: drop and let the read pump close eventually.
		target.log.Warn("send buffer full, dropping message")
		return false
	}
}

// ----- handlers -----

const (
	writeWait      = 10 * time.Second
	pongWait       = 60 * time.Second
	pingPeriod     = 30 * time.Second
	maxMessageSize = 1 << 20 // 1 MiB; SDP can be large.
	sendBuffer     = 32
)

var upgrader = websocket.Upgrader{
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
	// Phase-1 dev: accept any origin. Auth + origin check land in Phase 3.
	CheckOrigin: func(*http.Request) bool { return true },
}

func healthHandler(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(`{"status":"ok"}`))
}

// wsHandler upgrades the request, learns the peer's role from its first
// message, registers it with the session, and runs read/write pumps.
func (h *hub) wsHandler(w http.ResponseWriter, r *http.Request) {
	sessionID := strings.TrimPrefix(r.URL.Path, "/ws/")
	if sessionID == "" || strings.ContainsRune(sessionID, '/') {
		http.Error(w, "invalid session_id", http.StatusBadRequest)
		return
	}

	connLog := h.log.With(slog.String("session_id", sessionID), slog.String("remote", r.RemoteAddr))

	// T48: token may be present as ?token=. We can't verify yet (no
	// role) — defer until we've read the first envelope.
	tokenParam := r.URL.Query().Get("token")

	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		connLog.Warn("upgrade failed", slog.Any("err", err))
		return
	}
	conn.SetReadLimit(maxMessageSize)
	_ = conn.SetReadDeadline(time.Now().Add(pongWait))
	conn.SetPongHandler(func(string) error {
		return conn.SetReadDeadline(time.Now().Add(pongWait))
	})

	// First frame must be a valid envelope so we can learn the role.
	_, raw, err := conn.ReadMessage()
	if err != nil {
		connLog.Info("closed before first message", slog.Any("err", err))
		_ = conn.Close()
		return
	}
	var first Envelope
	if err := json.Unmarshal(raw, &first); err != nil {
		connLog.Warn("first message not JSON", slog.Any("err", err))
		_ = conn.WriteControl(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseUnsupportedData, "invalid envelope"),
			time.Now().Add(writeWait))
		_ = conn.Close()
		return
	}
	if !first.From.valid() {
		connLog.Warn("first message has invalid 'from'", slog.String("from", string(first.From)))
		_ = conn.Close()
		return
	}

	// T48: verify the session token now that we know the role.
	// T67: extract the tenant id from the token claim. When auth is
	// disabled, fall back to anonymousTenant — every connection in
	// that mode shares the same namespace (matching pre-T67 behaviour).
	// T89: after verify, consult the denylist for a (tenant, jti) hit.
	var tokenClaims *Claims
	tenantID := anonymousTenant
	if authEnabled() {
		c, vErr := verifyToken(tokenParam, sessionID, string(first.From))
		if vErr != nil {
			connLog.Warn("auth rejected", slog.Any("err", vErr), slog.String("role", string(first.From)))
			_ = conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.ClosePolicyViolation, vErr.Error()),
				time.Now().Add(writeWait))
			_ = conn.Close()
			return
		}
		// T89: deny revoked tokens. We fail-open on denylist errors.
		revoked, _ := checkRevoked(r.Context(), c, connLog)
		if revoked {
			connLog.Warn("auth rejected: revoked",
				slog.String("tenant", c.Sub), slog.String("jti", c.Jti))
			_ = conn.WriteControl(websocket.CloseMessage,
				websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "token revoked"),
				time.Now().Add(writeWait))
			_ = conn.Close()
			return
		}
		tokenClaims = c
		tenantID = c.Sub
		connLog.Info("auth ok", slog.String("tenant", c.Sub), slog.String("role", c.Role), slog.String("jti", c.Jti))
	}
	connLog = connLog.With(slog.String("tenant", tenantID))

	p := &peer{
		role:   first.From,
		conn:   conn,
		send:   make(chan []byte, sendBuffer),
		log:    connLog.With(slog.String("role", string(first.From))),
		claims: tokenClaims,
	}

	sess := h.getOrCreate(tenantID, sessionID)
	replayed, err := sess.register(p)
	if err != nil {
		p.log.Warn("rejecting duplicate role", slog.Any("err", err))
		_ = conn.WriteControl(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.ClosePolicyViolation, err.Error()),
			time.Now().Add(writeWait))
		_ = conn.Close()
		h.dropIfEmpty(tenantID, sessionID)
		return
	}
	if replayed > 0 {
		p.log.Info("peer joined (replayed buffered envelopes)", slog.Int("replayed", replayed))
	} else {
		p.log.Info("peer joined")
	}
	recordPeerRegistered(p.role, tenantID) // T38/T67 metrics

	// Forward the first envelope before starting pumps.
	if _, ok := validTypes[first.Type]; ok {
		recordMessageForwarded(first.Type) // T38 metrics
		if !sess.forward(p.role.other(), first.Type, raw) {
			p.log.Debug("no peer for first frame yet", slog.String("type", first.Type))
		}
	} else {
		p.log.Warn("ignoring unknown first-frame type", slog.String("type", first.Type))
	}

	// Pumps.
	done := make(chan struct{})
	go p.writePump(done)
	p.readPump(sess, done)

	sess.unregister(p)
	recordPeerUnregistered(p.role, tenantID) // T38/T67 metrics; pairs with the Inc above
	h.dropIfEmpty(tenantID, sessionID)
	p.log.Info("peer left")
}

func (p *peer) readPump(sess *session, done chan struct{}) {
	defer func() {
		_ = p.conn.Close()
		close(done)
	}()
	for {
		_, raw, err := p.conn.ReadMessage()
		if err != nil {
			// T38 metrics: extract the close code so /metrics reports
			// {1000, 1001, 1006, 1011, …} as the {code} label.
			code := 0
			var ce *websocket.CloseError
			if errors.As(err, &ce) {
				code = ce.Code
			}
			recordClose(code)
			if websocket.IsUnexpectedCloseError(err,
				websocket.CloseGoingAway,
				websocket.CloseNormalClosure,
				websocket.CloseAbnormalClosure) {
				p.log.Info("read error", slog.Any("err", err))
			}
			return
		}
		var env Envelope
		if err := json.Unmarshal(raw, &env); err != nil {
			p.log.Warn("dropping non-JSON frame", slog.Any("err", err))
			continue
		}
		if _, ok := validTypes[env.Type]; !ok {
			p.log.Warn("dropping unknown type", slog.String("type", env.Type))
			continue
		}
		// Defensive: ensure 'from' matches the registered role; rewrite if absent.
		if env.From != p.role {
			env.From = p.role
			if rewritten, err := json.Marshal(env); err == nil {
				raw = rewritten
			}
		}
		recordMessageForwarded(env.Type) // T38 metrics
		if !sess.forward(p.role.other(), env.Type, raw) {
			p.log.Debug("no counterpart yet (buffered if replayable)", slog.String("type", env.Type))
		}
		if env.Type == "bye" {
			return
		}
	}
}

func (p *peer) writePump(done chan struct{}) {
	ticker := time.NewTicker(pingPeriod)
	defer func() {
		ticker.Stop()
		_ = p.conn.Close()
	}()
	for {
		select {
		case msg, ok := <-p.send:
			_ = p.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if !ok {
				_ = p.conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}
			if err := p.conn.WriteMessage(websocket.TextMessage, msg); err != nil {
				p.log.Info("write error", slog.Any("err", err))
				return
			}
		case <-ticker.C:
			_ = p.conn.SetWriteDeadline(time.Now().Add(writeWait))
			if err := p.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				return
			}
		case <-done:
			return
		}
	}
}

// ----- main / lifecycle -----

func main() {
	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	port := os.Getenv("SIGNALING_PORT")
	if port == "" {
		port = "8080"
	}
	addr := ":" + port

	// T93: read CBWRTC_REGION first so every metric Set/Inc on the
	// startup path picks up the correct label.
	initRegion(logger)
	// T48: dev issuer must run before initAuth so the env var it
	// sets is visible. Both are no-ops in production unless the
	// matching env vars are set.
	initDevIssuer(logger)
	initAuth(logger)
	// T89: revocation denylist + admin endpoint. Both are
	// independently env-gated; either or both may be disabled.
	deny := initDenylist(logger)
	initAdmin(logger)

	h := newHub(logger)

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", healthHandler)
	mux.HandleFunc("/turn-credentials", turnHandler)
	mux.HandleFunc("/issue-token", devIssuerHandler) // T48 dev only; 404 unless CBWRTC_DEV_ISSUER=1
	if adminEnabled() {
		// T89: only register the route when the admin pubkey is set.
		// "Forgot to configure auth" should be 404, not anonymous.
		mux.HandleFunc("/admin/revoke", adminRevokeHandler(deny, logger))
	}
	mux.HandleFunc("/ws/", h.wsHandler)
	mux.Handle("/metrics", metricsHandler()) // T38

	srv := &http.Server{
		Addr:              addr,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	errCh := make(chan error, 1)
	go func() {
		logger.Info("signaling server listening", slog.String("addr", addr))
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

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(shutdownCtx); err != nil {
		logger.Error("graceful shutdown failed", slog.Any("err", err))
		os.Exit(1)
	}
	logger.Info("bye")
}

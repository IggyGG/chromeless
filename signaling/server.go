// Package main implements the v0 chromeless signaling server.
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
	"strconv"
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
	"probe_result":        {}, // T102: client → streamer connection-quality hint.
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
	// negotiated records that this peer took part in a real negotiation —
	// it was replayed the counterpart's buffered SDP on join, or it sent
	// SDP of its own. Read on disconnect to decide whether the
	// COUNTERPART's buffer is now void; see unregister's call site.
	//
	// Guarded by the session mutex on every access — markNegotiated and
	// register write it, didNegotiate reads it — so `go test -race` is clean
	// even though writer and reader are the same goroutine in practice.
	negotiated bool
	// saidBye records that this peer sent a `bye` of its own before the
	// socket dropped. Read on disconnect so unregister does NOT synthesise a
	// SECOND bye for a peer that already announced itself.
	//
	// Without this the worker received TWO byes ~49ms apart on every clean
	// client close (the client sends one at session.ts:622, then the socket
	// closes and unregister synthesised another). Harmless while a bye meant
	// "exit" — the process was already going down. Fatal once the worker
	// RE-ARMS instead: the first bye tore down and rebuilt the session, and
	// the second closed the fresh one ~1ms later, so the pending
	// OnRenegotiationNeeded landed in kClosed -> FailWithReason -> kFailed ->
	// Rearm() refused -> process exit. Measured 2026-08-21; the browser pid
	// changed across every viewer change while the logs said "rebuilt".
	//
	// Guarded by the session mutex, same as negotiated above.
	saidBye bool
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
// per session, regardless of throughput.
//
// T104: `recentICE` mirrors the same idea for ICE candidates, but as
// a *bounded FIFO queue* per sender (vs. single most-recent). The T96
// "streamer-finishes-gathering-before-client-joins" race ALSO drops
// pre-join ICE candidates on the floor; with libwebrtc completing its
// host+reflexive gathering in milliseconds on container boot, by the
// time the client joins the streamer has nothing left to trickle and
// the connection sits in iceConnectionState=checking forever.
//
// Cap is iceReplayMaxPerSender (oldest evicted on overflow). Replay
// drops entries older than iceReplayMaxAge to avoid handing the new
// peer a TURN candidate whose allocation has lapsed.
type session struct {
	id        string
	tenant    string
	mu        sync.Mutex
	peers     map[peerRole]*peer
	recent    map[peerRole]map[string]bufferedSDP
	recentICE map[peerRole][]bufferedICE // T104
}

// bufferedICE is a captured ICE envelope plus the wall-clock time we
// saw it, so register() can age out stale TURN candidates on replay.
type bufferedICE struct {
	raw []byte
	ts  time.Time
}

// bufferedSDP is the same idea for offer/answer/renegotiate.
//
// It was NOT timestamped originally, and that asymmetry was a live bug: ICE
// aged out after 5 minutes while the SDP it belonged to lived forever, so a
// viewer joining later was handed a well-formed offer carrying dead ICE
// credentials and NO candidates to go with it. Measured 2026-08-20 from a real
// browser: an offer buffered at 19:21 was replayed at 19:44 (`replayed:1`,
// 23 minutes old); the client answered it, gathered its own host/srflx/relay
// candidates, and ICE went straight to `failed` because the peer those
// credentials named no longer existed. Everything upstream looked perfect.
type bufferedSDP struct {
	raw []byte
	ts  time.Time
}

const (
	// iceReplayMaxPerSender bounds memory: a typical streamer trickles
	// 4–8 host + ~2 reflexive + 1 relay + 1 end-of-candidates = ~12
	// candidates. 32 leaves headroom for ICE restarts mid-buffer
	// without unbounded growth.
	iceReplayMaxPerSender = 32
	// defaultICEReplayMaxAge is the default age cap on replayed ICE.
	// Bounded well under typical TURN allocation TTL (~10 min) so we never
	// replay a candidate whose underlying allocation has lapsed.
	//
	// OSS-W0: raised 60s → 300s. The original 60s was justified as "longer
	// than any realistic Phase-1 demo session-start latency" — an assumption
	// that microVM/cold-node deployments invalidate. On firecracker, guest
	// boot + reconnect can push client registration past 60s, at which point
	// EVERY buffered guest candidate ages out and is dropped. The peer then
	// sees remoteCandidates=0 and sits in iceConnectionState=checking forever,
	// surfacing as 0x0 video with nothing in the logs to explain it. Triform's
	// broker independently converged on 300s for exactly this reason; anyone
	// on slow-boot infrastructure (firecracker, gVisor, cold k8s nodes pulling
	// images) hits it identically. 300s remains comfortably under the ~600s
	// TURN allocation TTL, so the staleness guarantee is preserved.
	defaultICEReplayMaxAge = 300 * time.Second

	// iceReplayMaxAgeEnv overrides defaultICEReplayMaxAge, in seconds.
	// Values <= 0, unparsable values, and values above the TURN-allocation
	// safety ceiling are ignored (the default is kept and a warning logged).
	iceReplayMaxAgeEnv = "CHROMELESS_ICE_REPLAY_MAX_AGE_S"

	// iceReplayMaxAgeCeiling is the hard upper bound on the configured age.
	// Past this we would routinely replay candidates whose TURN allocation
	// has lapsed, which is worse than replaying nothing.
	iceReplayMaxAgeCeiling = 600 * time.Second
)

// iceReplayMaxAge is the effective age cap, resolved once at init from
// iceReplayMaxAgeEnv. A var (not a const) so deployments on slow-boot
// infrastructure can tune it without a rebuild.
var iceReplayMaxAge = resolveICEReplayMaxAge(os.Getenv)

// resolveICEReplayMaxAge reads the override and validates it. Takes a getenv
// func so tests can exercise it without mutating process env.
func resolveICEReplayMaxAge(getenv func(string) string) time.Duration {
	raw := strings.TrimSpace(getenv(iceReplayMaxAgeEnv))
	if raw == "" {
		return defaultICEReplayMaxAge
	}
	secs, err := strconv.Atoi(raw)
	if err != nil || secs <= 0 {
		slog.Warn("ignoring invalid ICE replay max age override",
			slog.String("env", iceReplayMaxAgeEnv),
			slog.String("value", raw),
			slog.Duration("using", defaultICEReplayMaxAge))
		return defaultICEReplayMaxAge
	}
	d := time.Duration(secs) * time.Second
	if d > iceReplayMaxAgeCeiling {
		slog.Warn("ICE replay max age override exceeds the TURN-allocation ceiling; clamping",
			slog.String("env", iceReplayMaxAgeEnv),
			slog.Duration("requested", d),
			slog.Duration("ceiling", iceReplayMaxAgeCeiling))
		return iceReplayMaxAgeCeiling
	}
	return d
}

// replayableTypes lists the envelope types whose most-recent value is
// buffered for a future-joining peer. The slice doubles as the replay
// order — offer must come before answer must come before
// request_renegotiate so the receiver can build its peer connection
// state in the right order. ICE is replayed AFTER these (T104), so
// the receiver has setRemoteDescription'd the offer before any
// addIceCandidate calls land.
var replayableTypes = []string{"offer", "answer", "request_renegotiate"}

// hasICEData reports whether the given ICE envelope carries a real
// candidate (vs. a hello-frame / end-of-candidates marker whose
// `data` field is null or absent). It does a fast JSON-aware scan
// rather than a full unmarshal: the read pump runs hot and ICE is
// the highest-frequency envelope type.
//
// Returns true when the envelope's `data` field is a non-null object
// (either `{...}` or any other non-`null` token), false otherwise.
// Conservative: malformed envelopes are treated as "no data" and
// therefore not buffered; live-forwarding still happens regardless.
func hasICEData(raw []byte) bool {
	// Find `"data"` (must be a JSON key, so always preceded by `,` or
	// `{` plus optional whitespace). We accept the common pretty-print
	// variants.
	const key = `"data"`
	for i := 0; i < len(raw)-len(key); i++ {
		if raw[i] != '"' {
			continue
		}
		if i+len(key) > len(raw) {
			break
		}
		if string(raw[i:i+len(key)]) != key {
			continue
		}
		// Walk forward past `:` and whitespace.
		j := i + len(key)
		for j < len(raw) && (raw[j] == ' ' || raw[j] == '\t') {
			j++
		}
		if j >= len(raw) || raw[j] != ':' {
			continue
		}
		j++
		for j < len(raw) && (raw[j] == ' ' || raw[j] == '\t') {
			j++
		}
		if j >= len(raw) {
			return false
		}
		// `null` → no data; anything else (object, array, string,
		// number, bool) is treated as real data.
		if raw[j] == 'n' && j+4 <= len(raw) && string(raw[j:j+4]) == "null" {
			return false
		}
		return true
	}
	// `data` key absent.
	return false
}

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
			id:        id,
			tenant:    tenant,
			peers:     make(map[peerRole]*peer, 2),
			recent:    make(map[peerRole]map[string]bufferedSDP, 2), // T96 replay buffer
			recentICE: make(map[peerRole][]bufferedICE, 2),          // T104 ICE queue
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
			entry, has := buf[t]
			if !has {
				continue
			}
			// AGE-GATE the SDP, exactly as the ICE queue below is gated.
			//
			// Without this, `recent` kept an offer forever while its ICE aged
			// out after 5 minutes — so a viewer joining later got a perfectly
			// well-formed offer whose ufrag/pwd named a peer that no longer
			// existed, plus no candidates at all. It answers, gathers, and goes
			// straight to iceConnectionState=failed. Measured from a real
			// browser 2026-08-20: `replayed:1` of an offer buffered 23 minutes
			// earlier; ICE failed in 5s with host, srflx AND relay candidates
			// present on both sides, which reads as a TURN problem and is not.
			//
			// Same cap as ICE: they describe the same negotiation, so outliving
			// it is never useful.
			if age := time.Since(entry.ts); age > iceReplayMaxAge {
				p.log.Info("dropping stale buffered SDP on replay",
					slog.String("type", t),
					slog.Duration("age", age.Round(time.Second)),
					slog.Duration("max", iceReplayMaxAge))
				delete(buf, t)
				continue
			}
			select {
			case p.send <- entry.raw:
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

	// T104: replay buffered ICE candidates from the counterpart, in
	// the original send order. Must come AFTER the SDP replay above
	// so the receiver has already setRemoteDescription'd before any
	// addIceCandidate calls land. Drop entries older than
	// iceReplayMaxAge — TURN allocations may have lapsed.
	if queue, ok := s.recentICE[other]; ok {
		now := timeNow()
		var dropped int
		for _, env := range queue {
			if now.Sub(env.ts) > iceReplayMaxAge {
				dropped++
				continue
			}
			select {
			case p.send <- env.raw:
				replayed++
			default:
				p.log.Warn("send buffer full during ICE replay; dropping",
					slog.Int("queued", len(queue)))
			}
		}
		if dropped > 0 {
			p.log.Info("dropped stale ICE on replay",
				slog.Int("dropped", dropped),
				slog.Duration("age_cap", iceReplayMaxAge))
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

// discardReplay drops the envelopes buffered FROM role, so a later-joining
// peer is not handed a dead peer connection's SDP.
//
// A `bye` means the negotiated peer connection is GONE — its ufrag/pwd, its
// DTLS fingerprint and every candidate either side ever gathered are void.
// Replaying them is not merely useless, it is actively misleading: the new peer
// receives a complete, well-formed offer, answers it, and pairs its own fresh
// candidates against sockets that no longer exist. Nothing in either log says
// so, and it presents as `iceConnectionState=checking` forever with a single
// unresponsive pair (`req=0`) — indistinguishable by inspection from a NAT or
// TURN failure, which is where an afternoon went.
//
// CALLED FROM TWO PLACES, and both are load-bearing:
//
//   - When a peer DISCONNECTS, for its own buffer. Its candidates and SDP
//     describe a peer connection that went away with it. Missing this was
//     observed live as a fresh worker being replayed the previous viewer's
//     stale `answer` on join, reaching ICE `connected` seconds after boot with
//     nobody watching, and burning its one and only session on a dead client.
//
//   - On `bye`, for BOTH roles. A bye ends the negotiated session, not just
//     the sender's half. The observed failure was a CLIENT bye (viewer closes
//     the tab) invalidating the BROWSER's buffered offer: the worker tears its
//     peer connection down and — because cb_offerer_driver's kClosed is
//     terminal and main_parts' native_session_started_ is a one-way latch —
//     can never offer again for the life of the process. Its socket stays up,
//     so the session is not reaped, and the next viewer to join is replayed
//     the dead offer plus nine dead candidates (`replayed: 10`).
//
// The buffer exists for the opposite race (an offer arriving BEFORE the
// counterpart joins, T96/T104) — a live session whose peer has not shown up
// yet. Once either side says `bye`, that no longer describes anything.
//
// Note this cannot rely on hub.dropIfEmpty: that only fires when BOTH peers
// have disconnected, and in the bye case the browser deliberately stays
// connected.
// markNegotiated records that p took part in an offer/answer exchange. Held
// under the session lock because peers on two goroutines touch the same
// session; the flag itself is only ever read on p's own goroutine, after its
// readPump has returned.
func (s *session) markNegotiated(p *peer) {
	s.mu.Lock()
	defer s.mu.Unlock()
	p.negotiated = true
}

// markSaidBye records that p sent a `bye` itself. Same locking discipline as
// markNegotiated.
func (s *session) markSaidBye(p *peer) {
	s.mu.Lock()
	defer s.mu.Unlock()
	p.saidBye = true
}

// didSayBye reads p.saidBye under the session lock.
func (s *session) didSayBye(p *peer) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return p.saidBye
}

// didNegotiate reads p.negotiated under the session lock. Both the write
// (markNegotiated / register) and this read take s.mu, so the flag is
// race-free even though in practice they run on the same goroutine.
func (s *session) didNegotiate(p *peer) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return p.negotiated
}

func (s *session) discardReplay(roles ...peerRole) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, r := range roles {
		delete(s.recent, r)
		delete(s.recentICE, r)
	}
}

// forward sends raw to the peer in role; returns false if no such peer.
//
// T96: when envType is replayable, the most recent raw is also kept
// in the sender's slot of the replay buffer so that a later-joining
// counterpart receives it on register(). The buffer keeps a single
// envelope per (sender, type) — no growth.
//
// T104: when envType is "ice", we also append into a bounded FIFO
// queue per sender so a later-joining counterpart receives the full
// candidate stream on register(). Cap is iceReplayMaxPerSender;
// oldest is evicted on overflow.
func (s *session) forward(role peerRole, envType string, raw []byte) bool {
	s.mu.Lock()
	if isReplayable(envType) {
		sender := role.other()
		buf, ok := s.recent[sender]
		if !ok {
			buf = make(map[string]bufferedSDP, len(replayableTypes))
			s.recent[sender] = buf
		}
		// Copy raw because the caller's underlying buffer may be
		// reused across reads. Cheap (offers are ~5 KiB) and avoids a
		// data race with the read pump's next ReadMessage.
		dup := make([]byte, len(raw))
		copy(dup, raw)
		// Timestamped so register() can age it out — see bufferedSDP.
		buf[envType] = bufferedSDP{raw: dup, ts: time.Now()}
	}
	if envType == "ice" && hasICEData(raw) {
		// Skip envelopes whose `data` is null or absent: those are
		// hello frames (registration-only, see /ws/ first-frame
		// handling) or end-of-candidates markers. Replaying either
		// to a late-joining peer is at best a no-op and at worst
		// confuses libwebrtc into early-EOC state.
		sender := role.other()
		// Same defensive copy reasoning as above — the gorilla read
		// pump reuses its buffer across ReadMessage calls.
		dup := make([]byte, len(raw))
		copy(dup, raw)
		queue := s.recentICE[sender]
		if len(queue) >= iceReplayMaxPerSender {
			// Evict oldest. We keep the most recent N because they're
			// most likely still valid (host candidates rarely change
			// during a session, and reflexive/relay get refreshed by
			// libwebrtc's renomination if they expire).
			queue = queue[1:]
		}
		queue = append(queue, bufferedICE{raw: dup, ts: timeNow()})
		s.recentICE[sender] = queue
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
	// CV2-81 (verification-lead 2026-05-20): the native cb-chromium worker
	// dials ws://<host>/api/webrtc/signaling/<session> — cb_signaling_ws_client.cc
	// hardcodes that path (the physics-broker shape). The legacy JS-streamer
	// and the e2e harnesses dial /ws/<session>. Accept either prefix so a
	// single signaling server brokers both peer kinds; the path remaining
	// after the matched prefix is the session id.
	sessionID := r.URL.Path
	for _, p := range []string{"/api/webrtc/signaling/", "/ws/"} {
		if strings.HasPrefix(sessionID, p) {
			sessionID = strings.TrimPrefix(sessionID, p)
			break
		}
	}
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
	// A peer's buffered envelopes describe ITS peer connection, so they die
	// with its socket — see discardReplay. Doing this only on `bye` was not
	// enough: a client that goes away without one (tab closed, network drop,
	// 1006) leaves a live `answer` in the buffer, and the next BROWSER to join
	// is replayed it. A fresh worker then consumes a dead answer, believes it
	// has a viewer, and burns its one and only session (see
	// docs/findings/one-session-per-worker-process.md) on nobody — observed
	// live as a worker reaching ICE `connected` seconds after boot with no
	// client present.
	//
	// AND the counterpart's, when this peer had actually negotiated. A `bye`
	// already does this (readPump below), but a bye is not guaranteed to
	// arrive: `beforeunload` does not fire reliably when a browser context is
	// closed programmatically, and a lid-close, crash or network drop never
	// sends one at all. Measured 2026-08-19 (task 341797): across a whole
	// Playwright run the broker received ZERO byes, so the worker's offer
	// survived every disconnect. Spec 01 connected in 0.42s off that buffer
	// and passed; specs 02 and 03 were each replayed the same 7 now-dead
	// envelopes, answered an offer whose peer connection the worker had
	// already torn down, and sat in `connecting` for the full 30s timeout.
	//
	// Gated on `negotiated` — NOT unconditional. The buffer exists for the
	// opposite race (T96: the worker offers before any viewer has joined), and
	// a client that connects and drops again without exchanging SDP must NOT
	// destroy that still-valid offer. Only a peer that SENT SDP has made the
	// counterpart's buffer describe a connection that is now gone — being
	// replayed the offer is passive and happens to every joiner.
	roles := []peerRole{p.role}
	negotiated := sess.didNegotiate(p)
	if negotiated {
		roles = append(roles, p.role.other())
	}
	sess.discardReplay(roles...)

	// ...and TELL the counterpart, which discarding alone does not do.
	//
	// Dropping the buffers stops the NEXT peer being handed dead SDP, but the
	// peer still connected never learns its partner is gone. For the browser
	// that is fatal: cb_offerer_driver only leaves its session on an explicit
	// close, so a worker whose viewer vanished keeps encoding into a dead
	// transport forever and never offers again. Measured 2026-08-20 against
	// the live standalone stack: the worker sat at
	// `frames_encoded=3369 fps=10 1280x720` with NO viewer, while every new
	// client joined to an empty replay buffer (`peer joined` with no
	// `replayed`) and waited out its timeout with `m=` lines absent — no offer
	// had ever been sent to it.
	//
	// So synthesise the `bye` the departing peer failed to send. This is the
	// same envelope a clean teardown produces, so the counterpart takes its
	// existing, well-tested path: the worker closes its session, exits, and
	// supervisord respawns it ready to offer to whoever joins next.
	//
	// Gated on `negotiated` for the same reason as the discard above: a peer
	// that connected and dropped without exchanging SDP never had a session,
	// so announcing its death would tear down a worker that is legitimately
	// waiting for its first viewer.
	// ...but ONLY if this peer did not already send one. A clean client close
	// sends its own bye (client/src/session.ts) and THEN drops the socket;
	// synthesising a second one delivered two byes ~49ms apart, which a
	// re-arming worker cannot survive — the first rebuilds the session and the
	// second closes the rebuild, stranding OnRenegotiationNeeded in kClosed.
	// See peer.saidBye for the measured trace.
	if negotiated && !sess.didSayBye(p) {
		if raw, err := json.Marshal(Envelope{Type: "bye", From: p.role}); err == nil {
			if sess.forward(p.role.other(), "bye", raw) {
				p.log.Info("synthesised bye to counterpart (peer left without one)",
					slog.String("to", string(p.role.other())))
			}
		}
	}

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
		// SDP from this peer means a negotiation is under way. Recorded so
		// that if this peer vanishes WITHOUT a bye, unregister knows the
		// counterpart's buffered SDP is now void. Not ICE: candidates alone
		// do not establish that an offer/answer exchange happened, and the
		// T96 race (SDP buffered before the counterpart joins) must keep
		// working.
		if env.Type == "offer" || env.Type == "answer" {
			sess.markNegotiated(p)
		}
		if !sess.forward(p.role.other(), env.Type, raw) {
			p.log.Debug("no counterpart yet (buffered if replayable)", slog.String("type", env.Type))
		}
		if env.Type == "bye" {
			// The negotiated session is over for BOTH sides, not just the
			// sender's — so drop both buffers. See discardReplay for what
			// replaying a dead session's offer costs.
			sess.discardReplay(roleClient, roleBrowser)
			// Recorded so the imminent unregister does not synthesise a
			// SECOND bye on top of this one. See peer.saidBye.
			sess.markSaidBye(p)
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

	// T93: read CHROMELESS_REGION first so every metric Set/Inc on the
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
	mux.HandleFunc("/issue-token", devIssuerHandler) // T48 dev only; 404 unless CHROMELESS_DEV_ISSUER=1
	if adminEnabled() {
		// T89: only register the route when the admin pubkey is set.
		// "Forgot to configure auth" should be 404, not anonymous.
		mux.HandleFunc("/admin/revoke", adminRevokeHandler(deny, logger))
	}
	mux.HandleFunc("/ws/", h.wsHandler)
	// CV2-81: route-alias for the native cb-chromium worker, which hardcodes
	// ws://<host>/api/webrtc/signaling/<session>. Same handler; wsHandler
	// strips whichever prefix matched.
	mux.HandleFunc("/api/webrtc/signaling/", h.wsHandler)
	mux.HandleFunc("/probe", probeHandler(logger)) // T102
	mux.Handle("/metrics", metricsHandler())       // T38

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

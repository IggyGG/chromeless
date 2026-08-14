package main

import (
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// fakePeer builds a peer struct with an in-memory send channel large
// enough to hold any reasonable replay burst, plus a discarding logger.
func fakePeer(role peerRole) *peer {
	return &peer{
		role: role,
		send: make(chan []byte, 256),
		log:  slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
}

// drainSend pulls every queued envelope off p.send and returns the raw
// frames in arrival order. Used to assert what register() / forward()
// delivered without spinning up the read pump.
func drainSend(p *peer) [][]byte {
	var out [][]byte
	for {
		select {
		case raw := <-p.send:
			out = append(out, raw)
		default:
			return out
		}
	}
}

// TestSession_ICEReplay_AgeCap verifies the 60-second TURN-allocation
// guard: ICE candidates buffered more than iceReplayMaxAge ago are
// dropped on replay rather than handed to the late-joining peer.
func TestSession_ICEReplay_AgeCap(t *testing.T) {
	// Fake the clock so we can advance past iceReplayMaxAge without
	// burning real wall time. Restore on cleanup so we don't leak
	// state into other tests in the package.
	prev := timeNow
	t.Cleanup(func() { timeNow = prev })

	var mu sync.Mutex
	now := time.Date(2026, 4, 30, 12, 0, 0, 0, time.UTC)
	timeNow = func() time.Time {
		mu.Lock()
		defer mu.Unlock()
		return now
	}
	advance := func(d time.Duration) {
		mu.Lock()
		defer mu.Unlock()
		now = now.Add(d)
	}

	s := &session{
		id:        "age-cap",
		peers:     make(map[peerRole]*peer, 2),
		recent:    make(map[peerRole]map[string][]byte, 2),
		recentICE: make(map[peerRole][]bufferedICE, 2),
	}

	// Register a streamer so forward() has a "sender" attribution.
	brw := fakePeer(roleBrowser)
	if _, err := s.register(brw); err != nil {
		t.Fatalf("register browser: %v", err)
	}

	// Streamer trickles two candidates spaced over time. forward()
	// targets the OTHER role; with no client present yet the ICE
	// gets buffered (and the call returns false).
	if ok := s.forward(roleClient, "ice", []byte(`{"type":"ice","from":"browser","data":{"c":"old"}}`)); ok {
		t.Fatal("forward to absent client should return false")
	}
	advance(2 * iceReplayMaxAge) // first one ages past the cap.
	if ok := s.forward(roleClient, "ice", []byte(`{"type":"ice","from":"browser","data":{"c":"new"}}`)); ok {
		t.Fatal("forward to absent client should return false")
	}

	// Now the client joins. Replay should drop the stale candidate
	// and deliver only the recent one.
	cli := fakePeer(roleClient)
	replayed, err := s.register(cli)
	if err != nil {
		t.Fatalf("register client: %v", err)
	}
	if replayed != 1 {
		t.Fatalf("want 1 replayed envelope, got %d", replayed)
	}
	got := drainSend(cli)
	if len(got) != 1 {
		t.Fatalf("send channel: want 1 envelope, got %d (%v)", len(got), got)
	}
	if string(got[0]) != `{"type":"ice","from":"browser","data":{"c":"new"}}` {
		t.Fatalf("unexpected envelope: %s", got[0])
	}
}

// TestSession_ICEReplay_QueueCap verifies the bounded-queue invariant:
// after iceReplayMaxPerSender + N forwards, only the most recent
// iceReplayMaxPerSender envelopes survive (FIFO eviction).
func TestSession_ICEReplay_QueueCap(t *testing.T) {
	s := &session{
		id:        "queue-cap",
		peers:     make(map[peerRole]*peer, 2),
		recent:    make(map[peerRole]map[string][]byte, 2),
		recentICE: make(map[peerRole][]bufferedICE, 2),
	}
	brw := fakePeer(roleBrowser)
	if _, err := s.register(brw); err != nil {
		t.Fatalf("register browser: %v", err)
	}

	const total = iceReplayMaxPerSender * 2
	for i := 0; i < total; i++ {
		raw := []byte(`{"type":"ice","from":"browser","data":{"i":` +
			itoaSimple(i) + `}}`)
		s.forward(roleClient, "ice", raw)
	}

	cli := fakePeer(roleClient)
	if _, err := s.register(cli); err != nil {
		t.Fatalf("register client: %v", err)
	}
	got := drainSend(cli)
	if len(got) != iceReplayMaxPerSender {
		t.Fatalf("want %d envelopes (cap), got %d", iceReplayMaxPerSender, len(got))
	}
	// First surviving entry should be index `total - cap` = 32, last 63.
	wantFirst := `{"type":"ice","from":"browser","data":{"i":` +
		itoaSimple(total-iceReplayMaxPerSender) + `}}`
	wantLast := `{"type":"ice","from":"browser","data":{"i":` +
		itoaSimple(total-1) + `}}`
	if string(got[0]) != wantFirst {
		t.Fatalf("oldest survivor: want %s, got %s", wantFirst, got[0])
	}
	if string(got[len(got)-1]) != wantLast {
		t.Fatalf("most recent: want %s, got %s", wantLast, got[len(got)-1])
	}
}

func TestHasICEData(t *testing.T) {
	cases := []struct {
		name string
		in   string
		want bool
	}{
		{"explicit null", `{"type":"ice","from":"client","data":null}`, false},
		{"omitted data", `{"type":"ice","from":"client"}`, false},
		{"object data", `{"type":"ice","from":"client","data":{"candidate":"x"}}`, true},
		{"data with whitespace", `{"type":"ice","from":"client","data": {"candidate":"x"}}`, true},
		{"null data with whitespace", `{"type":"ice","from":"client","data":  null }`, false},
		{"data first", `{"data":{"x":1},"type":"ice","from":"client"}`, true},
		{"empty", ``, false},
		{"malformed", `{not json`, false},
		{"data string false-positive guard", `{"type":"ice","from":"data:fake"}`, false}, // "data" only inside a value
	}
	for _, c := range cases {
		got := hasICEData([]byte(c.in))
		if got != c.want {
			t.Errorf("%s: hasICEData(%q) = %v, want %v", c.name, c.in, got, c.want)
		}
	}
}

// TestResolveICEReplayMaxAge covers the OSS-W0 env override. The default
// moved 60s → 300s so that slow-boot deployments (firecracker, cold k8s
// nodes) don't silently age out every buffered guest candidate before the
// viewer registers; the override lets anyone tune further, but never past
// the TURN-allocation ceiling.
func TestResolveICEReplayMaxAge(t *testing.T) {
	cases := []struct {
		name string
		env  string
		want time.Duration
	}{
		{"unset uses default", "", defaultICEReplayMaxAge},
		{"whitespace uses default", "   ", defaultICEReplayMaxAge},
		{"valid override", "120", 120 * time.Second},
		{"trimmed override", "  90  ", 90 * time.Second},
		{"zero rejected", "0", defaultICEReplayMaxAge},
		{"negative rejected", "-30", defaultICEReplayMaxAge},
		{"unparsable rejected", "abc", defaultICEReplayMaxAge},
		{"float rejected", "12.5", defaultICEReplayMaxAge},
		{"above ceiling clamped", "5000", iceReplayMaxAgeCeiling},
		{"at ceiling kept", "600", iceReplayMaxAgeCeiling},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got := resolveICEReplayMaxAge(func(string) string { return c.env })
			if got != c.want {
				t.Errorf("resolveICEReplayMaxAge(%q) = %v, want %v", c.env, got, c.want)
			}
		})
	}

	// The default must stay under the TURN-allocation ceiling, else replay
	// would routinely hand out candidates whose allocation has lapsed.
	if defaultICEReplayMaxAge > iceReplayMaxAgeCeiling {
		t.Fatalf("default %v exceeds ceiling %v", defaultICEReplayMaxAge, iceReplayMaxAgeCeiling)
	}
}

// itoaSimple is a local int→string helper to keep this test file
// self-contained without pulling strconv into a server-internal test.
func itoaSimple(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var buf [20]byte
	i := len(buf)
	for n > 0 {
		i--
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		buf[i] = '-'
	}
	return string(buf[i:])
}

// TestWS_ByeDiscardsReplayBuffer is the regression test for "the second viewer
// never gets video". It drives real WebSockets through the real handler,
// because the bug is in the WIRING (the bye branch of readPump) as much as in
// any one function — an earlier version of this test that called discardReplay
// directly passed with the fix reverted.
//
// The sequence is copied from a live capture, and the details matter:
//
//	browser joins, offers, trickles ICE     → buffered
//	viewer 1 joins, gets the replay, works
//	viewer 1 sends `bye` and disconnects    → the worker tears its peer down
//	                                          and can never offer again
//	                                          (kClosed is terminal), but its
//	                                          SOCKET stays up
//	viewer 2 joins                          → MUST NOT get the dead offer
//
// Note the `bye` comes from the CLIENT while the BROWSER stays connected. That
// is what makes this a real bug rather than a tidiness issue: the session is
// never reaped (dropIfEmpty needs both peers gone), so the browser's stale
// offer sits in the buffer indefinitely. Before the fix, viewer 2 received
// `replayed: 10` — a complete, well-formed offer for a peer connection that no
// longer existed — answered it, and sat in iceConnectionState=checking forever
// with one unresponsive candidate pair. That looks exactly like a NAT or TURN
// failure and is neither.
func TestWS_ByeDiscardsReplayBuffer(t *testing.T) {
	// globalAuth is package-level state that other tests in this package flip
	// on. Without this the handler rejects these tokenless connections and the
	// failure is "peer browser never registered" — which reads as a bug in the
	// broker rather than as leakage from a neighbouring test. Same helper the
	// probe tests use for the same reason.
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/bye-replay"

	// Each connection gets a reader GOROUTINE rather than inline reads with
	// deadlines. gorilla latches the first read error onto the connection, so
	// a read that times out — which is exactly how "nothing was replayed" is
	// observed — makes every later read on that conn fail instantly. That cost
	// a confusing "live forwarding broken" failure against a working server.
	type client struct {
		conn   *websocket.Conn
		frames chan string
	}
	dial := func(name string) *client {
		t.Helper()
		c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
		if err != nil {
			t.Fatalf("dial %s: %v", name, err)
		}
		t.Cleanup(func() { _ = c.Close() })
		cl := &client{conn: c, frames: make(chan string, 64)}
		go func() {
			defer close(cl.frames)
			for {
				_, raw, err := c.ReadMessage()
				if err != nil {
					return
				}
				cl.frames <- string(raw)
			}
		}()
		return cl
	}
	send := func(c *client, who, msg string) {
		t.Helper()
		if err := c.conn.WriteMessage(websocket.TextMessage, []byte(msg)); err != nil {
			t.Fatalf("%s write: %v", who, err)
		}
	}
	// collect drains whatever arrives in a short window. An EMPTY result is
	// the assertion in the middle of this test: "nothing was replayed" is a
	// non-event, observable only by waiting and seeing nothing arrive.
	collect := func(c *client) string {
		t.Helper()
		var sb strings.Builder
		timeout := time.After(750 * time.Millisecond)
		for {
			select {
			case f, ok := <-c.frames:
				if !ok {
					return sb.String()
				}
				sb.WriteString(f)
				sb.WriteByte('\n')
			case <-timeout:
				return sb.String()
			}
		}
	}
	// waitRegistered blocks until the hub actually holds a peer in `role`.
	// Polling the hub beats sleeping: registration happens on the SERVER's
	// read pump, so there is no client-side event to synchronise on, and too
	// short a wait turns a live forward into a silently buffered one — which
	// then reads as "live forwarding is broken" when it is the test that raced.
	waitRegistered := func(role peerRole) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			sess := h.getOrCreate(anonymousTenant, "bye-replay")
			sess.mu.Lock()
			_, ok := sess.peers[role]
			sess.mu.Unlock()
			if ok {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("peer %s never registered", role)
	}

	// The browser registers (the first frame carries the role), offers, and
	// trickles a candidate. No client yet, so both are buffered.
	brw := dial("browser")
	send(brw, "browser", `{"type":"ice","from":"browser"}`) // registration hello
	waitRegistered(roleBrowser)
	send(brw, "browser", `{"type":"offer","from":"browser","data":{"sdp":"v=0 SESSION-ONE"}}`)
	send(brw, "browser", `{"type":"ice","from":"browser","data":{"candidate":"one"}}`)

	// Viewer 1 joins and is correctly replayed the live session. If this stops
	// holding, the assertion below would pass for the wrong reason.
	cli1 := dial("client-1")
	send(cli1, "client-1", `{"type":"ice","from":"client"}`)
	waitRegistered(roleClient)
	if got := collect(cli1); !strings.Contains(got, "SESSION-ONE") {
		t.Fatalf("precondition: viewer 1 should get the buffered offer, got %q", got)
	}

	// Viewer 1 closes the tab: `bye`, then the socket goes away. The browser
	// stays connected throughout — that is the whole point.
	send(cli1, "client-1", `{"type":"bye","from":"client"}`)
	_ = cli1.conn.Close()

	// Viewer 2 joins. The negotiated connection is gone and the worker cannot
	// offer again, so there is nothing legitimate to say to it.
	cli2 := dial("client-2")
	send(cli2, "client-2", `{"type":"ice","from":"client"}`)
	waitRegistered(roleClient)
	if got := collect(cli2); strings.Contains(got, "SESSION-ONE") ||
		strings.Contains(got, `"candidate"`) {
		t.Fatalf("viewer 2 was replayed a dead session's envelopes:\n%s", got)
	}

	// And the session must still work: a worker that CAN re-offer (a fresh
	// process, or a future in-process re-arm) still reaches viewer 2. Without
	// this, "discard everything" would pass by breaking the broker.
	send(brw, "browser", `{"type":"offer","from":"browser","data":{"sdp":"v=0 SESSION-TWO"}}`)
	if got := collect(cli2); !strings.Contains(got, "SESSION-TWO") {
		t.Fatalf("live forwarding broken after bye: got %q", got)
	}
}

// TestWS_DisconnectDiscardsReplay covers the OTHER half of the same rule: a
// peer that vanishes WITHOUT a `bye` (tab closed, network drop, WS 1006) must
// also have its buffer dropped.
//
// This was found only by watching a live deployment, after the bye-only fix was
// already in. A fresh worker joined and was replayed the previous viewer's
// stale `answer` — `replayed: 1` in the broker log — consumed it, reached ICE
// `connected` seconds after boot with nobody watching, and burned its one and
// only session (see docs/findings/one-session-per-worker-process.md) on a peer
// that had been gone for a minute. The next real viewer got nothing.
//
// The direction matters, so this asserts the browser side: a departed CLIENT's
// answer must not be replayed to a newly-joining BROWSER.
func TestWS_DisconnectDiscardsReplay(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/drop-replay"

	dial := func(name string) (*websocket.Conn, chan string) {
		t.Helper()
		c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
		if err != nil {
			t.Fatalf("dial %s: %v", name, err)
		}
		t.Cleanup(func() { _ = c.Close() })
		frames := make(chan string, 64)
		go func() {
			defer close(frames)
			for {
				_, raw, err := c.ReadMessage()
				if err != nil {
					return
				}
				frames <- string(raw)
			}
		}()
		return c, frames
	}
	collect := func(frames chan string) string {
		t.Helper()
		var sb strings.Builder
		timeout := time.After(750 * time.Millisecond)
		for {
			select {
			case f, ok := <-frames:
				if !ok {
					return sb.String()
				}
				sb.WriteString(f)
				sb.WriteByte('\n')
			case <-timeout:
				return sb.String()
			}
		}
	}

	// A browser is present THROUGHOUT. This is what makes the peer-left
	// discard load-bearing: with both peers gone, hub.dropIfEmpty reaps the
	// whole session and the buffers go with it, so a test without this
	// standing browser passes even with the fix reverted.
	brw0, _ := dial("browser-0")
	if err := brw0.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"ice","from":"browser"}`)); err != nil {
		t.Fatalf("browser-0 write: %v", err)
	}
	waitRegisteredIn(h, "drop-replay", roleBrowser, t)

	// A client registers and answers, then drops its socket with NO bye —
	// the ordinary "closed the laptop lid" case.
	cli, _ := dial("client")
	if err := cli.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"answer","from":"client","data":{"sdp":"v=0 DEAD-ANSWER"}}`)); err != nil {
		t.Fatalf("client write: %v", err)
	}
	// Wait for the client to actually REGISTER before closing it, or the
	// answer never reaches the buffer and the assertion below is vacuous.
	waitRegisteredIn(h, "drop-replay", roleClient, t)
	_ = cli.Close()
	waitGoneRole(h, "drop-replay", roleClient, t)

	// Assert on the buffer a future joiner would be served from, with the
	// browser STILL connected. That is the real contract, and it is the only
	// way to observe it: retiring the browser to make room for a "fresh" one
	// empties the session via hub.dropIfEmpty, which would hide the defect.
	h.mu.Lock()
	sess, live := h.sessions[sessionKey{anonymousTenant, "drop-replay"}]
	h.mu.Unlock()
	if !live {
		t.Fatal("session was reaped while the browser was still connected")
	}
	sess.mu.Lock()
	buffered := sess.recent[roleClient]
	nICE := len(sess.recentICE[roleClient])
	sess.mu.Unlock()
	if len(buffered) != 0 || nICE != 0 {
		t.Fatalf("departed client left %d envelopes + %d candidates buffered; "+
			"the next worker to join would be replayed them: %s",
			len(buffered), nICE, buffered["answer"])
	}

	// And the surviving browser must be unaffected — it is still live, and a
	// new viewer joining after this still needs its offer.
	_ = brw0
	_ = collect
}

// waitRegisteredIn blocks until the hub holds a peer in `role` for `sessionID`.
// Polling the hub beats sleeping: registration happens on the SERVER's read
// pump, so there is no client-side event to synchronise on — and a test that
// asserts "nothing was replayed" against a peer that never registered passes
// for entirely the wrong reason.
func waitRegisteredIn(h *hub, sessionID string, role peerRole, t *testing.T) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		sess := h.getOrCreate(anonymousTenant, sessionID)
		sess.mu.Lock()
		_, ok := sess.peers[role]
		sess.mu.Unlock()
		if ok {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("peer %s never registered in %s", role, sessionID)
}

// waitGoneRole blocks until `role` is absent from the session — either because
// the peer unregistered or because the whole session was reaped. Reads the hub
// map directly rather than via getOrCreate, which would RESURRECT a session
// dropIfEmpty had just reaped and report a fresh empty one as "gone".
func waitGoneRole(h *hub, sessionID string, role peerRole, t *testing.T) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		h.mu.Lock()
		sess, live := h.sessions[sessionKey{anonymousTenant, sessionID}]
		h.mu.Unlock()
		if !live {
			return
		}
		sess.mu.Lock()
		_, present := sess.peers[role]
		sess.mu.Unlock()
		if !present {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("peer %s never left %s", role, sessionID)
}

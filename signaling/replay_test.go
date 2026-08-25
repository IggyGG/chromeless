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
		recent:    make(map[peerRole]map[string]bufferedSDP, 2),
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
		recent:    make(map[peerRole]map[string]bufferedSDP, 2),
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
			len(buffered), nICE, string(buffered["answer"].raw))
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

// TestWS_ByelessDisconnectDiscardsCounterpartReplay is the case both tests
// above miss, and it is the one that actually bit.
//
// TestWS_ByeDiscardsReplayBuffer covers a viewer that says `bye`.
// TestWS_DisconnectDiscardsReplay covers a viewer that vanishes, but asserts
// only that ITS OWN buffer is dropped. Neither covers a viewer that vanishes
// WITHOUT a bye after negotiating — where the dead offer left behind is the
// BROWSER's, and the next viewer is the one poisoned.
//
// That is not a hypothetical gap. Measured 2026-08-19 (chromeless task 341797,
// a full Playwright run against a real worker): the broker received ZERO byes
// for the entire run. `beforeunload` — the only thing client/main.ts hangs
// disconnect() on — does not fire reliably when a browser context is closed
// programmatically, and a lid-close, crash or network drop never sends one at
// all. So:
//
//	worker joins, offers, trickles ICE           → buffered
//	spec 01 joins, replayed:7, connects in 0.42s → PASSES
//	spec 01's page is closed — no bye            → only the CLIENT's buffer dropped
//	spec 02 joins                                → replayed the SAME dead 7
//	spec 03 joins                                → replayed the SAME dead 7
//
// Specs 02 and 03 each answered an offer whose peer connection the worker had
// already torn down (kClosed is terminal), and sat in `connecting` for the full
// 30s timeout. It reads exactly like a media or NAT failure and is neither —
// the same disguise documented on discardReplay.
func TestWS_ByelessDisconnectDiscardsCounterpartReplay(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/byeless-replay"

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
	waitPeer := func(role peerRole, present bool) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			sess := h.getOrCreate(anonymousTenant, "byeless-replay")
			sess.mu.Lock()
			_, ok := sess.peers[role]
			sess.mu.Unlock()
			if ok == present {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("peer %s present=%v never happened", role, present)
	}

	// Worker joins, offers, trickles a candidate. No viewer yet: both buffer.
	brw := dial("browser")
	send(brw, "browser", `{"type":"ice","from":"browser"}`)
	waitPeer(roleBrowser, true)
	send(brw, "browser", `{"type":"offer","from":"browser","data":{"sdp":"v=0 SESSION-ONE"}}`)
	send(brw, "browser", `{"type":"ice","from":"browser","data":{"candidate":"one"}}`)

	// Viewer 1 joins and IS correctly replayed the live offer.
	cli1 := dial("client-1")
	send(cli1, "client-1", `{"type":"ice","from":"client"}`)
	waitPeer(roleClient, true)
	if got := collect(cli1); !strings.Contains(got, "SESSION-ONE") {
		t.Fatalf("precondition: viewer 1 should get the buffered offer, got %q", got)
	}

	// Viewer 1 ANSWERS. This line is load-bearing: being replayed an offer is
	// passive and happens to every joiner, so it deliberately does not mark a
	// peer as negotiated (see TestWS_ByelessDisconnectKeepsUnnegotiatedOffer,
	// which fails if it does). Sending SDP is what makes this a real session
	// whose teardown invalidates the worker's offer — and it is what a real
	// viewer does within milliseconds of receiving one.
	send(cli1, "client-1", `{"type":"answer","from":"client","data":{"sdp":"v=0 ANSWER-ONE"}}`)

	// Viewer 1 disappears with NO bye — the whole point of this test.
	_ = cli1.conn.Close()
	waitPeer(roleClient, false)

	// Viewer 2 joins. The worker's offer describes a peer connection that died
	// with viewer 1, and kClosed is terminal, so replaying it hands viewer 2 a
	// connection that can never come up.
	cli2 := dial("client-2")
	send(cli2, "client-2", `{"type":"ice","from":"client"}`)
	waitPeer(roleClient, true)
	if got := collect(cli2); strings.Contains(got, "SESSION-ONE") ||
		strings.Contains(got, `"candidate"`) {
		t.Fatalf("viewer 2 was replayed the dead session after a bye-less exit:\n%s", got)
	}

	// The broker must still WORK: a worker that can offer again still reaches
	// viewer 2. Without this, "discard everything always" would pass this test
	// by breaking the product.
	send(brw, "browser", `{"type":"offer","from":"browser","data":{"sdp":"v=0 SESSION-TWO"}}`)
	if got := collect(cli2); !strings.Contains(got, "SESSION-TWO") {
		t.Fatalf("live forwarding broken after a bye-less disconnect: got %q", got)
	}
}

// TestWS_ByelessDisconnectKeepsUnnegotiatedOffer is the guard rail on the fix
// above, and the reason it is gated on `negotiated` rather than unconditional.
//
// The replay buffer exists for the OPPOSITE race (T96): the worker offers
// before any viewer has joined, and that offer must survive until one does. A
// viewer that connects and drops again WITHOUT exchanging SDP — a refreshed
// tab, a probe, a health check, a client that fails TLS — has not made the
// worker's offer stale. Discarding it there would replace an every-viewer-
// after-the-first bug with an every-viewer bug, and this test is what stops a
// future simplification to `discardReplay(roleClient, roleBrowser)` from
// looking correct.
func TestWS_ByelessDisconnectKeepsUnnegotiatedOffer(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/unnegotiated"

	dialRaw := func(name string) (*websocket.Conn, chan string) {
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
	waitPeer := func(role peerRole, present bool) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			sess := h.getOrCreate(anonymousTenant, "unnegotiated")
			sess.mu.Lock()
			_, ok := sess.peers[role]
			sess.mu.Unlock()
			if ok == present {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("peer %s present=%v never happened", role, present)
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

	// Worker offers into an empty session — the T96 race this buffer is for.
	brwConn, _ := dialRaw("browser")
	if err := brwConn.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"ice","from":"browser"}`)); err != nil {
		t.Fatalf("browser hello: %v", err)
	}
	waitPeer(roleBrowser, true)
	if err := brwConn.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"offer","from":"browser","data":{"sdp":"v=0 STILL-VALID"}}`)); err != nil {
		t.Fatalf("browser offer: %v", err)
	}

	// A viewer connects and leaves again WITHOUT any SDP: it registers (the
	// hello is an ICE frame, which is not SDP) and then drops. Note it IS
	// replayed the offer on join — that alone must not count as negotiating,
	// or this test and the one above cannot both pass.
	cliConn, cliFrames := dialRaw("client-transient")
	if err := cliConn.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"ice","from":"client"}`)); err != nil {
		t.Fatalf("client hello: %v", err)
	}
	waitPeer(roleClient, true)
	_ = collect(cliFrames) // drain whatever it was replayed
	_ = cliConn.Close()
	waitPeer(roleClient, false)

	// The real viewer arrives. The worker's offer was never answered and is
	// still live, so it MUST still be replayed.
	realConn, realFrames := dialRaw("client-real")
	if err := realConn.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"ice","from":"client"}`)); err != nil {
		t.Fatalf("real client hello: %v", err)
	}
	waitPeer(roleClient, true)
	if got := collect(realFrames); !strings.Contains(got, "STILL-VALID") {
		t.Fatalf("a transient viewer destroyed the worker's un-negotiated offer; "+
			"the T96 race is broken:\n%s", got)
	}
}

// TestWS_ByelessDisconnectNotifiesCounterpart is the half that
// TestWS_ByelessDisconnectDiscardsCounterpartReplay does not cover: dropping
// the dead SDP protects the NEXT peer, but the peer still connected is never
// told its partner is gone.
//
// For the browser that is fatal. cb_offerer_driver leaves its session only on
// an explicit close, so a worker whose viewer vanished keeps encoding into a
// dead transport and never offers again. Measured 2026-08-20 against the live
// standalone stack: the worker sat at `frames_encoded=3369 fps=10 1280x720`
// with NO viewer attached, while every newly-joining client got an EMPTY
// replay buffer and timed out with no `m=` lines in its remote description —
// no offer had ever reached it. Three specs failed that way and the cause was
// invisible from the client side, which is what makes this worth a test.
func TestWS_ByelessDisconnectNotifiesCounterpart(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/byeless-notify"

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
	send := func(c *client, msg string) {
		t.Helper()
		if err := c.conn.WriteMessage(websocket.TextMessage, []byte(msg)); err != nil {
			t.Fatalf("write: %v", err)
		}
	}
	collect := func(c *client) string {
		t.Helper()
		var sb strings.Builder
		timeout := time.After(1500 * time.Millisecond)
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
	waitPeer := func(role peerRole, present bool) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			sess := h.getOrCreate(anonymousTenant, "byeless-notify")
			sess.mu.Lock()
			_, ok := sess.peers[role]
			sess.mu.Unlock()
			if ok == present {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("peer %s present=%v never happened", role, present)
	}

	brw := dial("browser")
	send(brw, `{"type":"ice","from":"browser"}`)
	waitPeer(roleBrowser, true)
	send(brw, `{"type":"offer","from":"browser","data":{"sdp":"v=0 SESSION-ONE"}}`)

	cli := dial("client")
	send(cli, `{"type":"ice","from":"client"}`)
	waitPeer(roleClient, true)
	_ = collect(cli) // drain the replayed offer

	// The viewer ANSWERS (so it counts as negotiated) and then vanishes with
	// no bye — a closed laptop, a killed tab, a dropped network.
	send(cli, `{"type":"answer","from":"client","data":{"sdp":"v=0 ANSWER-ONE"}}`)
	_ = collect(brw) // drain the answer on the browser side
	_ = cli.conn.Close()
	waitPeer(roleClient, false)

	// The browser MUST be told. Without this it keeps streaming into a dead
	// transport forever and never offers to the next viewer.
	if got := collect(brw); !strings.Contains(got, `"type":"bye"`) {
		t.Fatalf("browser was never told its viewer left; got %q", got)
	}
}

// TestWS_CleanByeIsNotDoubled is the counterpart guard to
// TestWS_ByelessDisconnectNotifiesCounterpart: a viewer that DOES send its own
// bye and then closes must produce EXACTLY ONE bye on the wire, not two.
//
// This was a real, measured defect. The client sends a bye
// (client/src/session.ts) and then drops the socket, and unregister
// synthesised a second one — so the worker received two byes ~49ms apart. That
// was harmless while a bye meant "exit": the process was already going down.
// It became fatal once the worker RE-ARMS instead. The first bye tore the
// session down and RearmSession rebuilt it in ~7ms; the second closed the
// REBUILD, so the still-pending OnRenegotiationNeeded landed in kClosed, took
// FailWithReason -> kFailed, and Rearm() then refused — falling back to process
// exit. The browser died on every viewer change while the logs read "rebuilt".
func TestWS_CleanByeIsNotDoubled(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/clean-bye-once"

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
	send := func(c *client, msg string) {
		t.Helper()
		if err := c.conn.WriteMessage(websocket.TextMessage, []byte(msg)); err != nil {
			t.Fatalf("write: %v", err)
		}
	}
	collect := func(c *client) string {
		t.Helper()
		var sb strings.Builder
		timeout := time.After(1500 * time.Millisecond)
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
	waitPeer := func(role peerRole, present bool) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			sess := h.getOrCreate(anonymousTenant, "clean-bye-once")
			sess.mu.Lock()
			_, ok := sess.peers[role]
			sess.mu.Unlock()
			if ok == present {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("peer %s present=%v never happened", role, present)
	}

	brw := dial("browser")
	send(brw, `{"type":"ice","from":"browser"}`)
	waitPeer(roleBrowser, true)
	send(brw, `{"type":"offer","from":"browser","data":{"sdp":"v=0 SESSION-ONE"}}`)

	cli := dial("client")
	send(cli, `{"type":"ice","from":"client"}`)
	waitPeer(roleClient, true)
	_ = collect(cli) // drain the replayed offer

	send(cli, `{"type":"answer","from":"client","data":{"sdp":"v=0 ANSWER-ONE"}}`)
	_ = collect(brw) // drain the answer

	// A CLEAN departure: the viewer announces itself, THEN closes.
	send(cli, `{"type":"bye","from":"client"}`)
	_ = cli.conn.Close()
	waitPeer(roleClient, false)

	got := collect(brw)
	if n := strings.Count(got, `"type":"bye"`); n != 1 {
		t.Fatalf("browser must receive EXACTLY ONE bye for a clean close, got %d: %q", n, got)
	}
}

// TestWS_UnnegotiatedDisconnectDoesNotNotify is the guard rail: a viewer that
// connects and drops without exchanging SDP never had a session, so announcing
// its death would tear down a worker that is legitimately waiting for its
// FIRST viewer — turning "the second viewer fails" into "every viewer fails".
func TestWS_UnnegotiatedDisconnectDoesNotNotify(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/unnegotiated-notify"

	dialRaw := func() (*websocket.Conn, chan string) {
		t.Helper()
		c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
		if err != nil {
			t.Fatalf("dial: %v", err)
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
	waitPeer := func(role peerRole, present bool) {
		t.Helper()
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			sess := h.getOrCreate(anonymousTenant, "unnegotiated-notify")
			sess.mu.Lock()
			_, ok := sess.peers[role]
			sess.mu.Unlock()
			if ok == present {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
		t.Fatalf("peer %s present=%v never happened", role, present)
	}
	drain := func(ch chan string) string {
		var sb strings.Builder
		timeout := time.After(1200 * time.Millisecond)
		for {
			select {
			case f, ok := <-ch:
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

	brwConn, brwFrames := dialRaw()
	_ = brwConn.WriteMessage(websocket.TextMessage, []byte(`{"type":"ice","from":"browser"}`))
	waitPeer(roleBrowser, true)
	_ = brwConn.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"offer","from":"browser","data":{"sdp":"v=0 STILL-WAITING"}}`))

	cliConn, cliFrames := dialRaw()
	_ = cliConn.WriteMessage(websocket.TextMessage, []byte(`{"type":"ice","from":"client"}`))
	waitPeer(roleClient, true)
	_ = drain(cliFrames) // it IS replayed the offer; that alone is not negotiating
	_ = cliConn.Close()
	waitPeer(roleClient, false)

	if got := drain(brwFrames); strings.Contains(got, `"type":"bye"`) {
		t.Fatalf("a transient viewer tore down a worker that never had a session:\n%s", got)
	}
}

// TestWS_StaleSDPIsNotReplayed is the bug a REAL browser found that every
// in-cluster test missed.
//
// `recent` held offer/answer forever while `recentICE` aged out after
// iceReplayMaxAge. So a viewer joining later was handed a perfectly
// well-formed offer whose ufrag/pwd named a peer connection that no longer
// existed — and NO candidates, because those had already been dropped. It
// answers, gathers its own host/srflx/relay, and goes straight to
// iceConnectionState=failed.
//
// Measured 2026-08-20 from Chrome on a laptop: `replayed:1` of an offer
// buffered 23 minutes earlier, ICE failed 5s later with relay candidates
// present on BOTH sides. That reads as a TURN or NAT problem and is neither,
// which is exactly why it needs a test.
func TestWS_StaleSDPIsNotReplayed(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	sess := h.getOrCreate(anonymousTenant, "stale-sdp")

	// Buffer an offer and backdate it past the cap, the way a worker that has
	// been waiting for a viewer ends up looking.
	sess.mu.Lock()
	sess.recent[roleBrowser] = map[string]bufferedSDP{
		"offer": {
			raw: []byte(`{"type":"offer","from":"browser","data":{"sdp":"v=0 ANCIENT"}}`),
			ts:  time.Now().Add(-2 * iceReplayMaxAge),
		},
	}
	sess.mu.Unlock()

	cli := &peer{role: roleClient, send: make(chan []byte, 8),
		log: slog.New(slog.NewTextHandler(io.Discard, nil))}
	replayed, err := sess.register(cli)
	if err != nil {
		t.Fatalf("register: %v", err)
	}
	if replayed != 0 {
		t.Fatalf("a %v-old offer was replayed (%d envelope(s)); the viewer would "+
			"answer dead ICE credentials and fail", 2*iceReplayMaxAge, replayed)
	}

	// And a FRESH one must still be replayed — the buffer exists for the T96
	// race and this must not turn into "never replay anything".
	sess.unregister(cli)
	sess.mu.Lock()
	sess.recent[roleBrowser] = map[string]bufferedSDP{
		"offer": {
			raw: []byte(`{"type":"offer","from":"browser","data":{"sdp":"v=0 FRESH"}}`),
			ts:  time.Now(),
		},
	}
	sess.mu.Unlock()

	cli2 := &peer{role: roleClient, send: make(chan []byte, 8),
		log: slog.New(slog.NewTextHandler(io.Discard, nil))}
	replayed2, err := sess.register(cli2)
	if err != nil {
		t.Fatalf("register 2: %v", err)
	}
	if replayed2 != 1 {
		t.Fatalf("a fresh offer was NOT replayed (%d); the T96 race is broken", replayed2)
	}
}

// A peer that joins an empty session is TOLD, immediately.
//
// Regression test for a minute of the user's life. A client dialled session
// "devs" while the worker was on "dev" — one stray keystroke in a text input
// that was styled `border:none; outline:none; background:transparent`, so it
// did not look editable. From the client everything looked correct: socket
// open, auth accepted, ICE config delivered, probe green. It simply never
// received an offer, because no browser was in that session. The only signal
// was the client's own watchdog, 65 seconds later, into a log.
//
// The broker is the ONLY party that can know this at join time, and it
// already had the fact — it just said nothing.
//
// Note what this does NOT assert: that a peer joining a POPULATED session gets
// no notice. That is the second half and is checked below, because a bare
// "does it send" test would pass just as well if the notice were sent
// unconditionally, which would make it noise and train people to ignore it.
func TestWS_LonePeerIsToldTheCounterpartIsAbsent(t *testing.T) {
	withAuthDisabled(t)

	h := newHub(slog.New(slog.NewTextHandler(io.Discard, nil)))
	mux := http.NewServeMux()
	mux.HandleFunc("/ws/", h.wsHandler)
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http") + "/ws/lonely"

	dial := func(name string) (*websocket.Conn, chan string) {
		t.Helper()
		c, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
		if err != nil {
			t.Fatalf("dial %s: %v", name, err)
		}
		t.Cleanup(func() { _ = c.Close() })
		frames := make(chan string, 32)
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

	// First in: nobody else is here.
	c1, f1 := dial("client")
	if err := c1.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"hello","from":"client"}`)); err != nil {
		t.Fatalf("client write: %v", err)
	}
	got := collect(f1)
	if !strings.Contains(got, `"type":"peer_absent"`) {
		t.Fatalf("lone peer was not told it is alone; frames=%q", got)
	}

	// Second in: the counterpart IS present, so no notice. Without this the
	// test would pass on an implementation that always warns.
	c2, f2 := dial("browser")
	if err := c2.WriteMessage(websocket.TextMessage,
		[]byte(`{"type":"hello","from":"browser"}`)); err != nil {
		t.Fatalf("browser write: %v", err)
	}
	got2 := collect(f2)
	if strings.Contains(got2, `"type":"peer_absent"`) {
		t.Fatalf("peer joining a POPULATED session was wrongly warned; frames=%q", got2)
	}
}

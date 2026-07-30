package main

import (
	"io"
	"log/slog"
	"sync"
	"testing"
	"time"
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

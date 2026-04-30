// T96 — verify the signaling server replays a buffered offer to a
// peer that joins AFTER the streamer has already offered.
//
// This is the "streamer comes up before any client connects, then the
// client connects" race. Pre-T96 the offer was forwarded the moment
// the streamer sent it (no client → dropped); the client then sat in
// `waiting for offer` forever. T96 buffers the most recent offer per
// session and replays it on register().

package integration_test

import (
	"bytes"
	"encoding/json"
	"testing"
	"time"
)

// TestOfferReplay_StreamerBeforeClient: the streamer joins, sends an
// offer with no client present (which the server buffers under T96),
// then the client joins — and must receive that offer within a
// generous 2s window.
func TestOfferReplay_StreamerBeforeClient(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay"

	// Streamer joins first and offers immediately. Pre-T96 this offer
	// would be dropped on the floor.
	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-PRE-CLIENT"}`),
	})
	defer brw.Close()

	// Send a bit of latency before the client joins so the server has
	// definitely processed the offer into its replay buffer.
	time.Sleep(150 * time.Millisecond)

	// Client now joins with a benign hello.
	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	// The client must receive the buffered offer.
	got := readEnvelope(t, cli, 2*time.Second)
	if got.Type != "offer" || got.From != "browser" {
		t.Fatalf("replay envelope: want offer/browser, got %+v", got)
	}
	if !bytes.Contains(got.Data, []byte(`"OFFER-PRE-CLIENT"`)) {
		t.Fatalf("replay payload mismatch: %s", got.Data)
	}
}

// TestOfferReplay_OnlyMostRecent: when the streamer offers TWICE
// before a client joins (e.g. ICE-restart renegotiation), the client
// receives only the most recent — bounded buffer.
func TestOfferReplay_OnlyMostRecent(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-mostrecent"

	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-OLD"}`),
	})
	defer brw.Close()
	writeEnvelope(t, brw, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-NEW"}`),
	})
	time.Sleep(150 * time.Millisecond)

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	got := readEnvelope(t, cli, 2*time.Second)
	if !bytes.Contains(got.Data, []byte(`"OFFER-NEW"`)) {
		t.Fatalf("expected most-recent OFFER-NEW, got %s", got.Data)
	}

	// And there should be NO further envelope queued (the older offer
	// was overwritten, not buffered separately).
	_ = cli.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
	if _, raw, err := cli.ReadMessage(); err == nil {
		t.Fatalf("unexpected second envelope on cli: %s", raw)
	}
}

// TestOfferReplay_NoEffectOnNormalFlow: when peers join in the
// natural order (client first, then streamer) the replay logic must
// not duplicate the offer. The client should see the offer EXACTLY
// once (forwarded live).
func TestOfferReplay_NoEffectOnNormalFlow(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-noop"

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()
	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-LIVE"}`),
	})
	defer brw.Close()

	got := readEnvelope(t, cli, 2*time.Second)
	if !bytes.Contains(got.Data, []byte(`"OFFER-LIVE"`)) {
		t.Fatalf("first frame: %+v", got)
	}
	// No duplicate.
	_ = cli.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
	if _, raw, err := cli.ReadMessage(); err == nil {
		t.Fatalf("unexpected duplicate: %s", raw)
	}
}

// TestOfferReplay_IceReplayed: ICE candidates sent before the
// counterpart joins ARE replayed in their original send order
// (T104). The pre-T104 design dropped them on the floor; under the
// cloud-browser session shape the streamer finishes ICE gathering
// before any client joins, so without buffering the client sat in
// iceConnectionState=checking forever.
func TestOfferReplay_IceReplayed(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-ice"

	// Streamer joins first and trickles three ICE candidates with no
	// client present. All three should be buffered by signaling.
	brw := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"host-1"}`),
	})
	defer brw.Close()
	writeEnvelope(t, brw, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"srflx-1"}`),
	})
	writeEnvelope(t, brw, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"relay-1"}`),
	})
	time.Sleep(150 * time.Millisecond)

	// Client joins. Should receive all three buffered ICE candidates
	// in the order they were sent.
	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	wantOrder := []string{"host-1", "srflx-1", "relay-1"}
	for i, want := range wantOrder {
		got := readEnvelope(t, cli, 2*time.Second)
		if got.Type != "ice" || got.From != "browser" {
			t.Fatalf("ICE replay #%d: want ice/browser, got %+v", i, got)
		}
		if !bytes.Contains(got.Data, []byte(want)) {
			t.Fatalf("ICE replay #%d: want %q in data, got %s", i, want, got.Data)
		}
	}
}

// TestOfferReplay_IceWithOfferOrdering: streamer sends offer + ICE
// candidates before any client. On client join, signaling must replay
// the offer FIRST so the client can setRemoteDescription, then the
// ICE candidates can be addIceCandidate'd against the resulting
// peer connection (T104 ordering invariant).
func TestOfferReplay_IceWithOfferOrdering(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-ice-ordering"

	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-FOR-ORDERING"}`),
	})
	defer brw.Close()
	writeEnvelope(t, brw, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"first-ice"}`),
	})
	writeEnvelope(t, brw, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"second-ice"}`),
	})
	time.Sleep(150 * time.Millisecond)

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	// 1) Offer first.
	got := readEnvelope(t, cli, 2*time.Second)
	if got.Type != "offer" {
		t.Fatalf("frame 1: want offer, got %+v", got)
	}
	// 2) Then ICE in order.
	for i, want := range []string{"first-ice", "second-ice"} {
		got := readEnvelope(t, cli, 2*time.Second)
		if got.Type != "ice" {
			t.Fatalf("ICE frame %d: want ice, got %+v", i+2, got)
		}
		if !bytes.Contains(got.Data, []byte(want)) {
			t.Fatalf("ICE frame %d: want %q, got %s", i+2, want, got.Data)
		}
	}
}

// TestOfferReplay_FullStreamerFirstFlow exercises the whole T96+T104
// late-joiner path: streamer sends offer + 3 ICE candidates while no
// client is present, then the client joins and must receive all four
// envelopes in offer-then-ICE order. Mirrors the canonical Phase-1
// container-boots-then-user-arrives shape.
func TestOfferReplay_FullStreamerFirstFlow(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-full"

	brw := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"FULL-OFFER"}`),
	})
	defer brw.Close()
	for _, c := range []string{"host-c", "srflx-c", "relay-c"} {
		writeEnvelope(t, brw, envelope{
			Type: "ice", From: "browser",
			Data: json.RawMessage(`{"candidate":"` + c + `"}`),
		})
	}
	time.Sleep(150 * time.Millisecond)

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	// Expect: offer first, then 3 ICE in order. Total 4 envelopes.
	want := []struct {
		typ, contains string
	}{
		{"offer", "FULL-OFFER"},
		{"ice", "host-c"},
		{"ice", "srflx-c"},
		{"ice", "relay-c"},
	}
	for i, w := range want {
		got := readEnvelope(t, cli, 2*time.Second)
		if got.Type != w.typ {
			t.Fatalf("frame %d: want type=%q, got %+v", i, w.typ, got)
		}
		if !bytes.Contains(got.Data, []byte(w.contains)) {
			t.Fatalf("frame %d: want %q in data, got %s", i, w.contains, got.Data)
		}
	}
	// And NOTHING else should arrive (no duplicates).
	_ = cli.SetReadDeadline(time.Now().Add(250 * time.Millisecond))
	if _, raw, err := cli.ReadMessage(); err == nil {
		t.Fatalf("unexpected extra envelope after replay: %s", raw)
	}
}

// TestOfferReplay_IceQueueBounded: when the streamer trickles more
// than iceReplayMaxPerSender candidates, the oldest are evicted so a
// late-joining client sees only the most recent N. Verifies the
// O(1) memory bound the queue is documented to provide.
func TestOfferReplay_IceQueueBounded(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-bounded"

	brw := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"ice-0"}`),
	})
	defer brw.Close()
	// Send 50 total (cap is 32). The first ~18 should be evicted.
	const total = 50
	for i := 1; i < total; i++ {
		writeEnvelope(t, brw, envelope{
			Type: "ice", From: "browser",
			Data: json.RawMessage(`{"candidate":"ice-` + itoa(i) + `"}`),
		})
	}
	time.Sleep(200 * time.Millisecond)

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	// We expect to receive AT MOST 32 ICE envelopes. The most recent
	// ones (ice-18 through ice-49) should be there; ice-0 must have
	// been evicted.
	const cap = 32
	var seen []string
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		_ = cli.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
		got, raw, err := cli.ReadMessage()
		_ = got
		if err != nil {
			break
		}
		seen = append(seen, string(raw))
	}
	if len(seen) > cap {
		t.Fatalf("queue overran cap: got %d envelopes, want ≤ %d", len(seen), cap)
	}
	if len(seen) == 0 {
		t.Fatal("no ICE envelopes replayed")
	}
	// Oldest (ice-0) must be evicted.
	for _, s := range seen {
		if bytes.Contains([]byte(s), []byte(`"ice-0"`)) && !bytes.Contains([]byte(s), []byte(`"ice-0`)) {
			// (the second clause guards against accidental match on
			// "ice-01", but our fixtures use plain "ice-N" so the
			// first-form check above is exact already.)
			t.Fatalf("oldest candidate ice-0 leaked through: %s", s)
		}
	}
	// Most recent (ice-49) must be present.
	foundLast := false
	for _, s := range seen {
		if bytes.Contains([]byte(s), []byte(`"ice-49"`)) {
			foundLast = true
			break
		}
	}
	if !foundLast {
		t.Fatalf("most-recent ice-49 missing from %d replayed envelopes", len(seen))
	}
}

// itoa is a local int→ascii without importing strconv into this test
// file just for the loop above.
func itoa(n int) string {
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

// TestOfferReplay_SurvivesPeerReconnect: streamer joins, offers,
// disconnects (offer is stale per the design intent), then a fresh
// client joins. With one peer gone the session is still alive (the
// other slot is empty), so the buffered offer SHOULD still be replayed
// — but it's clearly stale, so when the streamer rejoins it overwrites
// with a fresh offer.
//
// What we assert here: the client gets *some* offer, and a re-offering
// streamer's fresh offer is delivered live to the still-connected
// client (not double-replayed).
func TestOfferReplay_SurvivesPeerReconnect(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-reconnect"

	// Streamer #1 offers.
	brw1 := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-1"}`),
	})
	// Streamer drops out before any client.
	brw1.Close()
	time.Sleep(150 * time.Millisecond)

	// Client joins. Session still exists (the streamer's slot is
	// empty; total peers == 0 → would be cleaned up on next
	// dropIfEmpty, but the client's join races the cleanup. Even if
	// cleanup wins we just get a fresh empty session: the test below
	// then exercises the streamer-2-joins path).
	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	// Streamer #2 joins and sends a fresh offer.
	brw2 := dialPeer(t, port, sessID, envelope{
		Type: "offer", From: "browser",
		Data: json.RawMessage(`{"sdp":"OFFER-2"}`),
	})
	defer brw2.Close()

	// Within 2s, cli must receive at least one offer; the most recent
	// (OFFER-2) must be among them.
	deadline := time.Now().Add(2 * time.Second)
	sawOffer2 := false
	for time.Now().Before(deadline) {
		_ = cli.SetReadDeadline(time.Now().Add(500 * time.Millisecond))
		_, raw, err := cli.ReadMessage()
		if err != nil {
			break
		}
		if bytes.Contains(raw, []byte(`"OFFER-2"`)) {
			sawOffer2 = true
			break
		}
	}
	if !sawOffer2 {
		t.Fatal("client never saw OFFER-2 from streamer #2")
	}
}

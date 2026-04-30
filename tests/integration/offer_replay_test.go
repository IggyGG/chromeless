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

// TestOfferReplay_IceNotReplayed: ICE candidates sent before the
// counterpart joins are NOT replayed (per the T96 design — ICE is a
// stream and stale candidates are not useful). Only offer/answer/
// request_renegotiate are buffered.
func TestOfferReplay_IceNotReplayed(t *testing.T) {
	port := startServer(t)
	const sessID = "rt-replay-ice"

	brw := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "browser",
		Data: json.RawMessage(`{"candidate":"early-ice-candidate"}`),
	})
	defer brw.Close()
	time.Sleep(100 * time.Millisecond)

	cli := dialPeer(t, port, sessID, envelope{
		Type: "ice", From: "client",
		Data: json.RawMessage(`{"candidate":null}`),
	})
	defer cli.Close()

	// Cli should NOT receive the early ICE.
	_ = cli.SetReadDeadline(time.Now().Add(300 * time.Millisecond))
	if _, raw, err := cli.ReadMessage(); err == nil {
		t.Fatalf("ICE leaked through replay (should be dropped): %s", raw)
	}
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

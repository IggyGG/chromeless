// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// cb_signaling_transport.h — the seam between "how envelopes move" and
// "what the peer does with them".
//
// # Why this exists
//
// This project is meant to work for people with none of the triform
// infrastructure. Today the only way an envelope reaches a peer is
// SignalingWsClient — a chromium-network-service WebSocket. That is a fine
// default and stays the default; it is not a reasonable REQUIREMENT to
// impose on someone whose broker speaks something else, or on a test that
// wants to drive the offerer without a network stack at all.
//
// # What this is NOT
//
// Not a plugin system, not a registry, not a factory. Deliberately.
// SignalingWsClient's full surface is Connect / Send / SendText /
// Disconnect / is_connected / is_closing, and extracting all of it as
// virtuals would export implementation details (SendText exists so tests
// can inject malformed frames) into a contract third parties would then
// depend on.
//
// So the interface is exactly what the CONSUMER needs. Checked, not
// assumed: cb_offerer_driver.cc — the only module that talks to the client
// on the hot path — calls exactly one method, `Send()`, at four sites
// (lines 788, 809, 818, 1192). Everything else (Connect, Disconnect, the
// state accessors) is called by the embedder, which is the code that owns
// the concrete client anyway and does not need an abstraction to reach it.
//
// One virtual. If a second is genuinely needed later, adding it is a
// smaller change than removing four that were never used.
//
// # Threading
//
// Same contract as SignalingWsClient: UI thread only. An implementation
// that wants to be called from elsewhere must post, and must say so.

#ifndef CAPTURE_SIGNALING_CB_SIGNALING_TRANSPORT_H_
#define CAPTURE_SIGNALING_CB_SIGNALING_TRANSPORT_H_

#include "capture/signaling/cb_wire_envelope.h"

namespace cloud_browser {
namespace signaling {

// The outbound half of the signaling channel.
//
// Inbound is NOT part of this interface: envelopes arrive through
// SignalingClientObserver, which is already an abstract observer and
// already the seam for that direction. Duplicating it here would create
// two ways to say the same thing.
class SignalingTransport {
 public:
  virtual ~SignalingTransport() = default;

  // Send |envelope| to the peer. Returns false if the transport is not in
  // a state to send, or if encoding rejected the envelope.
  //
  // Implementations MUST rewrite `from` to PeerRole::kBrowser before
  // emitting — the browser is the sole emitter on this side, and the
  // broker rewrites server-side too, so an implementation that forwards a
  // caller-supplied `from` verbatim will disagree with both. This is a
  // contract requirement rather than something the interface can enforce;
  // SignalingWsClient::Send does it and any replacement must as well.
  virtual bool Send(const Envelope& envelope) = 0;
};

}  // namespace signaling
}  // namespace cloud_browser

#endif  // CAPTURE_SIGNALING_CB_SIGNALING_TRANSPORT_H_

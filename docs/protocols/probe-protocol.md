# Connection-quality probe (T102)

How the client measures available bandwidth + RTT before establishing
a WebRTC session, and how that estimate seeds the streamer's encoder
bitrate ceiling so the first few seconds aren't degraded while
libwebrtc's BWE ramps up.

> **Status:** v1, ships with T102.

---

## TL;DR

1. Client opens 3–5 small HTTP GETs to `/probe?size=N` on the
   signaling host, times each round-trip → median RTT.
2. Client makes one larger sized GET (256 KiB) and one POST (256 KiB)
   to derive a rough downlink/uplink bps.
3. Client sends `{type: "probe_result", from: "client", data: {rtt_ms,
   downlink_kbps, uplink_kbps}}` over the signaling websocket *after*
   its hello frame and *before* the streamer's offer arrives (or
   whenever, the streamer adapts on receipt).
4. Streamer reads the envelope and calls
   `RTCRtpSender.setParameters({encodings:[{maxBitrate: cap}]})` on
   the video sender, where `cap = min(uplink_kbps, downlink_kbps) ×
   0.8 × 1000` (kbps → bps with 20% headroom).

The probe is best-effort. On any failure (HTTP error, parse error,
auth rejection) the client connects without a `probe_result` envelope
and the streamer keeps its default ceiling.

## Why HTTP, not WebRTC

A "real" probe would open a temporary `RTCPeerConnection` against a
Pion peer in signaling, exchange enough RTP for libwebrtc's BWE to
settle, and read `availableOutgoingBitrate` from `getStats()`. We
rejected that for v1:

- Pion-on-signaling is a substantial dep tree we don't otherwise
  need until the Phase-4 SFU work. T102's deliverable is "starts
  encoding at the right bitrate" — HTTP-derived numbers satisfy
  that goal at a fraction of the complexity.
- libwebrtc's BWE needs ~5–10 s to converge meaningfully. A 2-second
  WebRTC probe gives noisy numbers; a 2-second HTTP probe gives
  accurate-to-within-20% numbers because TCP+ACKs surface RTT and
  bandwidth quickly.
- Symmetric NATs that would block the WebRTC probe also block the
  later real session; nothing is gained by failing-early via WebRTC.

When SFU lands and a Pion peer exists, T102 may graduate to the
WebRTC path. The client API (`estimateConnectionQuality`) keeps the
same shape; only the implementation under it changes.

---

## Wire format

### `/probe` endpoint (signaling)

```
GET  /probe?size=<bytes>     → 200 OK, <bytes> of zeros
POST /probe                  → 200 OK, JSON {received: <bytes>}
```

Response headers on both:
- `Content-Type: application/octet-stream` (GET) / `application/json` (POST)
- `Cache-Control: no-store`
- `Access-Control-Allow-Origin: *` (CORS for cross-origin client/static-host)

Limits:
- `size` clamped to `[1, 1048576]` (1 byte to 1 MiB) per request.
  Without the cap, a single client could exhaust the host's egress.
- POST body limit: 1 MiB. Larger requests get 413.
- Auth: required when `CBWRTC_AUTH_PUBKEY` is set (T48). Token in
  `?token=…` query string, same as `/ws/`. Consistent with
  `/turn-credentials` and `/admin/revoke`. The probe's lack of auth
  in dev mode is fine — auth-disabled deploys are local/CI only.

### `probe_result` envelope (client → signaling → streamer)

```jsonc
{
  "type": "probe_result",
  "from": "client",
  "data": {
    "rtt_ms":         62,      // median of N HTTP RTT samples
    "downlink_kbps":  10240,   // derived from sized GET
    "uplink_kbps":    4800,    // derived from sized POST
    "samples":        5         // number of RTT samples (informational)
  }
}
```

Forwarded peer-to-peer like every other envelope. The signaling
server's `validTypes` set includes `probe_result`. It is **NOT**
replayable (T96): the probe is a fresh measurement per connect,
stale numbers are worse than no numbers.

The streamer treats receipt as "hint, not contract." A missing
envelope means "use default ceiling." A present envelope means
"set the encoder ceiling to ~80% of the lower of uplink/downlink."

---

## Streamer-side bitrate seeding

```js
sender.setParameters({
  encodings: [{
    maxBitrate: Math.min(uplink_kbps, downlink_kbps) * 0.8 * 1000,
  }],
});
```

The 0.8 multiplier is the headroom the encoder needs for protocol
overhead (RTP headers, FEC, ICE keepalives, audio). Without it,
encoding at the full estimated rate fills the network and
libwebrtc's BWE pulls back hard within seconds.

Floor / ceiling clamps:
- min: 200 kbps (lower than this and the encoder produces unwatchable
  output; better to refuse than misrepresent).
- max: 8 Mbps (above this and there's no benefit at our v1 1080p30
  target; reservations should go to the buffer).

---

## Failure modes

| Probe outcome | Streamer behaviour | Why |
|---|---|---|
| `probe_result` received, sane numbers | setParameters → estimated bitrate | Common case. |
| `probe_result` not received within first 3 s of pc.connectionState=connected | keep default 4 Mbps ceiling | Probe failed silently (auth, network blip). Default is safe for typical home broadband. |
| `probe_result.uplink_kbps < 200` | setParameters with 200 kbps floor | Probe says the connection is unwatchable; we still try, libwebrtc's BWE pulls down further if needed. |
| Multiple `probe_result` envelopes in one session | last one wins | Renegotiation may want a fresh probe; the streamer accepts updates. |

---

## Files

- `client/src/probe.ts` — `estimateConnectionQuality`. Pure, no DOM.
  Fully unit-tested with a fake `fetch`.
- `client/src/probe.test.ts` — vitest cases.
- `client/main.ts` — calls `estimateConnectionQuality` before
  `rws.connect()`; emits the `probe_result` envelope on `ws.open`
  along with the hello frame.
- `signaling/probe.go` — `/probe` GET + POST handlers; bandwidth cap.
- `signaling/server.go` — `validTypes` includes `probe_result`;
  endpoint registered next to `/healthz`, `/turn-credentials`.
- `capture/streamer-page/streamer.js` — `ws.onmessage` handler for
  `probe_result`; calls `setParameters` on the active video sender.

## Cross-references

- T48 — auth (probe endpoint takes the same `?token=` query param).
- T58 — BWE adapter (the encoder-factory hook the probe seeds).
- T77 — simulcast (when on, every layer's `maxBitrate` is scaled by
  the same probe-derived ceiling fraction; layer 0 gets the largest
  share).
- T96 — replay buffer (probe_result is **excluded** from replayable
  types; it's a fresh measurement per connect).

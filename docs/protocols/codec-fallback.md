# Codec fallback negotiation

How the chromeless client decides what video codec is in
play after the offer/answer exchange, surfaces fallbacks to the user
and to telemetry, and converts a "no codec at all" outcome into a
hard error rather than a stuck-but-silent peer connection.

> **Status:** v1 (T54). Built on top of T30 (SDP munging) and T34
> (answerer-role flow).

## Why this exists

T30 makes the client _ask_ for VP9 first. T35/T36 give the streamer
software encoders for VP9 and H.264. But asking and getting are not
the same — codec negotiation depends on what libwebrtc actually
compiles in, what the streamer's encoder factory exposes, and what
we strip when we munge the SDP. Until T54, three failure modes were
indistinguishable in the debug panel:

1. We wanted VP9 and got VP9. ✅
2. We wanted VP9 but the streamer fell back to H.264. ⚠️ Useful to know:
   diagnoses "why is my latency / quality off."
3. The streamer answered with no usable video codec at all. ❌
   Today this leaves the peer connection in `have-remote-answer` with
   no track delivery and no error — invisible in the UI.

T54 adds an explicit inspector that runs after `setLocalDescription`
on the answerer side (post-T34) or after `setRemoteDescription` on
the offerer side (pre-T34 / streamer-driven) and classifies the
outcome.

## Algorithm

`classifyNegotiation(sdp, preferred = DEFAULT_PREFERRED)` walks the
m=video section of the supplied SDP:

1. Extract the m=video PT list.
2. For each PT, look up its `a=rtpmap:<pt> CODEC/<clock>` entry.
3. Drop rtx, red, ulpfec, flexfec from the codec list.
4. The **first** remaining codec is the negotiated codec.
5. Compare against `preferred` (case-insensitive):
   - `preferred[0] == negotiated` → outcome `"ok"`
   - `negotiated` in `preferred` but not at the top → outcome `"fallback"`
   - `negotiated` is `null` (no video codec) → outcome `"no_codec"`
   - `negotiated` is set but absent from `preferred` → outcome `"fallback"`
     (degraded but still working — the streamer chose something for us)

Default preference list is `["AV1", "VP9", "VP8", "H264"]`,
tightest-latency / highest-quality first. The list is per-client,
not negotiated — clients on different OS/browser combinations will
present different lists once we differentiate them in Phase 2.

## Side effects

`main.ts` calls `classifyNegotiation` after `setLocalDescription` of
the answer and:

- **Always** logs a `negotiated_codec=...` line to the debug panel.
- On `fallback`: logs at WARN level so the operator notices.
- On `no_codec`: tears down the connection and surfaces
  `connection-failed: no video codec negotiated` in the status pill,
  rather than letting the pc sit indefinitely.
- Fires a one-time `codec_fallback` event on the next `stats` data
  channel sample so server-side observability (T38 metrics, T42
  stats sample emission) sees it. Wire envelope:

  ```jsonc
  { "v": 1, "t": 1730290000123,
    "event": "codec_fallback",
    "data": { "preferred": ["AV1","VP9","VP8","H264"],
              "negotiated": "H264",
              "video_codecs": ["H264","VP8"] }
  }
  ```

  This is _not_ a separate channel; it rides the existing T42 stats
  channel. The server treats it as opaque data and forwards to the
  Prometheus scraper via T38 follow-up work.

## Server side

The streamer's responsibility, _before_ T54 lands its server-side
counterpart:

- If the streamer's encoder factory has VP9 disabled, it MUST NOT
  include VP9 PTs in its offer. (T35's libvpx wiring already
  guarantees this when libvpx isn't compiled in.)
- If the streamer cannot produce any of the codecs the client offers,
  it should answer with `m=video 0 ...` (port 0, inactive). The
  client will see `no_codec` and surface a hard error.
- Phase-3 follow-up: emit a server-side metric when codec fallback
  happens, so an operator can correlate with the client-side
  `codec_fallback` event.

## Files

- `client/src/codec-negotiate.ts` — pure SDP inspector + classifier.
- `client/src/codec-negotiate.test.ts` — 12 vitest cases over
  hand-crafted answer SDPs (VP9-only, H264-only, both-with-VP9-first,
  port 0, missing rtpmap, no m=video, default-prefs, case
  variations, unexpected codec).
- `client/main.ts` — call site after `setLocalDescription(answer)`.
- `docs/protocols/codec-fallback.md` — this file.

## Cross-references

- T30 — SDP munging (sets the preferred order on the wire).
- T34 — role flip (decides which SDP to inspect).
- T35 / T36 — actual encoders behind the encoder factory.
- T42 — stats channel (carries the `codec_fallback` event).
- T38 — Prometheus scraping (consumes the event downstream).

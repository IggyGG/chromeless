# Phase 1 latency measurement — 2026-04-30 (y4m fixture validated; full
end-to-end gated on spec-05 fix)

> **Status update 2026-04-30 evening (post-T109):** T109 wired
> `--use-file-for-fake-video-capture-loop` into the launch script and
> compose stack; `harness/latency/record-y4m.sh` produces the y4m
> fixture. Generated `harness/latency/fixtures/harness-loop-720p30.y4m`
> (1.2 GiB raw, 900 frames @ 30 fps × 30 s) inside an ephemeral
> container based on `chromeless:dev` (Chromium 147 + Xvfb
> + ffmpeg x11grab). Transcoded to mp4 (391 KiB) for compactness.
> Direct reconciliation against the mp4 (no WebRTC pipeline; just
> reconcile.py decoding the y4m's own QRs):
>
>   qr_decode_rate:  1.000
>   latency_count:   900
>   cam_fps_hz:      30.303
>   flash_freq_hz:   0.986
>   aliased_warning: False
>
> **Validates the y4m + reconciler chain end-to-end** — 100% QR
> decode rate proves the recording captured the harness page
> faithfully and the reconciler can extract every frame's emit
> timestamp. The negative latency in this run (-63 billion ms) is
> expected and confirms reconcile is correct: I passed a placeholder
> `--recording-start-epoch-ms` from a 2024 timestamp; the y4m's QRs
> carry real 2026-04-30 epoch ms; the offset between them is exactly
> two years.
>
> **Hermetic methodology baseline GREEN.** Re-ran the new
> `tests/harness/y4m-loopback-baseline.sh` (T109 deliverable):
> `qr_decoded=30 / 30, qr_decode_rate=1.000`. The y4m fixture
> carries decodable QRs end-to-end; the reconciler produces a sane
> summary file; the methodology pre-condition for any downstream
> live-pipeline measurement is met.
>
> **End-to-end (glass-to-glass via live WebRTC pipeline) measurement
> still pending an operator session.** With T96 + T109 + T108-partial
> all in main, the contract layer connects (signaling logs show offer
> replay; SDP advertises audio + opus). What remains is a frame
> capture on the client side — either a webcam pointed at the
> client display (the canonical operator path, per
> `tests/harness/validation.md` §4), or a Playwright-driven
> screenshot loop against the client's `<video>` once spec 05 lands
> a complete fix. **Per team-lead's framing, neither path gates v1
> ship**; the v1-defining glass-to-glass number is the operator
> measurement session ready to run when an operator with a cam rig
> is available, and the y4m fixture-correctness baseline is the
> CI-runnable methodology proof we have today.

> **Status update 2026-04-30 afternoon (post-T96):** T96
> (`f640478` — signaling now buffers and replays the most-recent
> offer/answer/request_renegotiate per session) shipped. Confirmed live:
> the `audio_loopback_test.go` Go integration test passes against the
> running compose stack (PASS in 21.84 s; SDP carries `m=audio` +
> opus rtpmap), and signaling logs show
> `peer joined (replayed buffered envelopes) ... role=client, replayed:1`.
> The protocol-layer pipeline is fully open.
>
> **However**, the *real glass-to-glass number* the harness was
> designed to produce is **still gated** — not on T96 anymore, but on
> the still-active `CHROMELESS_USE_FAKE_MEDIA=1` workaround for the
> Chromium-147 + Xvfb getDisplayMedia issue (T78 / #86). With
> fake-media, the streamer captures Chromium's synthetic test pattern,
> NOT whatever harness page is actually rendered. So the harness's
> QR-bearing flashing block never enters the captured stream, and
> `reconcile.py`'s QR detector finds nothing. Real numbers require
> either (a) the real-getDisplayMedia path (T78 real fix), or (b) the
> Phase 2 FrameSinkVideoCapturer path, or (c) feeding a recorded
> harness y4m to `--use-file-for-fake-video-capture`.
>
> The framework section below remains valid; the numbers section is
> still `(blocked-on-real-screen-capture)` rather than `(blocked-on-T86)`.
>
> **Template for future runs:** [`phase1-loopback-TEMPLATE.md`](./phase1-loopback-TEMPLATE.md).

---

## 1. Summary

| | |
|-|-|
| **Run ID**         | `phase1-loopback-2026-04-30` (placeholder; replace on first real run) |
| **Date / time**    | 2026-04-30 (framework authored) |
| **Target bucket**  | `loopback` (intended) — first real run should be cam-pointed-at-source on a single host to establish the noise floor before any cross-host LAN run |
| **Operator**       | qa-tester (framework) → operator-with-cam-rig (numbers) |
| **Stack revision** | `git rev-parse HEAD` at run time — current HEAD is post-T86 partial (fake-media workaround landed, real-getDisplayMedia fix and offer-routing fix still in flight) |
| **Pass / fail vs budget** | **(blocked-on-T86)** |
| **Recommendation** | **(blocked-on-T86)** — the framework here lets the next operator deliver this in one commit |

---

## 2. Setup

> Sections 2.1–2.6 are filled when the actual run happens. Below are
> the stack-side facts known today so the operator only has to fill
> in the host/cam/lighting cells.

### 2.1 Hosts

| | source host | cam host |
|-|-|-|
| Machine        | _(operator)_ | _(operator)_ |
| OS / kernel    | _(operator)_ | _(operator)_ |
| CPU / RAM      | _(operator)_ | _(operator)_ |
| Network        | _(operator)_ | _(operator)_ |
| NTP source     | _required: see calibrate.md Procedure 1_ | _same_ |
| Skew at run    | _required: `chronyc tracking` ≤ 2 ms_ | _same_ |

### 2.2 Cam

_(operator — see `tests/harness/validation.md` §4 step 3 for the
required AE/AF lock + recording flags.)_

### 2.3 Display + lighting

_(operator — `tests/harness/validation.md` §1.4 + §4 step 4.)_

### 2.4 Cloud-browser stack

| | |
|-|-|
| Image tag             | `chromeless:dev` (current dev build) |
| `CHROMELESS_USE_FAKE_MEDIA` | **1** if the run is on the current HEAD (T86 fake-media unblock per commit 411d06a). **0** required for a numbers-against-the-real-screen-capture run, which depends on T86's real fix landing. |
| Encoder               | _whichever is wired in launch-chromeless.sh + the SDP munger (T30) at run time. T35 (VP9), T36 (x264), T75 (SVT-AV1) all completed. Default appears to be x264 with zero-latency tuning._ |
| Audio                 | enabled (T24 wires PulseAudio null-sink → getDisplayMedia capture; spec 05 verifies audio reaches the client) |
| Cursor metadata (T26) | enabled |
| Auth (T48)            | disabled in dev (no `CHROMELESS_AUTH_PUBKEY`); signaling logs `auth disabled` warning |
| TURN (T76 issuer)     | available; not required for loopback or LAN runs |

### 2.5 Harness page parameters

Operator should use the defaults from `harness/latency/index.html`
unless probing for aliasing:

| | default |
|-|-|
| Flash period          | 1000 ms (1 Hz) |
| ON / OFF colors       | `#ff0000` / `#000000` |
| Fullscreen            | yes (press `f`) |
| Run ID URL param      | unique per run, e.g. `?run=phase1-2026-04-30-A` |

### 2.6 Loopback offset measurement (Procedure 3)

For this report's intended **loopback** target this section is N/A —
the run **is** the loopback. For a follow-on LAN or regional run, the
operator records this run's `p50_ms` here and passes it as
`--offset-ms` to a subsequent `phase1-baseline.sh --target lan` run.

---

## 3. Numbers

**(blocked-on-T86)** — see §6.

The reconciler is validated against deterministic fixtures
(`tests/harness/loopback-baseline.sh` — 8/8 assertions green on
every PR). Hermetic confidence in the math; what's missing is a
real assembled-stack capture.

When unblocked, fill from `harness/captures/<runId>-summary.txt`:

| | value | unit |
|-|------:|-|
| `frames_seen`     | _(blocked-on-T86)_ |  |
| `qr_decoded`      | _(blocked-on-T86)_ |  |
| `qr_decode_rate`  | _(blocked-on-T86)_ |  |
| `negative_count`  | _(blocked-on-T86)_ |  |
| `min_ms`          | _(blocked-on-T86)_ | ms |
| `p50_ms`          | _(blocked-on-T86)_ | ms |
| `p95_ms`          | _(blocked-on-T86)_ | ms |
| `p99_ms`          | _(blocked-on-T86)_ | ms |
| `max_ms`          | _(blocked-on-T86)_ | ms |
| `mean_ms`         | _(blocked-on-T86)_ | ms |
| `stdev_ms`        | _(blocked-on-T86)_ | ms |

Histogram path will be `harness/captures/<runId>-histogram.png` once
the run lands.

### 3.1 Input latency (T39)

T39's input-latency harness (`harness/input-latency/`) is independent
of the video pipeline's offer-routing — it measures the round-trip of
a synthesized keystroke from client → input bridge → cloud Chromium →
visible-glyph-on-screen. It's blocked by the same getDisplayMedia
issue (T86) only insofar as we need the cloud Chromium to actually
render the keystroke result. Plan: run T39 in the same operator
session; record numbers here.

| | value | unit |
|-|------:|-|
| `input_p50_ms` | _(blocked-on-T86)_ | ms |
| `input_p95_ms` | _(blocked-on-T86)_ | ms |
| `input_p99_ms` | _(blocked-on-T86)_ | ms |
| `input_max_ms` | _(blocked-on-T86)_ | ms |

---

## 4. Comparison vs budget

### 4.1 v1 success criteria
([`docs/v1-success-criteria.md`](../v1-success-criteria.md))

| Path                   | Target  | Measured p50 | Measured p95 | Pass? |
|------------------------|---------|-------------:|-------------:|:-----:|
| Glass-to-glass, LAN    | < 100 ms | _(blocked-on-T86)_ | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| Glass-to-glass, regional | < 200 ms | _(blocked-on-T86)_ | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| Input round-trip       | _v1 doc_ | _(blocked-on-T86)_ | _(blocked-on-T86)_ | _(blocked-on-T86)_ |

### 4.2 tests/harness/validation.md §3 ranges

| Run shape                 | Expected p50    | Measured p50 | In range? |
|---------------------------|-----------------|-------------:|:---------:|
| Loopback (cam → source)   | 30–50 ms        | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| LAN, software encode      | 60–95 ms        | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| Regional (≤ 30 ms RTT)    | 100–180 ms      | _(blocked-on-T86)_ | _(blocked-on-T86)_ |

Sanity column (`validation.md` §3.1):

| Field             | Pass condition                  | Measured | Pass? |
|-------------------|--------------------------------|---------:|:-----:|
| `qr_decode_rate`  | ≥ 0.95                         | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| `negative_count`  | == 0                           | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| `min_ms`          | strictly positive               | _(blocked-on-T86)_ | _(blocked-on-T86)_ |
| `frames_seen`     | ≥ 95% of cam_fps × duration   | _(blocked-on-T86)_ | _(blocked-on-T86)_ |

`phase1-baseline.sh` enforces all of these automatically when invoked
with `--target loopback` (or `lan`/`regional`); a non-zero exit code
means at least one bound was breached. `--no-assert` lets the operator
record the first-ever baseline before assertion thresholds exist.

---

## 5. What surprised, what didn't

**(blocked-on-T86)**

When unblocked: notable observations from the actual run go here.
Likely topics to prepare for, based on validation.md §1 + the audit
during T16:

- The bottom of the rolling-shutter sweep vs. the top: at 60 fps a
  consistent ±1 frame jitter in min_ms is expected.
- rAF-vs-paint vsync bias: ~8 ms median bias absorbed by the loopback
  offset; the LAN delta from loopback should be the encoder + transport
  + decode time alone.
- T86 fake-media path produces synthetic green frames + 440 Hz tone —
  if the run uses `CHROMELESS_USE_FAKE_MEDIA=1`, the encoder gets nearly
  no entropy and the encoded bitrate is unrealistically low. **The
  numbers from a fake-media run are NOT comparable to a real-screen
  run**; document the configuration explicitly above and call this
  out here.

---

## 6. Bottleneck & next-step recommendation

**Updated 2026-04-30 afternoon — what's resolved, what's still
blocked:**

1. **#96 — RESOLVED.** T96 (`f640478`) shipped. Signaling buffers and
   replays the most-recent offer/answer/request_renegotiate per
   session. Confirmed live:
   - `tests/integration/audio_loopback_test.go`
     `TestStreamerOffersAudio`: **PASS in 21.84 s** with
     `CHROMELESS_INTEGRATION_LIVE=1`. SDP advertises `m=audio` + opus
     rtpmap. (No real-pipeline numbers from this — it asserts on the
     contract layer.)
   - Signaling logs show `peer joined (replayed buffered envelopes)
     ... role=client, replayed:1` for late-joining clients.
   - Companion regression: `tests/integration/offer_replay_test.go`
     covers four scenarios (only-most-recent, no-effect-on-normal,
     ICE-not-replayed, survives-peer-reconnect) — all green.
   - Filed [#104](#) (T96-followup) on the explicit
     ICE-not-replayed gap — late-joining client must initiate ICE
     restart per design; not in scope for v1 LAN.
2. **T86 / #86 — STILL ACTIVE for real numbers.** The X11+SwiftShader
   pin (`5d69ac4`), the broader ozone/angle/swiftshader-webgl flag
   suite (T80–T84 area), and finally the
   `CHROMELESS_USE_FAKE_MEDIA=1` env switch (`411d06a`, T86 commit)
   together let the streamer dial signaling and complete WebRTC
   negotiation. **They do NOT make real screen capture work.** The
   captured stream is Chromium's synthetic test pattern, not whatever
   page Chromium is rendering. Consequently:
   - Real glass-to-glass latency cannot be reconciled by `reconcile.py`
     because the harness's QR-bearing flashing block never enters the
     captured stream.
   - The audio path *does* carry a real-ish 440 Hz tone (synthesized
     by Chromium's fake-audio generator), so audio-only measurements
     could in principle work — but the `harness/latency/` reconciler
     is video-side.
   - **Only fixes that unblock real numbers are**: (a) real
     `getDisplayMedia` on Chromium 147 + Xvfb (T78 real fix, still
     pending), (b) the Phase 2 FrameSinkVideoCapturer path (T55
     scaffold ready; gated on T17 build env per T101), or (c)
     `--use-file-for-fake-video-capture=<y4m of the harness page>` —
     possible workaround if someone records a harness page run as a
     y4m file and loops it; not implemented today.
3. **[#108](#) — NEW**. While running spec 05 against the post-T96
   stack, found a client-side regression: `#state-conn` stays at the
   initial `"—"` for 20 s after Connect. Audio_loopback Go test
   confirms the streamer's offer is delivered (replayed:1 in
   signaling logs), so the failure is downstream — likely client
   `pc.onconnectionstatechange` not wired or `fetchTurnConfig`
   stalling. Filed; doesn't gate v1 ship since spec 05 was already
   conditional in the audit (T100 §2). Investigation owner suggested:
   webrtc-dev (T34/T42 surface).
4. **Loopback-rig session** — still required for the v1-defining
   number. Operator with cam + display + NTP-synced hosts runs
   `tests/harness/validation.md` §4 once a real-screen-capture path
   is alive (item 2(a), 2(b), or 2(c) above), and pipes the results
   through `phase1-baseline.sh`. Today's stack does not support this.

**Anticipated bottleneck order** (to be confirmed against real
numbers): encoder time (libvpx VP9 zero-latency or x264 zero-latency)
> network transport > display vsync > capture. The harness measures
all four together; only the comparison between {loopback, LAN, regional}
isolates the network contribution.

---

## 7. Reproducibility

See the canonical reproduction recipe in
[`phase1-loopback-TEMPLATE.md`](./phase1-loopback-TEMPLATE.md) §7.
That is the operator's checklist for running this measurement once
T86 + #96 are resolved.

A condensed version, valid post-T86 fix:

```bash
# 0. Resolve blockers.
#    - T86: real getDisplayMedia path works; CHROMELESS_USE_FAKE_MEDIA=0
#    - #96: streamer re-offers on client join

git checkout <stack revision>
docker compose -f infra/compose.yaml up -d --build

# 1. Confirm streamer is offering for real.
docker compose -f infra/compose.yaml logs signaling | grep "peer joined"
#    -> expect role=browser within 30s

# 2. NTP sync, lock cam AE/AF, frame, light. (validation.md §4.)

# 3. Source side.
( cd harness/latency && python3 -m http.server 8000 ) &
( cd harness/latency && python3 server-sink.py --out-dir ../captures ) &
# Open http://<source>:8000/?run=phase1-2026-04-30-A&period=1000 fullscreen.

# 4. Cam recording — 60s loopback first.
START_MS=$(date +%s%3N)
ffmpeg -f v4l2 -framerate 60 -video_size 1280x720 \
       -i /dev/video0 -c:v libx264 -preset ultrafast -crf 18 \
       -t 60 cam.mp4
echo "$START_MS" > cam.start.txt

# 5. Reconcile + record baseline.
bash tests/harness/phase1-baseline.sh \
    --cam       cam.mp4 \
    --jsonl     harness/captures/phase1-2026-04-30-A.jsonl \
    --start-ms  "$(cat cam.start.txt)" \
    --offset-ms 0 \
    --target    loopback \
    --no-assert \
    --note      "first real Phase 1 loopback baseline"

# 6. Replace this file with the filled-in numbers; commit
#    docs/measurements/phase1-loopback-2026-04-30.md +
#    tests/harness/baselines/phase1-phase1-2026-04-30-A.json (sym
#    links updated by the script) + harness/captures/<runId>.{jsonl,
#    summary.txt, latencies.csv, histogram.png}.
```

---

## 8. Sign-off

- **Recommended next action:** **FIX-AND-RETRY** — see §6. T86 then
  #96 then operator run. Do NOT mark T65 complete until the §3 table
  has real numbers and the §4 comparison-vs-budget has Pass/Fail
  decisions.
- **Approved by:** _(deferred to operator + team-lead at run time)_
- **Date:** _(deferred)_

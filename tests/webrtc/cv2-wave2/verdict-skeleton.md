# CV2 Wave 2 rv8 — final verdict skeleton (verification-lead, fill on results)

Image: registry.triform.cloud/chromeless/chromeless:<RV8_TAG> @ <RV8_DIGEST>
Broker: registry.triform.cloud/chromeless/signaling:cv2-route-alias
Fired: <TIMESTAMP>

Each harness verdict uses the campaign 3-orthogonal design:
 (1) primary positive LOG/observation proving the path fired
 (2) FAIL-class enumeration — what was checked that could fail and did not
 (3) upstream-gate sweep — NOTREACHED / not-implemented / silent-no-op grep

────────────────────────────────────────────────────────────────────────
## M4 typed-dispatch / CV2-81   (phase-a-m4-typed-dispatch.mjs, worker cv2-81-m4-worker)
VERDICT: <PASS | FAIL Fn | HALT Hn>
 (1) positive:  ANCHOR-A composite ctor LOG count = <n> (expect 1)
                ANCHOR-B main_parts wiring LOG count = <n> (expect 1)
                handshake: 4 DCs open = <y/n>; 6 envelopes sent = <y/n>
 (2) FAIL-class: F1 anchor-A absent <>, F2 anchor-B absent <>, F3 NEG-C
                violated (pre-CV2-81 baseline line) <>, F4 NEG-D per-dispatcher
                drop <>, F5 cross-dispatcher leak <>
 (3) upstream-gate: HALT H3 renderer-DOM <present/empty>; H1 handshake <>;
                H2 input DC <>
 evidence: <grep lines>

## CV2-82 audio / M5.5   (phase-a-m5.5-r1-pulse-init.mjs + run_r2 + run_r5)
VERDICT R1: <PASS | FAIL a/b/c | HALT>   R2: <>   R3-piggyback: <>   R5: <>
 (1) positive:  LS_INFO "AudioDeviceModule constructed AND initialized
                (kPlatformDefaultAudio / PulseAudio)" present = <y/n>
                R2 pactl Default Source = cb_capture.monitor = <y/n>
 (2) FAIL-class: (a) line-110 LS_WARNING present <>, (b) PCF LS_WARNING
                present <>, (c) LS_INFO absent / Init not reached <>
 (3) upstream-gate: signaling connected (not "CDP-only") <>; media-PCF built
                <>; APM markers absent (R3) <>
 NOTE: if R1 LS_INFO absent — was a peer driven to build the media-PCF? If
       not, that is a harness-coverage gap, classify HALT not FAIL.
 evidence: <M55-R1/R2/R5-VERDICT json>

## M5-R1 cursor / CV2-88   (phase-a-m5-r1-percursor-event.mjs, pod cv2-82-real-pulse)
VERDICT: <PASS | FAIL | HALT H3>
 (1) positive:  renderer-DOM laid out — link rect non-zero = <y/n>
                CbCursorClient::SetCursor new_type=2 within ~2s = <y/n>
 (2) FAIL-class: NOTIMPLEMENTED ScreenBase::IsWindowUnderCursor <>,
                ::GetCursorScreenPoint <>, ::GetDisplayNearestWindow <>;
                routing reaches Aura but not CbCursorClient <>
 (3) upstream-gate: HALT H3 renderer-DOM empty (SwANGLE) <> — THE rv7 gate;
                rv8 must clear this
 evidence: <grep lines>

## CV2-89 SwANGLE/Vulkan (runtime re-probe on rv8)
 icd.json location/path correct = <y/n>; .so present = <y/n>;
 ANGLE Display::initialize — Vulkan error -3 GONE = <y/n>;
 GPU process stays up = <y/n>

────────────────────────────────────────────────────────────────────────
## CLOSURE-TYPE DECISION FRAME
- ARCHITECTURAL FLOOR  → all 4 PASS; ring-cascade closed in-codebase; rv8
  SwANGLE fix worked + CV2-81/82/88 verified. (the Task #211 pre-declaration)
- PARTIAL CLOSE WITH AXIS ISOLATION → some PASS, one axis still gated by a
  cheap-diagnosed cause (e.g. SwANGLE still fails → renderer/GPU axis).
- INFRA-ESCALATION FLOOR → blocked on external infra (NOT expected — SwANGLE
  is software; only applies if rv8 proves a hardware/driver dependency).
RECOMMENDATION: <type> because <evidence>.
Close-memory placeholder fill is gated on this — no PASS unless PASS is true.

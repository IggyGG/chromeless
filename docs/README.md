# docs — what to read, by what you're trying to do

Start here rather than browsing the tree: several documents in `docs/` are
point-in-time records of an architecture that no longer exists, and they do
not always announce it in the first line. This page says which is which.

The one-line rule: **if a document and the code disagree, the code wins.**
For wire contracts specifically, `capture/signaling/cb_wire_envelope.h` is
authoritative — not any prose here.

---

## I want to integrate against a running deployment

| read | why |
| --- | --- |
| [`protocols/signaling-envelope.md`](./protocols/signaling-envelope.md) | **The signaling frames themselves** — both dialects, the closed accept-list, and what a peer must reject. Start here if you are implementing a peer or a broker. |
| [`protocols/`](./protocols/) | Per-channel wire specs — input, cursor, clipboard, file-upload, stats, probe, reconnect, simulcast, codec-fallback, SDP munging, webcam/mic. The most useful docs in this tree, and kept in lockstep with the code by repo convention. |
| [`../capture/signaling/cb_wire_envelope.h`](../capture/signaling/cb_wire_envelope.h) | The signaling envelope contract itself. Authoritative over any prose. |
| [`../conformance/README.md`](../conformance/README.md) | Point the kit at your deployment and it tells you whether you satisfy the above. |
| [`ice-and-turn.md`](./ice-and-turn.md) | ICE/TURN configuration and what to expect through NATs. |
| [`security/auth.md`](./security/auth.md) | Session tokens, roles, tenancy. |

## I want to run it

| read | why |
| --- | --- |
| [`../README.md`](../README.md) | Accurate as of 2026-07. The place to start. |
| [`operations/standalone.md`](./operations/standalone.md) | **Running it yourself, with none of the triform infrastructure.** One machine, one port, a username and a password — plus TURN, split-host, and what this deliberately is not. |
| [`roadmap-ga.md`](./roadmap-ga.md) | **The plan of record to a GA-quality self-hosted product.** What already works, what a first-time user hits, and the ordered tracks and milestones that close the gap. |
| [`operations/runbook.md`](./operations/runbook.md) | Day-2 operations. |
| [`operations/phase1-deployment-checklist.md`](./operations/phase1-deployment-checklist.md) | Pre-flight before real traffic. Reviewed 2026-07-30 and still current — it is a checklist, not an architecture description, so the M7 migration barely touched it. |
| [`operations/sla.md`](./operations/sla.md) | The SLOs your alerting should enforce. |
| [`operations/tracing.md`](./operations/tracing.md) · [`operations/multi-region.md`](./operations/multi-region.md) | Observability and multi-region layout. |
| [`operations/triform-deploy.md`](./operations/triform-deploy.md) | **One real cluster, documented.** A worked example, not a generic guide — it names hosts and choices specific to that deployment. |
| [`build/chromium-from-source.md`](./build/chromium-from-source.md) | Pinning, rolls, and how the 4–8 h build actually works. Current. |

## I want to change the browser itself

| read | why |
| --- | --- |
| [`../CLAUDE.md`](../CLAUDE.md) | The traps that have cost real time. Read before touching `capture/`. |
| [`capture/`](./capture/) | FrameSink capture design, damage rects, the encoder-factory catalog. |
| [`internal/`](./internal/) | Per-encoder tuning rationale (H264, VP9, NVENC, VAAPI, SVT-AV1), BWE adapter, simulcast design. Design docs, so they age more slowly than status docs — but check the code before trusting a specific constant. |
| [`security/passthrough-threat-model.md`](./security/passthrough-threat-model.md) | Threat model for camera/mic passthrough. |
| [`findings/`](./findings/) | **Confirmed defects with no fix landed**, each reproduced against a live deployment. Read before debugging odd behaviour — the answer may already be here. Every one names the file and line to change; they are open tasks, not notes. |

## Background and prior art

[`prior-art/`](./prior-art/) (Selkies, Neko, Kasm) and [`research/`](./research/)
(AV1 encoders, decoder matrix, GPU passthrough, HDR, rendering matrix, sandbox
isolation). Survey work: still informative, not tracking the code.

---

## Historical — do not read as current

These describe **stock Chromium + `getDisplayMedia`**, driven from outside the
browser. The M7 native-peer migration deleted `capture/streamer-page/` and
moved the WebRTC peer *inside the browser process*, so the components these
documents measure and audit no longer exist. Each now carries a status banner
saying so.

| document | what it is |
| --- | --- |
| [`../PROJECT_BRIEF.md`](../PROJECT_BRIEF.md) | The original plan. Its *reasoning* is still worth reading — especially "don't fork Chromium until you have to", which is precisely the wall this project later hit. |
| [`phase-0-exit-report.md`](./phase-0-exit-report.md) · [`phase-1-exit-report.md`](./phase-1-exit-report.md) | Gate results, 2026-04-30. |
| [`audits/phase1-stack-audit.md`](./audits/phase1-stack-audit.md) | Component-by-component v1 ship/no-ship review. The *method* is worth copying; the findings are about deleted code. |
| [`measurements/`](./measurements/) | Latency runs against the old pipeline. Numbers are not comparable to today's. |
| [`demos/phase2-first-demo-runbook.md`](./demos/phase2-first-demo-runbook.md) | A demo script from the transition. |
| [`v1-success-criteria.md`](./v1-success-criteria.md) | The Phase-0 contract the above were judged against. |

Why keep them: they record *why* decisions were made, which is the part that
is expensive to reconstruct. Deleting them would leave the current design
looking arbitrary.

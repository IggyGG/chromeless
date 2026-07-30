# chromeless conformance kit

**Does your deployment actually implement the contracts this project
documents?** Point this at a running stack and find out.

```bash
node conformance/run.mjs --target=ws://localhost:8080 --role=signaling
node conformance/run.mjs --target=http://localhost:9222 --role=cdp
node conformance/run.mjs --target=http://localhost:3000 --role=full --json
```

Exit code is the verdict: `0` all required checks passed, `1` at least one
failed, `2` usage error. `--json` puts one machine-readable object on stdout;
human-readable progress always goes to stderr, never mixed into stdout.

## Why this exists

The specs in `docs/protocols/` and the wire contract in
`capture/signaling/cb_wire_envelope.h` describe what a chromeless deployment
must do. Nothing checked that a given deployment agreed with them. That
mattered as soon as this project became usable by people with none of the
triform infrastructure: a third party running their own broker, their own
build of the worker, or a reimplementation of either had no way to answer
"is my side correct?" short of reading C++.

`tests/` answers "does this repo's code work". This answers "does *your*
deployment satisfy the contract" — a different question, aimed at a
different person, and deliberately runnable against a stack this repo did
not build.

## Suites

| role | suite | what it asserts |
| --- | --- | --- |
| `signaling` | signaling-wire | envelope tags, `from` values, per-tag `data` shapes, and that off-contract tags are REJECTED |
| `cdp` | cdp-surface | DevTools reachable, `/json/version` shape, a page target exists |
| `full` | both of the above | |

**Not implemented yet:** a `media-sanity` suite (is an offer produced, does it
carry the expected m-lines, does the advertised codec set match the encoder
factory). It needs a real PeerConnection on the running deployment, which is
a bigger lift than the two suites above and belongs with the work that wires
`verification/`'s boot harness into a live media path. Until it exists there
is no `--role=media`; asking for one is a usage error rather than a silent
no-op.

Every check names the spec clause it enforces, so a failure tells you what
to read, not just that something is wrong.

## The negative checks are the point

Anyone can accept a well-formed `offer`. The contract's load-bearing half is
what a conforming implementation must **refuse**:

- `sdp_offer` — the historic v0 tag, dropped on purpose. Accepting it means
  your broker has drifted back to a contract this project abandoned.
- `bye` with a `data` field — `bye` carries no payload at all.
- an envelope with no `from`, or `from: "server"`.

`cb_wire_envelope.h` states these as MUST-reject. A deployment that accepts
them will appear to work right up until it meets a peer that assumes the
documented behaviour.

## Interpreting a failure

Each finding carries `spec` (where the rule is written), `expected`,
`observed`, and `hint`. Start with `spec` — it is a file path in this repo,
and by convention the file, not this kit, is the authority. If they
disagree, the file wins and this kit has a bug worth reporting.

## What it does not do

- It does not need this repo's `tests/` fixtures, a Chromium build, or
  triform. Node ≥ 18 and a reachable endpoint is the whole dependency list.
- It does not measure performance. Frame rates, latency and bitrate live in
  `tests/webrtc/` and `harness/`, which need a real browser.
- It does not mutate your deployment. Every check is read-only; the
  signaling suite opens a WebSocket, sends envelopes to a session id you
  supply, and disconnects.

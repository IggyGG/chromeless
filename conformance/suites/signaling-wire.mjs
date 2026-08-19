// conformance/suites/signaling-wire.mjs — does the broker honour the
// envelope contract?
//
// SPEC: capture/signaling/cb_wire_envelope.h is authoritative (per
// CLAUDE.md's docs table). Every rule below cites the line that states it,
// and the rules were read off cb_wire_envelope.cc's Decode/DecodeData
// rather than paraphrased from prose — the C++ is what a real peer does.
//
// What "conforming" means for a BROKER (the thing this suite talks to):
// it relays envelopes between peers of a session. It is not required to
// parse `data` — but it MUST NOT relay envelopes whose `type` is off the
// accept-list, because a peer on the far side will reject them and the
// resulting failure is invisible from the broker's side.
//
// The kit cannot see the far side of a broker it did not deploy, so the
// observable behaviour is: connect two peers to one session, send from A,
// see what reaches B. That is exactly how a real deployment fails, so it
// is what we test.

import { WireSocket } from '../lib/ws.mjs';
import { result } from '../lib/check.mjs';

const SPEC = 'capture/signaling/cb_wire_envelope.h';

// The COMPLETE accept-list — cb_wire_envelope.h:40 "The seven tags above
// are the COMPLETE accept-list."
export const VALID_TAGS = Object.freeze([
  'offer', 'answer', 'ice', 'bye',
  'request_renegotiate', 'probe_result', 'session_unhealthy',
]);

// The triform portal's flat dialect. Decode-side only: a conforming peer
// ACCEPTS these (cb_wire_envelope.cc PortalTagFromString) and still emits
// canonical. Listed here so the kit does not assert the opposite of the
// code — `sdp_offer` used to be in REJECTED_TAGS below, and moving it is
// part of the same change that taught the decoder this dialect.
export const PORTAL_TAGS = Object.freeze(['sdp_offer', 'sdp_answer', 'ice_candidate']);

// Tags a conforming implementation MUST reject. Plausible-looking
// neighbours that have never been valid in EITHER dialect — the
// accept-list is closed, just larger than it was.
export const REJECTED_TAGS = Object.freeze([
  'candidate', 'hello', 'restart_ice',
]);

function offerEnvelope(sdp = 'v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n') {
  return { type: 'offer', from: 'browser', data: { type: 'offer', sdp } };
}

// ---- checks ----------------------------------------------------------

// The happy path first: if this fails, every negative check below is
// meaningless (we would be asserting that a broken broker rejects things).
const relaysValidOffer = {
  id: 'relays-valid-offer',
  suite: 'signaling-wire',
  title: 'a well-formed `offer` envelope reaches the peer',
  spec: `${SPEC}:15`,
  required: true,
  async run(ctx) {
    let a, b;
    try {
      a = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'browser', timeoutMs: ctx.timeoutMs });
      b = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'client', timeoutMs: ctx.timeoutMs });
      const sent = offerEnvelope();
      await a.send(sent);
      const got = await b.waitFor(m => m?.type === 'offer', ctx.timeoutMs);
      if (!got) {
        return result.fail({
          expected: 'the `offer` envelope relayed to the other peer in the session',
          observed: 'nothing arrived before the timeout',
          hint: 'check that both peers joined the SAME session id, and that the broker relays browser→client. Pass --session=<id> to match your deployment.',
        });
      }
      if (got.data?.sdp !== sent.data.sdp) {
        return result.fail({
          expected: 'data.sdp relayed verbatim',
          observed: `data.sdp differs (${String(got.data?.sdp).slice(0, 60)}…)`,
          hint: 'a broker must not rewrite SDP; munging belongs to the peers (docs/protocols/sdp-munging.md)',
        });
      }
      return result.pass({ relayed: true, sdpBytes: sent.data.sdp.length });
    } finally {
      a?.close(); b?.close();
    }
  },
};

// The load-bearing half of the contract.
const rejectsOffContractTags = {
  id: 'rejects-off-contract-tags',
  suite: 'signaling-wire',
  title: 'off-contract `type` tags are not relayed',
  spec: `${SPEC}:40`,
  required: true,
  async run(ctx) {
    let a, b;
    try {
      a = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'browser', timeoutMs: ctx.timeoutMs });
      b = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'client', timeoutMs: ctx.timeoutMs });

      const relayed = [];
      for (const tag of REJECTED_TAGS) {
        await a.send({ type: tag, from: 'browser', data: { type: tag, sdp: 'x' } });
        const got = await b.waitFor(m => m?.type === tag, ctx.shortTimeoutMs);
        if (got) relayed.push(tag);
      }

      if (relayed.length) {
        return result.fail({
          expected: `these tags rejected: ${REJECTED_TAGS.join(', ')}`,
          observed: `relayed to the peer: ${relayed.join(', ')}`,
          hint: 'the accept-list is closed; anything outside it must not be relayed. Note it is closed, not small: the canonical tags PLUS the portal dialect (sdp_offer / sdp_answer / ice_candidate) are all valid — see PORTAL_TAGS above.',
          relayed,
        });
      }
      return result.pass({ rejected: REJECTED_TAGS });
    } finally {
      a?.close(); b?.close();
    }
  },
};

const rejectsByeWithData = {
  id: 'rejects-bye-with-data',
  suite: 'signaling-wire',
  title: '`bye` carrying a `data` field is not relayed',
  spec: `${SPEC}:27`,
  required: false,   // advisory: see below
  async run(ctx) {
    // Advisory, deliberately. The header is unambiguous that `bye` omits
    // `data` entirely ("not present, not null, not {}") and a peer WILL
    // reject it — but that is a check on the PAYLOAD, and a relay-only
    // broker that never parses `data` is a legitimate design. Failing a
    // deployment for this would make "required" mean "matches our
    // broker's implementation choices", not "satisfies the contract".
    let a, b;
    try {
      a = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'browser', timeoutMs: ctx.timeoutMs });
      b = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'client', timeoutMs: ctx.timeoutMs });
      await a.send({ type: 'bye', from: 'browser', data: {} });
      const got = await b.waitFor(m => m?.type === 'bye', ctx.shortTimeoutMs);
      if (got && got.data !== undefined) {
        return result.fail({
          expected: '`bye` relayed without a `data` field, or not relayed',
          observed: `relayed with data=${JSON.stringify(got.data)}`,
          hint: 'a peer built from this repo rejects the whole envelope (cb_wire_envelope.cc DecodeData: `if (data) return nullopt`), so the far side sees no bye at all and will wait out its own timeout instead',
        });
      }
      return result.pass({ byeDataStripped: !got || got.data === undefined });
    } finally {
      a?.close(); b?.close();
    }
  },
};

const relaysIceEndOfCandidates = {
  id: 'relays-ice-end-of-candidates',
  suite: 'signaling-wire',
  title: '`ice` with null `data` (end-of-candidates) survives the relay',
  spec: `${SPEC}:25`,
  required: true,
  async run(ctx) {
    let a, b;
    try {
      a = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'browser', timeoutMs: ctx.timeoutMs });
      b = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'client', timeoutMs: ctx.timeoutMs });
      await a.send({ type: 'ice', from: 'browser', data: null });
      const got = await b.waitFor(m => m?.type === 'ice', ctx.timeoutMs);
      if (!got) {
        return result.fail({
          expected: 'the end-of-candidates marker relayed',
          observed: 'nothing arrived',
          hint: 'JSON null is a MEANINGFUL value here, not an absent field. A broker that drops null-payload frames as "empty" silently breaks ICE completion.',
        });
      }
      if (got.data !== null) {
        return result.fail({
          expected: 'data === null preserved verbatim',
          observed: `data === ${JSON.stringify(got.data)}`,
          hint: 'null is the end-of-candidates marker (cb_wire_envelope.h:25). Rewriting it to {} or omitting the field changes its meaning.',
        });
      }
      return result.pass({ nullPreserved: true });
    } finally {
      a?.close(); b?.close();
    }
  },
};

const preservesFromField = {
  id: 'preserves-from-field',
  suite: 'signaling-wire',
  title: '`from` is present and is one of browser/client',
  spec: `${SPEC}:110`,
  required: true,
  async run(ctx) {
    let a, b;
    try {
      a = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'browser', timeoutMs: ctx.timeoutMs });
      b = await WireSocket.connect(ctx.target, { session: ctx.session, role: 'client', timeoutMs: ctx.timeoutMs });
      await a.send(offerEnvelope());
      const got = await b.waitFor(m => m?.type === 'offer', ctx.timeoutMs);
      if (!got) {
        return result.skip('no offer relayed; relays-valid-offer covers this', {});
      }
      if (!['browser', 'client'].includes(got.from)) {
        return result.fail({
          expected: '`from` is "browser" or "client"',
          observed: `from = ${JSON.stringify(got.from)}`,
          hint: 'a peer rejects any other value (cb_wire_envelope.cc Decode: RoleFromString → nullopt → whole envelope dropped). `from` is a sibling of `type`, never nested under `data`.',
        });
      }
      return result.pass({ from: got.from });
    } finally {
      a?.close(); b?.close();
    }
  },
};

export const checks = [
  relaysValidOffer,
  rejectsOffContractTags,
  relaysIceEndOfCandidates,
  preservesFromField,
  rejectsByeWithData,
];

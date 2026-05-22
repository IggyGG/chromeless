// verification/assertions/native-peer.mjs — R6 assertion #4.
//
// End-to-end native-peer connectivity probe. M0's slice is the harness —
// real probe logic lands in M3. The current image has no native peer; this
// assertion returns NOT_YET_IMPLEMENTED pre-M3 (NYI shim discipline)
// without hanging.
//
// Probe contract (post-M3):
//   - Connect TCP to ctx.target (host:port)
//   - Send hello frame: {"kind":"native-peer-hello","version":1}\n
//   - Expect ack frame:  {"kind":"native-peer-ack","version":1}\n  within
//     ctx.timeoutMs (default 10s)
//   - On ack: PASS
//   - On non-conforming reply: FAIL evidence.rejected='protocol mismatch'
//   - On no reply: FAIL evidence.reason='timeout'
//   - On TCP-refused: FAIL evidence.reason='connection refused' errno='ECONNREFUSED'
//
// M3 will replace the TCP wire with the real DataChannel/SDP exchange;
// the comparator surface (PASS/FAIL + evidence shape) is contractual.

import { connect } from 'node:net';
import { result } from '../lib/registry.mjs';
import { DEFAULT_SCHEDULE } from '../lib/verdict.mjs';

const HELLO = { kind: 'native-peer-hello', version: 1 };
const ACK_KIND = 'native-peer-ack';
const ACK_VERSION = 1;

function parseTarget(target) {
  if (!target) throw new TypeError('target required (host:port)');
  const i = target.lastIndexOf(':');
  if (i < 0) throw new TypeError(`bad target: ${target}`);
  return { host: target.slice(0, i), port: parseInt(target.slice(i + 1), 10) };
}

function modulePositionIndex(pos, schedule = DEFAULT_SCHEDULE) {
  const i = schedule.modules.indexOf(pos);
  return i < 0 ? 0 : i;
}

// Single-handshake TCP probe. Returns a discriminated result:
//   { kind: 'ack' } | { kind: 'protocol-mismatch', observed } |
//   { kind: 'timeout' } | { kind: 'refused', errno }
export async function probeNativePeer({ target, timeoutMs = 10_000 }) {
  const { host, port } = parseTarget(target);
  return new Promise(resolve => {
    let settled = false;
    let sock;
    const finish = (val) => {
      if (settled) return;
      settled = true;
      // Always tear the socket down so node:test (and the event loop) don't
      // keep the connection alive past the assertion's lifetime.
      try { sock && sock.destroy(); } catch { /* noop */ }
      resolve(val);
    };
    sock = connect({ host, port }, () => {
      sock.write(JSON.stringify(HELLO) + '\n');
    });
    sock.setTimeout(timeoutMs);
    let buf = '';
    sock.on('data', d => {
      buf += d.toString('utf8');
      const nl = buf.indexOf('\n');
      if (nl < 0) return;
      try {
        const msg = JSON.parse(buf.slice(0, nl));
        if (msg.kind === ACK_KIND && msg.version === ACK_VERSION) {
          finish({ kind: 'ack' });
        } else {
          finish({ kind: 'protocol-mismatch', observed: msg });
        }
      } catch (err) {
        finish({ kind: 'protocol-mismatch', observed: { parseError: err.message, raw: buf.slice(0, 80) } });
      }
    });
    sock.on('timeout', () => {
      finish({ kind: 'timeout' });
    });
    sock.on('error', err => {
      finish({ kind: 'refused', errno: err.code || 'UNKNOWN', message: err.message });
    });
    sock.on('close', () => {
      finish({ kind: 'refused', errno: 'CLOSED-WITHOUT-RESPONSE' });
    });
  });
}

export const assertion = {
  id: 'native-peer-connects',
  number: 4,
  title: 'native peer connectivity',
  async run(ctx) {
    // NYI shim discipline: pre-M3 returns NYI regardless of probe state.
    // Preserves the scaffold-green contract (test #6).
    const schedule = ctx.schedule || DEFAULT_SCHEDULE;
    const position = ctx.position || 'M0';
    const enablingMarker = schedule.assertions['native-peer-connects']?.enablingModule || 'M3';
    if (modulePositionIndex(position, schedule) < modulePositionIndex(enablingMarker, schedule)) {
      return result.notYetImplemented({
        reason: `position ${position} < enabling ${enablingMarker}; probe shimmed`,
      });
    }

    if (!ctx.target) {
      return result.fail({ error: 'ctx.target required (host:port)' });
    }

    let probe;
    try {
      probe = await probeNativePeer({ target: ctx.target, timeoutMs: ctx.timeoutMs || 10_000 });
    } catch (err) {
      return result.fail({ probeError: err.message });
    }

    switch (probe.kind) {
      case 'ack':
        return result.pass({ target: ctx.target });
      case 'protocol-mismatch':
        return result.fail({
          rejected: 'protocol mismatch',
          expected: { kind: ACK_KIND, version: ACK_VERSION },
          observed: probe.observed,
        });
      case 'timeout':
        return result.fail({ reason: 'timeout', timeoutMs: ctx.timeoutMs || 10_000 });
      case 'refused':
        return result.fail({
          reason: probe.errno === 'ECONNREFUSED' ? 'connection refused' : 'connection error',
          errno: probe.errno,
          message: probe.message,
        });
      default:
        return result.fail({ unknownProbeResult: probe });
    }
  },
};

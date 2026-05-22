// verification/fixtures/native-peer-loopback/peer-stub.mjs — R6 fixture.
//
// A tiny in-process TCP peer-stub. The R6 PASS-path test boots this on an
// ephemeral port and points the assertion's connectivity probe at it. The
// probe handshake is intentionally simple — we own both sides so the
// contract is whatever this stub plus the assertion's probe agree on.
//
// Wire shape (M0-only; M3 replaces this with real DataChannel/SDP exchange):
//
//   client → peer:  '{"kind":"native-peer-hello","version":1}\n'
//   peer   → client: '{"kind":"native-peer-ack","version":1}\n'
//
// A non-conformant peer in test #3 sends '{"kind":"wrong-magic"}\n'.
// A non-responding peer in test #4 accepts the TCP connection then never
// writes — exercises the timeout path.

import { createServer } from 'node:net';

export const MAGIC_HELLO = 'native-peer-hello';
export const MAGIC_ACK = 'native-peer-ack';
export const PROTOCOL_VERSION = 1;

// Track every accepted socket so close() can force-tear-down lingering
// connections (otherwise server.close() waits indefinitely for half-closed
// peer sockets and node:test hangs the whole run).
function wrap(server) {
  const live = new Set();
  server.on('connection', sock => {
    live.add(sock);
    sock.on('close', () => live.delete(sock));
  });
  return new Promise((resolve, reject) => {
    server.on('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const addr = server.address();
      resolve({
        host: '127.0.0.1',
        port: addr.port,
        close: () => new Promise(r => {
          for (const s of live) { try { s.destroy(); } catch { /* noop */ } }
          live.clear();
          server.close(() => r());
          server.unref();
        }),
      });
    });
  });
}

export function startConformantPeer({ port = 0 } = {}) {
  const server = createServer(sock => {
    let buf = '';
    sock.on('data', d => {
      buf += d.toString('utf8');
      const nl = buf.indexOf('\n');
      if (nl < 0) return;
      try {
        const msg = JSON.parse(buf.slice(0, nl));
        if (msg.kind === MAGIC_HELLO && msg.version === PROTOCOL_VERSION) {
          sock.write(JSON.stringify({ kind: MAGIC_ACK, version: PROTOCOL_VERSION }) + '\n');
        } else {
          sock.end();
        }
      } catch {
        sock.end();
      }
    });
    sock.on('error', () => {});
  });
  return wrap(server);
}

export function startMalformedPeer({ port = 0 } = {}) {
  const server = createServer(sock => {
    // Sends a wrong-magic frame regardless of input.
    sock.write(JSON.stringify({ kind: 'wrong-magic', version: 99 }) + '\n');
    sock.end();
  });
  return wrap(server);
}

export function startSilentPeer({ port = 0 } = {}) {
  const server = createServer(sock => {
    // Accepts the connection, then never writes. Exercises probe timeout path.
    // (Hold the socket open by attaching a noop data listener.)
    sock.on('data', () => {});
  });
  return wrap(server);
}

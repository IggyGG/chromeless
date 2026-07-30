// conformance/lib/ws.mjs — minimal signaling-WebSocket client.
//
// Uses node's built-in WebSocket (Node >= 22 stable, >= 18 with the flag —
// see run.mjs's preflight, which fails with a clear message rather than a
// ReferenceError). Deliberately zero dependencies: this kit is meant to be
// runnable by someone who has not installed anything from this repo.
//
// Registration contract, read off signaling/server.go:505-560 rather than
// guessed:
//
//   * URL is <base>/ws/<session-id>. The server also accepts
//     /api/webrtc/signaling/<session-id> — cb_signaling_ws_client.cc
//     hardcodes that path (the physics-broker shape) — so we let the
//     caller pass a base that already contains a prefix and only append
//     when they didn't.
//   * The FIRST frame must be a valid envelope, and its `from` field is
//     what registers the role. Any invalid `from` closes the connection
//     with no error frame, so a kit that skipped the hello would look like
//     a network failure instead of a protocol one.
//   * `ice` with null/absent data is treated as a registration-only hello
//     (server.go:446) and is NOT replayed to a late peer — which makes it
//     exactly the right hello frame: it registers without polluting the
//     replay buffer that a real session depends on.

const HELLO = (from) => ({ type: 'ice', from, data: null });

export class WireSocket {
  #ws;
  #inbox = [];
  #waiters = [];
  #closed = false;

  constructor(ws) {
    this.#ws = ws;
    ws.addEventListener('message', (ev) => {
      let parsed;
      try {
        parsed = JSON.parse(typeof ev.data === 'string' ? ev.data : String(ev.data));
      } catch {
        // Non-JSON on a JSON wire is itself a finding, but it belongs to
        // whichever check is listening — keep the raw so it can say so.
        parsed = { __unparseable: String(ev.data).slice(0, 200) };
      }
      this.#inbox.push(parsed);
      for (const w of this.#waiters.splice(0)) w();
    });
    ws.addEventListener('close', () => {
      this.#closed = true;
      for (const w of this.#waiters.splice(0)) w();
    });
  }

  static url(base, session) {
    const u = String(base).replace(/\/+$/, '');
    // Respect an explicit path; only synthesize /ws/ when the caller gave
    // a bare origin. A deployment behind a gateway may well mount the
    // broker somewhere else entirely.
    if (/\/(ws|api\/webrtc\/signaling)$/.test(u)) return `${u}/${session}`;
    if (/\/(ws|api\/webrtc\/signaling)\/[^/]+$/.test(u)) return u;
    return `${u}/ws/${session}`;
  }

  static connect(base, { session, role, timeoutMs = 5000 } = {}) {
    const url = WireSocket.url(base, session);
    return new Promise((resolve, reject) => {
      let ws;
      try {
        ws = new WebSocket(url);
      } catch (err) {
        reject(new Error(`cannot open ${url}: ${err.message}`));
        return;
      }
      const timer = setTimeout(() => {
        try { ws.close(); } catch { /* already gone */ }
        reject(new Error(`timed out after ${timeoutMs}ms connecting to ${url}`));
      }, timeoutMs);

      ws.addEventListener('open', () => {
        clearTimeout(timer);
        const sock = new WireSocket(ws);
        // Register the role before resolving, so callers never race the
        // hello against their first real send.
        ws.send(JSON.stringify(HELLO(role)));
        resolve(sock);
      });
      ws.addEventListener('error', () => {
        clearTimeout(timer);
        // The WHATWG error event carries no detail by design; the URL is
        // the only actionable thing we can report.
        reject(new Error(`websocket error connecting to ${url} (is the broker up, and is the path right?)`));
      });
    });
  }

  async send(envelope) {
    this.#ws.send(JSON.stringify(envelope));
    // Yield so the server can process before a subsequent waitFor. Without
    // this, a fast send→waitFor pair can sample the inbox before the event
    // loop has drained the socket.
    await new Promise(r => setTimeout(r, 0));
  }

  // Resolve with the first inbox entry matching `pred`, or null on timeout.
  // Returns null rather than throwing: "nothing arrived" is a normal,
  // expected outcome for the negative checks, not an error.
  async waitFor(pred, timeoutMs = 2000) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const hit = this.#inbox.find(pred);
      if (hit) return hit;
      if (this.#closed) return null;
      const remaining = deadline - Date.now();
      if (remaining <= 0) return null;
      await new Promise(resolve => {
        const t = setTimeout(resolve, Math.min(remaining, 50));
        this.#waiters.push(() => { clearTimeout(t); resolve(); });
      });
    }
  }

  close() {
    try { this.#ws.close(); } catch { /* already gone */ }
  }
}

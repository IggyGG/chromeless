// verification/lib/harness.mjs — R7.
//
// Container-boot + in-netns CDP harness for assertions #3/#4 (R5/R6).
//
// Pattern mirrors tests/smoke/container-boot.sh — DevTools is reached via
// `docker exec` so we operate inside the container's net namespace.
// Chromium 147 binds DevTools to 127.0.0.1 only, so host -p 9222:9222 cannot
// reach it; running CDP calls through `docker exec ... python3` is the
// portable workaround that needs no host pip installs.
//
// Public surface (intentionally small):
//   boot({imageTag, timeoutMs?, env?, args?, name?, env?})
//     → { containerId, wsUrl, client, teardown }
//   client.send(method, params?)        — single CDP request (per-call exec)
//   teardown()                          — idempotent cleanup
//   HarnessBootError                    — image-missing | container-exited
//   HarnessBootTimeoutError             — devtools-ready (or named phase)
//
// The client is duck-typed: anything that needs CDP only needs `send(method,
// params)` returning a promise of the CDP response object. No hard dep on
// `chrome-remote-interface`.
//
// Boot/devtools timeout: default 60s for boot, 1s used by harness self-test
// for the timeout assertion. teardown() is safe to call multiple times.

import { spawn, spawnSync, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { randomBytes } from 'node:crypto';

const execFileP = promisify(execFile);

export class HarnessBootError extends Error {
  constructor(message, { code, imageTag, exitCode, logsTail } = {}) {
    super(message);
    this.name = 'HarnessBootError';
    this.code = code;
    if (imageTag !== undefined) this.imageTag = imageTag;
    if (exitCode !== undefined) this.exitCode = exitCode;
    if (logsTail !== undefined) this.logsTail = logsTail;
  }
}

export class HarnessBootTimeoutError extends Error {
  constructor(message, { phase, timeoutMs } = {}) {
    super(message);
    this.name = 'HarnessBootTimeoutError';
    this.phase = phase;
    if (timeoutMs !== undefined) this.timeoutMs = timeoutMs;
  }
}

const DEFAULT_TIMEOUT_MS = 60_000;
const POLL_INTERVAL_MS = 250;

async function imageExists(imageTag) {
  try {
    await execFileP('docker', ['image', 'inspect', imageTag], {
      stdio: ['ignore', 'ignore', 'ignore'],
      timeout: 10_000,
    });
    return true;
  } catch {
    return false;
  }
}

async function dockerRunDetached({ imageTag, name, env = {}, args = [] }) {
  const runArgs = [
    'run',
    '--rm',
    '-d',
    '--name', name,
    '--shm-size=1g',
  ];
  // IDLE_TIMEOUT_S big so the in-container idle-watchdog doesn't race the
  // harness teardown.
  const mergedEnv = { IDLE_TIMEOUT_S: '999999', ...env };
  for (const [k, v] of Object.entries(mergedEnv)) {
    runArgs.push('-e', `${k}=${v}`);
  }
  runArgs.push(imageTag, ...args);
  try {
    const { stdout } = await execFileP('docker', runArgs, { timeout: 30_000 });
    return stdout.trim();
  } catch (err) {
    const stderr = (err.stderr || '').toString();
    if (/(no such image|manifest unknown|pull access denied|not found)/i.test(stderr)) {
      throw new HarnessBootError(`image not present: ${imageTag}`, {
        code: 'image-missing',
        imageTag,
      });
    }
    throw new HarnessBootError(`docker run failed: ${stderr.trim() || err.message}`, {
      code: 'docker-run-failed',
      imageTag,
    });
  }
}

async function containerIsRunning(containerId) {
  try {
    const { stdout } = await execFileP('docker', [
      'inspect', '-f', '{{.State.Running}}', containerId,
    ], { timeout: 5_000 });
    return stdout.trim() === 'true';
  } catch {
    return false;
  }
}

async function containerExitCode(containerId) {
  try {
    const { stdout } = await execFileP('docker', [
      'inspect', '-f', '{{.State.ExitCode}}', containerId,
    ], { timeout: 5_000 });
    const code = parseInt(stdout.trim(), 10);
    return Number.isFinite(code) ? code : null;
  } catch {
    return null;
  }
}

async function tailLogs(containerId, lines = 80) {
  try {
    const { stdout, stderr } = await execFileP('docker', [
      'logs', '--tail', String(lines), containerId,
    ], { timeout: 10_000, maxBuffer: 4 * 1024 * 1024 });
    return (stdout + (stderr ? '\n' + stderr : '')).trim();
  } catch {
    return '(logs unavailable)';
  }
}

async function execInContainerSimple(containerId, cmd, { timeoutMs = 10_000 } = {}) {
  // Run `docker exec <id> <cmd...>` and return stdout. Throws on non-zero.
  const { stdout } = await execFileP('docker', ['exec', containerId, ...cmd], {
    timeout: timeoutMs,
    maxBuffer: 4 * 1024 * 1024,
  });
  return stdout;
}

async function pollDevToolsReady(containerId, deadlineAt) {
  while (Date.now() < deadlineAt) {
    if (!(await containerIsRunning(containerId))) {
      const exitCode = await containerExitCode(containerId);
      const logsTail = await tailLogs(containerId);
      throw new HarnessBootError(
        `container exited (code=${exitCode}) before DevTools came up`,
        { code: 'container-exited', exitCode, logsTail }
      );
    }
    try {
      await execInContainerSimple(containerId, [
        'curl', '-fsS', 'http://127.0.0.1:9222/json/version',
      ], { timeoutMs: 3_000 });
      return;
    } catch {
      // not ready yet
    }
    await new Promise(r => setTimeout(r, POLL_INTERVAL_MS));
  }
  throw new HarnessBootTimeoutError(
    `DevTools did not respond within ${Date.now() - (deadlineAt - DEFAULT_TIMEOUT_MS)}ms`,
    { phase: 'devtools-ready', timeoutMs: DEFAULT_TIMEOUT_MS }
  );
}

async function findFirstPageWsUrl(containerId) {
  const raw = await execInContainerSimple(containerId, [
    'curl', '-fsS', 'http://127.0.0.1:9222/json',
  ]);
  let targets;
  try {
    targets = JSON.parse(raw);
  } catch (err) {
    throw new HarnessBootError('failed to parse /json target list', {
      code: 'targets-parse-failed',
    });
  }
  const page = targets.find(t => t.type === 'page' && t.webSocketDebuggerUrl);
  if (!page) {
    throw new HarnessBootError('no page target with webSocketDebuggerUrl', {
      code: 'no-page-target',
    });
  }
  return page.webSocketDebuggerUrl;
}

// ---------- in-container per-call CDP client ----------------------------
//
// Each `client.send(method, params)` spawns a fresh `docker exec -i python3`
// that opens the page WS, sends one frame, reads one response, exits.
// Stateless, robust, and matches the per-call cadence of the codec-cap and
// connectivity assertions (a handful of round-trips, not high-frequency).
//
// Returns the .result of the CDP response, or throws on error/timeout.

const CDP_PY = `\
import base64, json, os, secrets, socket, struct, sys
from urllib.parse import urlparse

ws_url = sys.argv[1]
req = json.loads(sys.argv[2])

u = urlparse(ws_url)
host = u.hostname or '127.0.0.1'
port = u.port or 80
path = u.path + (f'?{u.query}' if u.query else '') or '/'
s = socket.create_connection((host, port), timeout=30)
s.settimeout(30)
key = base64.b64encode(secrets.token_bytes(16)).decode('ascii')
s.sendall((
    f'GET {path} HTTP/1.1\\r\\n'
    f'Host: {host}:{port}\\r\\n'
    'Upgrade: websocket\\r\\n'
    'Connection: Upgrade\\r\\n'
    f'Sec-WebSocket-Key: {key}\\r\\n'
    'Sec-WebSocket-Version: 13\\r\\n\\r\\n'
).encode('ascii'))
buf = b''
while b'\\r\\n\\r\\n' not in buf:
    chunk = s.recv(4096)
    if not chunk: raise SystemExit('ws closed during handshake')
    buf += chunk
head, _, leftover = buf.partition(b'\\r\\n\\r\\n')
if b' 101 ' not in head.split(b'\\r\\n', 1)[0]:
    raise SystemExit('ws handshake failed: ' + head.decode(errors='replace'))
_buf = bytearray(leftover)

def _read(n):
    while len(_buf) < n:
        chunk = s.recv(max(4096, n - len(_buf)))
        if not chunk: raise SystemExit('ws closed mid-frame')
        _buf.extend(chunk)
    out = bytes(_buf[:n]); del _buf[:n]; return out

def send_text(text):
    p = text.encode('utf-8'); m = secrets.token_bytes(4); n = len(p)
    if n < 126: h = struct.pack('!BB', 0x81, 0x80 | n)
    elif n < (1 << 16): h = struct.pack('!BBH', 0x81, 0x80 | 126, n)
    else: h = struct.pack('!BBQ', 0x81, 0x80 | 127, n)
    s.sendall(h + m + bytes(b ^ m[i & 3] for i, b in enumerate(p)))

def recv_text():
    while True:
        b1, b2 = _read(2)
        fin = b1 & 0x80; opcode = b1 & 0x0F; masked = b2 & 0x80; n = b2 & 0x7F
        if n == 126: (n,) = struct.unpack('!H', _read(2))
        elif n == 127: (n,) = struct.unpack('!Q', _read(8))
        if masked:
            mk = _read(4)
            payload = bytes(b ^ mk[i & 3] for i, b in enumerate(_read(n)))
        else:
            payload = _read(n) if n else b''
        if not fin: raise SystemExit('ws fragmented (unsupported)')
        if opcode == 0x1: return payload.decode('utf-8')
        if opcode == 0x9:
            # pong
            mk = secrets.token_bytes(4); ph = struct.pack('!BB', 0x8A, 0x80 | len(payload))
            s.sendall(ph + mk + bytes(b ^ mk[i & 3] for i, b in enumerate(payload)))
            continue
        if opcode == 0xA: continue
        if opcode == 0x8: raise SystemExit('ws closed by server')

msg_id = req.get('id', 1)
payload = {'id': msg_id, 'method': req['method']}
if 'params' in req: payload['params'] = req['params']
send_text(json.dumps(payload))
while True:
    text = recv_text()
    parsed = json.loads(text)
    if parsed.get('id') == msg_id:
        sys.stdout.write(text)
        break
`;

function createClient({ containerId, wsUrl, defaultTimeoutMs = 30_000 }) {
  let nextId = 1;
  return {
    wsUrl,
    containerId,
    async send(method, params = undefined, { timeoutMs = defaultTimeoutMs } = {}) {
      const id = nextId++;
      const req = { id, method };
      if (params !== undefined) req.params = params;
      return new Promise((resolve, reject) => {
        // python3 -c "<script>" runs the script; argv[1]=ws_url, argv[2]=req-json.
        const child = spawn('docker', [
          'exec', containerId,
          'python3', '-c', CDP_PY, wsUrl, JSON.stringify(req),
        ], { stdio: ['ignore', 'pipe', 'pipe'] });
        let stdout = '';
        let stderr = '';
        const t = setTimeout(() => {
          try { child.kill('SIGKILL'); } catch { /* noop */ }
          reject(new HarnessBootTimeoutError(
            `CDP call ${method} (id=${id}) timed out after ${timeoutMs}ms`,
            { phase: `cdp-${method}`, timeoutMs }
          ));
        }, timeoutMs);
        child.stdout.on('data', d => { stdout += d.toString(); });
        child.stderr.on('data', d => { stderr += d.toString(); });
        child.on('error', err => { clearTimeout(t); reject(err); });
        child.on('close', code => {
          clearTimeout(t);
          if (code !== 0) {
            reject(new Error(`CDP exec exited ${code}; method=${method}; stderr=${stderr.trim()}`));
            return;
          }
          let parsed;
          try { parsed = JSON.parse(stdout); }
          catch (err) {
            reject(new Error(`CDP response not JSON: ${stdout.slice(0, 200)}`));
            return;
          }
          if (parsed.error) {
            const e = new Error(`CDP error: ${parsed.error.message || JSON.stringify(parsed.error)}`);
            e.cdpError = parsed.error;
            reject(e);
            return;
          }
          // Match chrome-remote-interface shape: return the result object
          // directly (what callers expect from `Runtime.evaluate` etc.).
          resolve(parsed.result || {});
        });
      });
    },
  };
}

// ------------- boot() -----------------------------------------------------

function genName() {
  return `chromeless-harness-${randomBytes(6).toString('hex')}`;
}

async function forceRemoveContainer(containerId) {
  try {
    await execFileP('docker', ['rm', '-f', containerId], {
      timeout: 15_000, stdio: ['ignore', 'ignore', 'ignore'],
    });
  } catch {
    // already gone — fine
  }
}

export async function boot({
  imageTag,
  timeoutMs = DEFAULT_TIMEOUT_MS,
  env = {},
  args = [],
  name = genName(),
} = {}) {
  if (!imageTag) throw new TypeError('boot({imageTag}) required');

  // Pre-flight image check so we can return the structured image-missing
  // error inside a tight bound (test #3 caps wall-time < 5s).
  if (!(await imageExists(imageTag))) {
    throw new HarnessBootError(`image not present: ${imageTag}`, {
      code: 'image-missing',
      imageTag,
    });
  }

  let containerId = null;
  let tornDown = false;
  const teardown = async () => {
    if (tornDown) return;
    tornDown = true;
    if (containerId) await forceRemoveContainer(containerId);
  };

  try {
    containerId = await dockerRunDetached({ imageTag, name, env, args });
    const deadlineAt = Date.now() + timeoutMs;
    await pollDevToolsReady(containerId, deadlineAt);
    const wsUrl = await findFirstPageWsUrl(containerId);
    const client = createClient({ containerId, wsUrl });
    return { containerId, wsUrl, client, teardown };
  } catch (err) {
    await teardown();
    throw err;
  }
}

// The CDP client is also useful in isolation (e.g. tests that fixture an
// existing container).
export { createClient };

// ---------- low-level helper exports for tests --------------------------

export const _internals = {
  imageExists,
  forceRemoveContainer,
};

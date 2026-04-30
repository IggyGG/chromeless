// 04: The streamer page exposes its live RTCPeerConnection on
// `window.pc` so the in-container idle-watchdog
// (`infra/lifecycle/idle-watchdog.sh`) can poll connectionState via
// DevTools Runtime.evaluate.
//
// This is a regression guard for the bug found during T16 — the
// watchdog was probing `(window.pc && window.pc.connectionState) ||
// null` and getting null on every tick, because streamer.js created
// the PC as a function-local `const pc` and never assigned it to
// window. Without the hook, every compose-launched container would
// tombstone itself after IDLE_TIMEOUT_S.
//
// Why this lives in tests/e2e/ but doesn't depend on the docker
// stack:
//   The streamer page (capture/streamer-page) is normally served by
//   the in-container static server on port 9000, which compose does
//   NOT expose to the host. Driving it via the full stack would
//   require either publishing 9000 (defeating the point of keeping
//   it loopback-only) or exec'ing into the chromium container (which
//   is essentially a smoke test, not a Playwright spec).
//
//   Instead we load capture/streamer-page/index.html via `file://`
//   with `getDisplayMedia` and `WebSocket` mocked in `addInitScript`.
//   The page reaches its `start()` function, awaits the mocked
//   media, builds the PC, and assigns `window.pc = pc`. We then
//   assert the hook is wired. We are NOT testing media negotiation
//   here — that's spec 03's job once T34 is fully in.
//
// Bug ref: T16-followup task description.

import { expect, test } from "@playwright/test";
import { createServer } from "node:http";
import type { IncomingMessage, ServerResponse, Server } from "node:http";
import type { AddressInfo } from "node:net";
import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const STREAMER_DIR = path.resolve(HERE, "../../capture/streamer-page");

// ES modules require an http(s) origin — file:// fails CORS for
// `import` / `<script type="module" src="...">`. We bring up a
// minimal static server scoped to capture/streamer-page/ for the
// duration of this spec; no docker, no compose.
function serveStreamerPage(): Promise<{
  baseUrl: string;
  close: () => Promise<void>;
}> {
  return new Promise((resolve) => {
    const server: Server = createServer(async (req: IncomingMessage, res: ServerResponse) => {
      const reqPath = (req.url ?? "/").split("?")[0] ?? "/";
      const filename = reqPath === "/" ? "/index.html" : reqPath;
      const fsPath = path.join(STREAMER_DIR, filename);
      // Don't escape the streamer-page directory.
      if (!fsPath.startsWith(STREAMER_DIR)) {
        res.statusCode = 403;
        res.end("forbidden");
        return;
      }
      try {
        const buf = await readFile(fsPath);
        const ext = path.extname(fsPath);
        const ct =
          ext === ".html" ? "text/html; charset=utf-8" :
          ext === ".js"   ? "text/javascript; charset=utf-8" :
          "application/octet-stream";
        res.setHeader("Content-Type", ct);
        res.end(buf);
      } catch {
        res.statusCode = 404;
        res.end("not found");
      }
    });
    server.listen(0, "127.0.0.1", () => {
      const port = (server.address() as AddressInfo).port;
      resolve({
        baseUrl: `http://127.0.0.1:${port}`,
        close: () =>
          new Promise<void>((r) => server.close(() => r())),
      });
    });
  });
}

// addInitScript runs before any document scripts, so streamer.js sees
// our mocks the first time it touches navigator.mediaDevices /
// WebSocket.
const installMocks = () => {
  // Replace getDisplayMedia. `navigator.mediaDevices` is not
  // configurable in modern Chromium — we can't redefine the whole
  // object, but we can override the method on it. Use a canvas
  // captureStream so the streamer's `await getDisplayMedia(...)`
  // resolves without needing auto-grant flags.
  const fakeGetDisplayMedia = async () => {
    const canvas = document.createElement("canvas");
    canvas.width = 320;
    canvas.height = 240;
    const ctx = canvas.getContext("2d");
    if (ctx) { ctx.fillStyle = "#222"; ctx.fillRect(0, 0, 320, 240); }
    // 1 fps is enough — we never read the frames. Some Chromium
    // versions require captureStream(0) for "still" sources; 1 is
    // a safe minimum.
    return (canvas as HTMLCanvasElement & { captureStream: (fps: number) => MediaStream })
      .captureStream(1);
  };
  if (!navigator.mediaDevices) {
    // Headless Playwright contexts usually expose mediaDevices, but
    // be defensive — if absent, install a minimal stub.
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: {} as MediaDevices,
    });
  }
  // Override the method directly. Falls back to defineProperty if
  // the slot is read-only on this build.
  try {
    (navigator.mediaDevices as unknown as {
      getDisplayMedia: typeof fakeGetDisplayMedia;
    }).getDisplayMedia = fakeGetDisplayMedia;
  } catch {
    Object.defineProperty(navigator.mediaDevices, "getDisplayMedia", {
      configurable: true,
      value: fakeGetDisplayMedia,
    });
  }

  // Replace WebSocket with a no-op stub so the streamer's signaling
  // dial doesn't fight with the test runner. We don't need the WS to
  // do anything; `window.pc = pc` is assigned before we send the
  // first WS frame, so the hook lands regardless.
  class FakeWS extends EventTarget {
    readyState = 0;  // CONNECTING
    url: string;
    static readonly CONNECTING = 0;
    static readonly OPEN = 1;
    static readonly CLOSING = 2;
    static readonly CLOSED = 3;
    constructor(url: string) {
      super();
      this.url = url;
      // Stay in CONNECTING forever. The streamer's onopen never
      // fires, which is fine — we don't need it for this test.
    }
    send(_: string): void { /* swallowed */ }
    close(): void { this.readyState = 3; /* CLOSED */ }
  }
  (window as unknown as { WebSocket: typeof WebSocket }).WebSocket =
    FakeWS as unknown as typeof WebSocket;
};

test.describe("streamer-page window hooks", () => {
  test.use({
    // The mocks live in init script form so we don't need a running
    // signaling server.
    baseURL: undefined,
  });

  test("exposes window.pc as an RTCPeerConnection after start()", async ({
    browser,
  }) => {
    // file:// requires a fresh context — addInitScript only applies
    // to subsequent navigations within the context, and we want a
    // clean slate.
    const context = await browser.newContext();
    await context.addInitScript(installMocks);
    const page = await context.newPage();
    // Surface page console / errors in the test log — without this, a
    // mock failure leaves the assertion failing on a 5s timeout with
    // no clue why.
    page.on("console", (msg) => {
      // Only echo errors/warnings into the test log on green runs;
      // console.log spam from the streamer is helpful only during
      // debugging.
      if (msg.type() === "error" || msg.type() === "warning") {
        // eslint-disable-next-line no-console
        console.log(`[page ${msg.type()}]`, msg.text());
      }
    });
    page.on("pageerror", (err) => {
      // eslint-disable-next-line no-console
      console.log("[pageerror]", err.message);
    });

    const { baseUrl, close } = await serveStreamerPage();
    try {
      const url = `${baseUrl}/index.html?signal=ws://stub/ws&session=test&fps=1`;
      await page.goto(url);

    // window.pc is assigned synchronously after the (mocked)
    // getDisplayMedia resolves. 5s is generous; in practice it lands
    // in well under 100ms.
    await page.waitForFunction(
      () => typeof (window as unknown as { pc?: unknown }).pc === "object" &&
             (window as unknown as { pc?: unknown }).pc !== null,
      undefined,
      { timeout: 5_000 },
    );

    const info = await page.evaluate(() => {
      const w = window as unknown as {
        pc?: { constructor: { name: string }; connectionState: string };
        signalingWs?: { url: string; readyState: number };
      };
      return {
        pcCtor: w.pc?.constructor.name ?? null,
        pcState: w.pc?.connectionState ?? null,
        wsCtor: w.signalingWs ? "present" : "absent",
        wsUrl: w.signalingWs?.url ?? null,
      };
    });

    // The PC is the load-bearing assertion for the watchdog.
    expect(
      info.pcCtor,
      "window.pc not exposed — idle-watchdog will tombstone every container",
    ).toBe("RTCPeerConnection");
    // connectionState before negotiation is "new"; we just need a
    // non-null string the watchdog can match against {connected,
    // connecting, new}.
    expect(info.pcState).toMatch(/^(new|connecting|connected)$/);

    // The signaling WS hook is informational but cheap to verify.
    expect(info.wsCtor).toBe("present");
    expect(info.wsUrl).toContain("ws://stub/ws");

      await context.close();
    } finally {
      await close();
    }
  });
});

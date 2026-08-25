// 05: The client receives real audio bytes from the cloud worker.
//
// What this asserts:
//   - The client peer connection has an inbound-rtp of `kind: "audio"`.
//   - `bytesReceived` is > 0 — actual audio crossed the wire through opus,
//     not just an empty m-line that negotiated successfully.
//
// HOW IT REACHES THE WORKER, AND WHY THAT CHANGED.
//
// This used to `connectOverCDP("http://localhost:9222")` and then hunt for a
// tab whose URL contained "/streamer/". Both halves are dead:
//
//   - The standalone stack no longer publishes 9222. DevTools is
//     unauthenticated remote code execution against the browser, so it stays
//     on the internal network; a spec cannot dial it from the host.
//   - There is no streamer page. It was deleted in the M7 native-peer
//     migration — the WebRTC peer lives in the browser process now, and the
//     worker's only tab is whatever the user navigated to.
//
// So the DevTools endpoint is supplied by the harness instead, via
// CHROMELESS_E2E_DEVTOOLS_URL. Forward it from wherever the worker actually
// runs and point the variable at the forward:
//
//   kubectl port-forward -n chromeless deploy/chromeless-worker 9222:9222
//   docker compose -f infra/compose.yaml exec chromium ...   # or a tunnel
//
// With the variable unset the spec SKIPS rather than fails: it needs a route
// into the worker that the default topology deliberately does not provide, and
// a red that means "you did not set up a tunnel" trains people to ignore reds.
//
// The tone is injected into the worker's active page. Its audio reaches the
// captured stream through PulseAudio's null sink (see infra/audio-routing.md).

import {
  chromium,
  expect,
  test,
  type Browser,
  type Page,
} from "@playwright/test";

// Supplied by the harness — see the header. No default: the stack does not
// publish DevTools, so guessing an endpoint would only produce a confusing
// connection error.
const DEVTOOLS_HTTP = process.env["CHROMELESS_E2E_DEVTOOLS_URL"] ?? "";

type StatsResult = {
  ok: boolean;
  bytes: number;
  packets: number;
  reason?: string;
};

test.describe("audio presence end-to-end", () => {
  // Whole-suite serial; the cloud-Chromium streamer is a single shared
  // resource and concurrent specs would collide.
  test.describe.configure({ mode: "serial" });

  let cdpBrowser: Browser | null = null;
  let streamerPage: Page | null = null;

  test.beforeAll(async () => {
    // No route to the worker's DevTools ⇒ skip. See the header: this is a
    // harness prerequisite, not a product failure.
    test.skip(
      DEVTOOLS_HTTP === "",
      "CHROMELESS_E2E_DEVTOOLS_URL is unset — forward the worker's DevTools " +
        "port and set it to inject the test tone (see the spec header).",
    );

    // Resolve the websocket URL OURSELVES and force the port back on.
    //
    // `connectOverCDP(http://host:9222)` fetches /json/version and then dials
    // whatever `webSocketDebuggerUrl` says. This worker answers
    //
    //     ws://localhost/devtools/browser/<id>          <- NO PORT
    //
    // so the client dials port 80 and hangs until the 30s timeout, reporting
    // "socket hang up" / "Timeout ... retrieving websocket url" — which reads
    // as the worker being down and is not. CLAUDE.md calls this out, and both
    // infra/gateway/cdp.go (rewriteWSHost) and tests/cdp/conftest.py already
    // carry the same workaround; this is the third site that needed it.
    //
    // Passing the ws:// endpoint directly skips the discovery step entirely.
    // RETRY, because the worker restarts as part of normal operation.
    //
    // It serves one session and then exits so supervisord can respawn it — see
    // docs/findings/one-session-per-worker-process.md. Specs run back-to-back,
    // so this one routinely arrives during the ~15s window when the previous
    // spec's session has just ended and DevTools is not yet listening. A
    // single fetch there fails with a bare "fetch failed" and reads as a
    // broken forward; measured 2026-08-20, the worker respawned 11 times
    // during one 8-spec run.
    let wsEndpoint = "";
    let lastErr: unknown = null;
    const deadline = Date.now() + 60_000;
    while (Date.now() < deadline) {
      try {
        const res = await fetch(`${DEVTOOLS_HTTP}/json/version`);
        const body = (await res.json()) as { webSocketDebuggerUrl?: string };
        const raw = body.webSocketDebuggerUrl ?? "";
        if (!raw) throw new Error(`no webSocketDebuggerUrl in ${JSON.stringify(body)}`);
        const u = new URL(raw);
        // Force host AND port onto it: this worker answers
        // `ws://localhost/devtools/...` with NO PORT when reached by service
        // name, which makes a client dial 80 and hang. CLAUDE.md documents
        // the trap; infra/gateway/cdp.go and tests/cdp/conftest.py both carry
        // the same workaround.
        u.host = new URL(DEVTOOLS_HTTP).host;
        wsEndpoint = u.toString();
        break;
      } catch (err) {
        lastErr = err;
        await new Promise((r) => setTimeout(r, 1000));
      }
    }
    if (!wsEndpoint) {
      throw new Error(
        `could not resolve the DevTools websocket URL from ${DEVTOOLS_HTTP} ` +
          `within 60s: ${lastErr}. Is the forward up, and is the worker running?`,
      );
    }

    try {
      cdpBrowser = await chromium.connectOverCDP(wsEndpoint);
    } catch (err) {
      throw new Error(
        `connectOverCDP(${wsEndpoint}) failed: ${err}. Is the port-forward ` +
          `still up, and is the worker running?`,
      );
    }

    const contexts = cdpBrowser.contexts();
    if (contexts.length === 0) {
      throw new Error(
        "no contexts on the worker — DevTools attached but no active tab",
      );
    }
    // Take the first real page. The worker runs exactly one (the embedder
    // creates a single WebContents at boot), so there is nothing to search
    // for — the old "/streamer/" hunt looked for a page that no longer exists.
    for (const ctx of contexts) {
      const pages = ctx.pages();
      if (pages.length > 0) {
        streamerPage = pages[0]!;
        break;
      }
    }
    if (!streamerPage) {
      throw new Error("worker has no page target — is Chromium still booting?");
    }

    // Inject a 440 Hz tone. Any AudioContext output reaches the captured
    // stream via the PulseAudio null sink (infra/audio-routing.md).
    // Idempotent: a flag on window keeps re-runs from stacking oscillators.
    await streamerPage.evaluate(() => {
      const w = window as unknown as {
        __cbwrtc_test_tone_started?: boolean;
        AudioContext: typeof AudioContext;
      };
      if (w.__cbwrtc_test_tone_started) return;
      const ac = new w.AudioContext();
      const osc = ac.createOscillator();
      const gain = ac.createGain();
      gain.gain.value = 0.1; // low amplitude
      osc.type = "sine";
      osc.frequency.value = 440;
      osc.connect(gain).connect(ac.destination);
      osc.start();
      w.__cbwrtc_test_tone_started = true;
    });
  });

  test.afterAll(async () => {
    // connectOverCDP returns a Browser handle that owns the connection
    // but NOT the underlying Chromium — close() detaches without
    // killing cloud Chromium. Important: don't disconnect early or
    // mid-test we'd lose the streamerPage handle.
    if (cdpBrowser) {
      await cdpBrowser.close();
      cdpBrowser = null;
    }
  });

  test("client inbound-rtp audio receives > 0 bytes within 10s of connect", async ({
    page,
  }) => {
    // ?e2e=1 turns on `window.__cbwrtc_pc` per client/main.ts.
    await page.goto("/?e2e=1");
    await expect(page.locator("#connect")).toBeEnabled();
    await page.locator("#connect").click();

    // Reach connected. The post-T34 flow: streamer is offerer; client
    // is answerer. ICE is exchanged in both directions.
    await expect(
      page.locator("#state-conn"),
      "client peer connection didn't reach 'connected' — check the broker log " +
        "for a browser-role peer on this session id; if absent, the worker " +
        "never dialled signaling."
    ).toHaveText("connected", { timeout: 60_000 });

    // Poll getStats for audio bytes. Done inside page.evaluate so the
    // RTCStatsReport iteration stays in-page (Playwright can't
    // serialize an RTCStatsReport across the boundary).
    const result: StatsResult = await page.evaluate(async (): Promise<StatsResult> => {
      const w = window as unknown as { __cbwrtc_pc?: RTCPeerConnection };
      const pc = w.__cbwrtc_pc;
      if (!pc) {
        return { ok: false, bytes: 0, packets: 0,
                 reason: "window.__cbwrtc_pc not exposed; ?e2e=1 missing or " +
                         "client/main.ts hook regressed" };
      }
      const deadline = performance.now() + 10_000;
      let lastBytes = 0;
      let lastPackets = 0;
      while (performance.now() < deadline) {
        const stats = await pc.getStats();
        let bytes = 0;
        let packets = 0;
        stats.forEach((r) => {
          // RTCInboundRtpStreamStats has kind+bytesReceived+packetsReceived.
          // mediaType is the deprecated alias; some Chromium versions
          // still emit it. Handle both.
          const isInboundAudio =
            r.type === "inbound-rtp" &&
            ((r as unknown as { kind?: string }).kind === "audio" ||
              (r as unknown as { mediaType?: string }).mediaType === "audio");
          if (isInboundAudio) {
            const br = (r as unknown as { bytesReceived?: number }).bytesReceived ?? 0;
            const pr = (r as unknown as { packetsReceived?: number }).packetsReceived ?? 0;
            if (br > bytes) bytes = br;
            if (pr > packets) packets = pr;
          }
        });
        if (bytes > 0) {
          return { ok: true, bytes, packets };
        }
        lastBytes = bytes;
        lastPackets = packets;
        await new Promise((r) => setTimeout(r, 500));
      }
      return {
        ok: false,
        bytes: lastBytes,
        packets: lastPackets,
        reason: "bytesReceived stayed at 0 for 10s — probable causes: " +
                "(a) audio fixture isn't producing audible output in cloud " +
                "Chromium (T24 PulseAudio null-sink regressed?), " +
                "(b) opus rtpmap missing from offer (T30/T34?), " +
                "(c) packet loss masking RTP entirely (unlikely on loopback)",
      };
    });

    expect(result.ok, result.reason ?? "audio bytes_received check failed").toBe(true);
    expect(result.bytes, "bytesReceived must be > 0").toBeGreaterThan(0);
    expect(result.packets, "packetsReceived must be > 0").toBeGreaterThan(0);
  });
});

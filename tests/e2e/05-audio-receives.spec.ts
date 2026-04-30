// 04: The client receives audio bytes from the cloud-Chromium streamer.
//
// What this asserts:
//   - The client peer connection has an inbound-rtp of `kind: "audio"`
//     within 10 s of `iceConnectionState=connected`.
//   - Its `bytesReceived` is > 0 — actual audio is flowing through opus
//     encode/decode on the wire, not just an empty m-line.
//   - Its `packetsReceived` is increasing — confirming the receiver is
//     actually consuming RTP packets, not seeing a one-shot probe.
//
// How it works:
//   1. Attach to the cloud Chromium via DevTools (host-reachable post-T52
//      thanks to the socat sidecar). Find the streamer page.
//   2. Inject a 440 Hz Web Audio tone into the streamer page (this is
//      what `tests/e2e/fixtures/audio-tone.html` documents canonically;
//      we inject inline so the fixture can stay self-contained for
//      manual inspection).
//   3. Drive the client at baseURL with ?e2e=1 — that exposes
//      `window.__cbwrtc_pc` per `client/main.ts maybeExposePcForE2e`.
//   4. Click Connect, wait for `#state-conn === "connected"`.
//   5. Poll `__cbwrtc_pc.getStats()` for inbound-rtp audio every 500 ms;
//      assert bytes_received > 0 within 10 s.
//
// Failure modes and what they tell you:
//   - Step 1 fails (`connectOverCDP` rejects)         → T52 regressed; host can't reach DevTools.
//   - Step 1 finds no streamer page                   → T28 streamer-static or supervisord chromium broken.
//   - Step 2 throws "NotReadableError" or
//     `__cbwrtc_test_tone_started` not set            → cloud-Chromium AudioContext can't reach the destination
//                                                        (T24 PulseAudio null-sink regressed?), or the streamer
//                                                        page hasn't initialized — see T78 (getDisplayMedia
//                                                        NotReadableError) which keeps the page from running
//                                                        its start() body.
//   - Step 4 times out                                → Streamer never reached `pc.connectionState=connected`.
//                                                        Most-likely root cause today is **T78** (getDisplayMedia
//                                                        fails inside the cloud Chromium, so the streamer's
//                                                        start() throws before it dials signaling). T69 is in
//                                                        but masked by T78.
//   - Step 5 times out                                → Audio routing (T24) regressed; getDisplayMedia({audio:true})
//                                                        returning no track; or T26 cursor-watcher swallowed the
//                                                        audio context.

import {
  chromium,
  expect,
  test,
  type Browser,
  type Page,
} from "@playwright/test";

const DEVTOOLS_HTTP = "http://localhost:9222";
const STREAMER_TITLE_HINT = "streamer";

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
    // Connect to the running cloud Chromium. If T52 isn't on this stack
    // yet, this throws and the test fails clearly.
    try {
      cdpBrowser = await chromium.connectOverCDP(DEVTOOLS_HTTP);
    } catch (err) {
      throw new Error(
        `connectOverCDP(${DEVTOOLS_HTTP}) failed: ${err}. ` +
          `Likely T52 regressed (Chromium DevTools not reachable from host) ` +
          `or the compose stack isn't running. Run \`docker compose up -d\` ` +
          `from infra/ first.`
      );
    }

    const contexts = cdpBrowser.contexts();
    if (contexts.length === 0) {
      throw new Error(
        "no contexts on cloud Chromium — DevTools attached but no active tab"
      );
    }
    for (const ctx of contexts) {
      for (const pg of ctx.pages()) {
        const url = pg.url();
        if (url.includes(STREAMER_TITLE_HINT) || url.includes("/streamer/")) {
          streamerPage = pg;
          break;
        }
      }
      if (streamerPage) break;
    }
    if (!streamerPage) {
      throw new Error(
        "streamer page not found in cloud Chromium contexts — " +
          "expected a tab whose URL contains '/streamer/'. Likely a T28 " +
          "regression in supervisord launch."
      );
    }

    // Inject the 440 Hz tone. The streamer page's getDisplayMedia stream
    // includes system audio via PulseAudio's null-sink (T24), so any
    // AudioContext output reaches the captured stream.
    //
    // Idempotent: stash a flag on window so re-runs don't double-tone.
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
      "client peer connection didn't reach 'connected' within 20s — " +
        "most likely T78 (getDisplayMedia NotReadableError in cloud Chromium) " +
        "is keeping the streamer's start() from completing, so it never dials " +
        "signaling. T69 fix is in but masked by T78. Check signaling logs " +
        "for `peer joined role=browser`; if absent, fix T78 first."
    ).toHaveText("connected", { timeout: 20_000 });

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

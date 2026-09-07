// 03: Real video arrives from the cloud peer and decodes.
//
// THIS IS THE SPEC THAT PROVES THE PRODUCT WORKS. Everything else in this
// suite checks that machinery is wired; this one checks that pixels move.
//
// Un-skipped. It carried `test.skip` pending "T34" — the flip of the client to
// the answerer role — which landed with the M7 native-peer migration. The
// `window.__cbwrtc_pc` hook it says "a future client-side hook should add"
// also exists: client/main.ts:248 `maybeExposePcForE2e`, gated on `?e2e=1`.
// Both preconditions have been met for a long time; the suite has simply not
// been asserting on video.
//
// A RECEIVER IS NOT ENOUGH. The original assertion — getReceivers() contains a
// video track — passes in a well-documented failure mode where the transport
// is up, the track object exists, and NOT ONE FRAME ever arrives. That is the
// CV2 `fdec=0` shape, and it has been mistaken for success here before (see
// the memory note: "green verdicts lie"). So this also requires
// `framesDecoded` to actually climb, which is the difference between "a video
// track was negotiated" and "you can see the browser".

import { expect, test } from "@playwright/test";
import { CONNECT_TIMEOUT_MS, openConnected } from "./fixtures/connect.js";

// A cold worker can take ~35s to offer (CONNECT_TIMEOUT_MS, in
// fixtures/connect.ts), then frames have to start flowing.
const FRAMES_TIMEOUT_MS = 30_000;

test.describe("video track reception", () => {
  test("client receives a video track that actually decodes frames", async ({
    page,
  }) => {
    // ?e2e=1 turns on window.__cbwrtc_pc (client/main.ts maybeExposePcForE2e).
    //
    // Stage 1 — the connection. Diagnostic ordering matters here: if this
    // fails, nothing below can pass, and the message should say so. (The
    // client connects on load; see fixtures/connect.ts.)
    await openConnected(page, "/?e2e=1");
    await expect(page.locator("#state-conn")).toHaveText("connected", {
      timeout: CONNECT_TIMEOUT_MS,
    });

    // Stage 2 — a video receiver exists. Necessary, not sufficient.
    const hasVideoReceiver = await page.evaluate(() => {
      const pc = (window as unknown as { __cbwrtc_pc?: RTCPeerConnection })
        .__cbwrtc_pc;
      if (!pc) return null;
      return pc.getReceivers().some((r) => r.track?.kind === "video");
    });
    expect(
      hasVideoReceiver,
      "client did not expose window.__cbwrtc_pc — is ?e2e=1 set, and did main.ts's hook run?",
    ).not.toBeNull();
    expect(
      hasVideoReceiver,
      "no video receiver on the peer connection — the worker negotiated no video m-line",
    ).toBe(true);

    // Stage 3 — FRAMES. This is the assertion that catches the failure the
    // other two miss: transport up, track present, nothing rendering.
    await expect
      .poll(
        async () =>
          page.evaluate(async () => {
            const pc = (window as unknown as { __cbwrtc_pc?: RTCPeerConnection })
              .__cbwrtc_pc;
            if (!pc) return 0;
            const stats = await pc.getStats();
            let frames = 0;
            stats.forEach((report) => {
              if (report.type === "inbound-rtp" && report.kind === "video") {
                frames = Math.max(frames, (report as { framesDecoded?: number }).framesDecoded ?? 0);
              }
            });
            return frames;
          }),
        {
          message:
            "framesDecoded never went above 0 — a video track was negotiated but " +
            "no frame ever decoded. This is the fdec=0 shape: check that the " +
            "worker's capture is armed and that ICE picked a working pair.",
          timeout: FRAMES_TIMEOUT_MS,
        },
      )
      .toBeGreaterThan(0);

    // Stage 4 — the element the user actually looks at has real dimensions.
    // A <video> with a live srcObject but 0x0 intrinsic size shows nothing.
    const dims = await page.evaluate(() => {
      const v = document.getElementById("remote") as HTMLVideoElement | null;
      return v ? { w: v.videoWidth, h: v.videoHeight } : null;
    });
    expect(dims, "no #remote video element").not.toBeNull();
    expect(
      dims!.w * dims!.h,
      `#remote has no intrinsic size (${dims!.w}x${dims!.h}) — nothing is painting`,
    ).toBeGreaterThan(0);
  });
});

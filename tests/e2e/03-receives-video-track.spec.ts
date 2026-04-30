// 03: The client receives a video RTCRtpReceiver from the cloud peer.
//
// CURRENTLY SKIPPED — un-skip when T34 lands and the streamer-page is
// confirmed actually emitting media into the negotiated PC.
//
// Why this is skipped today:
//   The streamer page (T23) and its supervisord/Dockerfile wiring (T28)
//   are in place, so the cloud-Chromium *can* call getDisplayMedia and
//   add a track. But the negotiation flow today still has both peers as
//   offerers (T34 hasn't flipped client/main.ts to the answerer role),
//   so the SDP exchange never completes and `ontrack` never fires on
//   the client. Both T23 (streamer media source) and T34 (correct
//   negotiation direction) need to be live for this assertion to hold.
//
// What to do when T34 lands:
//   - Remove `test.skip` and re-run.
//   - If it still fails, follow the diagnostic order documented inline:
//     (a) confirm `state-conn` reaches "connected"; (b) confirm
//     `state-dc` reaches "open"; (c) inspect `getReceivers()` from the
//     test page evaluation. If only (c) fails, the negotiation is OK
//     but the streamer isn't actually adding a track — that's a
//     T23/T28 follow-up bug.
//
// Implementation note:
//   Reaching the live RTCPeerConnection from Playwright requires the
//   client to expose a test-mode hook (e.g. assigning the active PC to
//   `window.__cbwrtc_pc` behind a query-string flag). A small follow-up
//   on the client (T34 work or a child task) should add that hook so
//   E2E tests can introspect getReceivers() without DOM scraping.

import { expect, test } from "@playwright/test";

test.describe("video track reception", () => {
  test.skip(
    "client peer connection has at least one video RTCRtpReceiver after Connect",
    async ({ page }) => {
      await page.goto("/?e2e=1");
      await page.locator("#connect").click();

      // Stage 1: wait for the connection to come up.
      await expect(page.locator("#state-conn")).toHaveText("connected", {
        timeout: 20_000,
      });

      // Stage 2: introspect the live PC for a video receiver. Requires
      // a future client-side hook that exposes the PC on window when
      // ?e2e=1 is set; without it, this evaluate() returns null and
      // the test fails with a clear message rather than a silent pass.
      const hasVideoReceiver = await page.evaluate(() => {
        const pc = (window as unknown as { __cbwrtc_pc?: RTCPeerConnection })
          .__cbwrtc_pc;
        if (!pc) return null;
        return pc.getReceivers().some((r) => r.track?.kind === "video");
      });

      expect(
        hasVideoReceiver,
        "client did not expose window.__cbwrtc_pc — add the e2e hook"
      ).not.toBeNull();
      expect(hasVideoReceiver).toBe(true);
    }
  );
});

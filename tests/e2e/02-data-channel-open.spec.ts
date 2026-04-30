// 02: The "input" data channel reaches the "open" state.
//
// CURRENTLY SKIPPED — un-skip when T34 lands.
//
// Why this is skipped today:
//   The data channel cannot transition past "connecting" until the
//   DTLS/SCTP handshake completes, which requires the remote peer to
//   answer the SDP offer. The streamer page (T23) and its container
//   wiring (T28) are in place, but T34 — "Flip T14 client to answerer
//   role per T15/T23 design" — has not landed. Today both peers act
//   as offerers, so neither side reaches a connected state and the
//   data channel stays at readyState "connecting".
//
// What to do when T34 lands:
//   - Remove `test.skip` (use `test`).
//   - Confirm that #state-dc reaches "open" within ~15 s on a warm
//     stack. If it doesn't, file a real bug — the contract per
//     client/main.ts is that the channel is created on connect and
//     opens once the peer connection is up.

import { expect, test } from "@playwright/test";

test.describe("input data channel", () => {
  test.skip("reaches open state on the client within 15s of Connect", async ({
    page,
  }) => {
    await page.goto("/");
    await page.locator("#connect").click();

    await expect(page.locator("#state-dc")).toHaveText("open", {
      timeout: 15_000,
    });

    // A defensive secondary signal: the connection-state pill should
    // also be at "connected" by this point.
    await expect(page.locator("#status")).toHaveAttribute(
      "data-state",
      "connected",
      { timeout: 15_000 }
    );
  });
});

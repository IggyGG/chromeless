// 02: The "input" data channel reaches the "open" state.
//
// This is the "can the user actually control the browser" check. Video can be
// flowing perfectly while input is dead — the channel is a separate SCTP
// stream over the same DTLS transport, and a page that renders but ignores
// every click is a documented failure shape (the triform portal ships a
// dedicated "degraded input" indicator for exactly it).
//
// Un-skipped. It carried `test.skip` with the note "un-skip when T34 lands" —
// T34 was "flip the client to the answerer role", which landed long ago: the
// client has been the answerer since the M7 native-peer migration, and
// client/src/session.ts has no offerer path left. The skip outlived its reason
// by a wide margin, which meant the suite silently stopped checking input.

import { expect, test } from "@playwright/test";

// Same budget as spec 01: a cold worker can take ~35s to emit its first offer,
// and the client's own watchdog does not fire until 45s.
const CONNECT_TIMEOUT_MS = 60_000;

test.describe("input data channel", () => {
  test("reaches open state on the client after Connect", async ({ page }) => {
    await page.goto("/");
    await page.locator("#connect").click();

    // The peer connection first — if this fails, the data channel never had a
    // transport to open on, and the failure below would be a symptom rather
    // than the cause.
    await expect(page.locator("#status")).toHaveAttribute(
      "data-state",
      "connected",
      { timeout: CONNECT_TIMEOUT_MS },
    );

    // Then the channel itself. The contract (client/main.ts) is that the
    // remote creates it and it opens once the peer connection is up.
    await expect(page.locator("#state-dc")).toHaveText("open", {
      timeout: CONNECT_TIMEOUT_MS,
    });
  });
});

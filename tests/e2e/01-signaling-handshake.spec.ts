// 01: The client connects to a real worker and the handshake completes.
//
// What this asserts:
//   - The client page loads at baseURL (i.e. the gateway served the bundle,
//     and auth.setup.ts's session cookie got us past the login).
//   - Clicking "Connect" drives the status pill to "connected".
//   - The peer connection reaches signaling state "stable" — the answer was
//     created and applied, so the SDP round trip really completed.
//
// WHAT THIS USED TO ASSERT, AND WHY IT WAS WRONG.
//
// It required `#state-iceg` to reach "complete" and `#state-sig` to be
// `have-local-offer|stable`, on the reasoning that ICE gathering is
// "independent of any remote answer and therefore proves the SDP machinery
// did real work". That described the pre-M7 architecture, where the client
// was the OFFERER.
//
// The client is the answerer now. client/src/session.ts only ever calls
// setLocalDescription with an *answer* (:422), so:
//
//   - `have-local-offer` is unreachable by construction. The answerer never
//     sets a local offer, so that state cannot occur.
//   - ICE gathering does not start until an offer arrives and an answer is
//     applied. Asserting "complete" on a 10s budget therefore races a cold
//     worker, which can take ~35s to produce its first offer.
//
// The regexes also accepted `connecting`, which is the never-connected state —
// so the spec passed whether or not anything worked. It has been green about
// nothing.
//
// The observable path for an answerer is: offer arrives → answer sent →
// signaling `stable` → connection `connected`. That is what we check.

import { expect, test } from "@playwright/test";

// A cold worker takes a while to emit its first offer — triform measured ~35s
// p99 on a cold Firecracker guest. The client's own offer-wait watchdog
// (client/src/session.ts) fires at 45s, so anything below that would fail the
// test before the code under test has given up. Sit above it.
const CONNECT_TIMEOUT_MS = 60_000;

test.describe("signaling handshake", () => {
  test("Connect reaches a connected peer connection", async ({ page }) => {
    await page.goto("/");

    // Sanity: the connect button is interactable. If the page did not load at
    // all this fails here rather than in a confusing state assertion.
    const connect = page.locator("#connect");
    await expect(connect).toBeVisible();
    await expect(connect).toBeEnabled();

    await connect.click();

    // The pill must reach "connected" — NOT "connecting", which is precisely
    // the state a broken stack sits in forever.
    await expect(page.locator("#status")).toHaveAttribute(
      "data-state",
      "connected",
      { timeout: CONNECT_TIMEOUT_MS },
    );

    // stable = the answer was created and applied. For an answerer this is
    // the state that proves the SDP round trip completed.
    await expect(page.locator("#state-sig")).toHaveText("stable", {
      timeout: CONNECT_TIMEOUT_MS,
    });

    // ICE reaches connected/completed once a candidate pair is nominated.
    // Both are success states: "completed" only appears on the controlling
    // agent, so requiring it alone would flake depending on which side won.
    await expect(page.locator("#state-ice")).toHaveText(/connected|completed/, {
      timeout: CONNECT_TIMEOUT_MS,
    });
  });
});

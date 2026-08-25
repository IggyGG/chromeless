import { defineConfig } from "@playwright/test";

// REAL Chrome, not the bundled chromium: `channel: "chrome"` makes Playwright
// launch /Applications/Google Chrome.app. That is the whole point of this
// config — the deployed stack must work for the browser the user actually has.
export default defineConfig({
  testDir: ".",
  timeout: 180_000,
  expect: { timeout: 30_000 },
  retries: 0,
  workers: 1,
  reporter: [["list"]],
  use: {
    baseURL: process.env["BASE_URL"] ?? "https://localhost:8443",
    channel: "chrome",
    // Headless, at the user's request — this must not steal focus. It is
    // still REAL Chrome (channel: "chrome" above), not the bundled chromium;
    // headless only changes whether a window is drawn, not which binary runs
    // or how WebRTC/ICE behaves.
    headless: true,
    // NOT ignoreHTTPSErrors. That flag hid a real, user-facing failure for two
    // rounds: the gateway served a self-signed cert, the browser accepted the
    // PAGE via a click-through exception, and then refused the wss:// upgrade —
    // which surfaces only as `ws closed {code:1015}` and an endless reconnect
    // loop. Every test passed the whole time, because Playwright was told not
    // to care about certificates.
    //
    // The gateway now serves a cert issued by the machine's mkcert root CA,
    // which the OS and Chrome genuinely trust, so no leniency is needed here.
    // Leaving this false is load-bearing: if the cert ever regresses to
    // self-signed, these tests fail the way the user does.
    ignoreHTTPSErrors: false,
    trace: "retain-on-failure",
    video: "retain-on-failure",
    launchOptions: {
      args: [
        // Chrome refuses mic/cam and some WebRTC paths without these on a
        // self-signed origin; they mirror what the in-cluster suite gets by
        // default from the playwright image.
        "--use-fake-ui-for-media-stream",
        "--autoplay-policy=no-user-gesture-required",
      ],
    },
  },
});

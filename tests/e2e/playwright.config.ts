// Playwright configuration for the v0 chromeless E2E suite.
//
// The default flow brings up the full stack via docker compose
// (infra/compose.yaml) and waits for the client to be reachable at
// CHROMELESS_E2E_BASE_URL (default https://localhost:8443 — the gateway, now
// the only published port; the nginx client on :3000 is gone). For local iteration
// or CI environments where the stack is already running (e.g. a
// self-hosted runner with a long-lived dev stack, or a manual
// `docker compose up` in another terminal), set
// CHROMELESS_E2E_USE_RUNNING_STACK=1 and Playwright will skip the webServer
// management entirely.
//
// Run from this directory:
//   npm install                       # one-time
//   npx playwright install --with-deps chromium   # one-time
//   npm run test:e2e
//
// See ./README.md for the full guide.

import { defineConfig, devices } from "@playwright/test";

const baseURL =
  process.env["CHROMELESS_E2E_BASE_URL"] ?? "https://localhost:8443";

// The stack is behind a login, so the specs need credentials. These must match
// what compose passes the gateway; the fixture in ./fixtures/auth.ts signs in
// with them before each spec.
export const gatewayUser = process.env["CHROMELESS_USER"] ?? "e2e";
export const gatewayPass = process.env["CHROMELESS_PASS"] ?? "e2e-password";

/** Where auth.setup.ts parks the signed-in session cookie. */
export const storageStatePath = "playwright/.auth/session.json";

const useRunningStack =
  process.env["CHROMELESS_E2E_USE_RUNNING_STACK"] === "1" ||
  process.env["CHROMELESS_E2E_USE_RUNNING_STACK"] === "true";

export default defineConfig({
  testDir: ".",

  // The whole suite drives a single shared cloud-browser stack, so
  // running specs in parallel against the same session_id would cause
  // them to collide on T13's one-pair-per-session contract. Keep it
  // serial.
  fullyParallel: false,
  workers: 1,

  // Per-test budget. MUST exceed the 60s the specs themselves ask for, or
  // Playwright kills the test before its own assertion can time out — the log
  // then says BOTH "Test timeout of 30000ms exceeded" AND "with timeout
  // 60000ms", which reads as a product failure and is not one. 30s (the
  // default) was the real cap until 2026-08-20.
  //
  // 90s is the 60s connect budget plus headroom for a worker restart: the
  // worker serves one session and then exits so supervisord can respawn it
  // (~15s), and specs run back-to-back, so spec N+1 routinely arrives while
  // the worker is still booting.
  timeout: 90_000,

  // Don't paper over flakes — see tests/README.md flakiness policy.
  //
  // ...with ONE exception, and it is not flakiness: the worker is a
  // single-session process. Spec N+1 can legitimately arrive mid-restart, and
  // no amount of in-spec waiting helps because the client has already dialled.
  // A single retry lets it re-dial against the respawned worker. Set to 0 to
  // see the raw behaviour.
  retries: process.env["CI"] ? 1 : 0,

  forbidOnly: !!process.env["CI"],

  reporter: process.env["CI"]
    ? [["github"], ["html", { open: "never" }]]
    : "list",

  use: {
    baseURL,
    // CHROMELESS_E2E_CHANNEL=chrome runs every project — the auth setup
    // included, which is why this sits here and not on the chromium project
    // — in an installed Google Chrome instead of Playwright's bundled
    // Chromium. For a laptop where `playwright install` is slow or blocked
    // (the 147 MB download sat at 200 KB for ten minutes on 2026-09-07; see
    // the arm64 section of the README for the older failure modes), and for
    // checking the client in the browser users actually have. CI leaves it
    // unset and uses the pinned build.
    ...(process.env["CHROMELESS_E2E_CHANNEL"]
      ? { channel: process.env["CHROMELESS_E2E_CHANNEL"] }
      : {}),
    actionTimeout: 10_000,
    navigationTimeout: 15_000,
    trace: "retain-on-failure",
    // Video needs Playwright's bundled ffmpeg, which on an arm64 Mac is the
    // x86 build (`spawn Unknown system error -88`, EBADARCH) — so a run in
    // the installed Chrome (CHROMELESS_E2E_CHANNEL above) still died in
    // newPage until this could be switched off. CHROMELESS_E2E_VIDEO=off.
    video: process.env["CHROMELESS_E2E_VIDEO"] === "off" ? "off" : "retain-on-failure",
    screenshot: "only-on-failure",
    // The gateway generates a self-signed certificate on first boot (there is
    // nothing to issue a real one for "localhost"), so every navigation and
    // every wss:// dial would otherwise fail the TLS check.
    ignoreHTTPSErrors: true,
  },

  projects: [
    // Signs in once and saves the session cookie; every spec then starts
    // authenticated without knowing the login exists. Keeps the login out of
    // the specs entirely, which matters because none of them are ABOUT auth.
    {
      name: "setup",
      testMatch: /auth\.setup\.ts/,
    },
    {
      name: "chromium",
      dependencies: ["setup"],
      // testDir is ".", so without this the chromium project would also match
      // auth.setup.ts and run the sign-in a second time as if it were a spec.
      testMatch: /.*\.spec\.ts/,
      use: {
        ...devices["Desktop Chrome"],
        // The cloud-browser client uses WebRTC features that require
        // permissions / fake-media flags; baseline desktop Chrome is
        // fine for the offer/ICE specs and the future receive-video
        // spec (which uses real media from the streamer container).
        storageState: storageStatePath,
      },
    },
  ],

  // The webServer launches docker compose with the full stack. We probe the
  // gateway's /healthz for readiness rather than baseURL: every other route is
  // behind the session cookie, so probing `/` would see a 303 to /login and
  // Playwright would call the stack ready before it is.
  //
  // CHROMELESS_USER/PASS are exported into compose's environment here so the
  // gateway's required-credential check is satisfied and the fixture's login
  // uses the same pair.
  webServer: useRunningStack
    ? undefined
    : {
        command:
          `CHROMELESS_USER=${gatewayUser} CHROMELESS_PASS=${gatewayPass} ` +
          "docker compose -f ../../infra/compose.yaml up --build",
        url: `${baseURL}/healthz`,
        ignoreHTTPSErrors: true,
        reuseExistingServer: true,
        timeout: 240_000,
        stdout: "pipe",
        stderr: "pipe",
      },
});

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

  // Don't paper over flakes — see tests/README.md flakiness policy.
  retries: 0,

  forbidOnly: !!process.env["CI"],

  reporter: process.env["CI"]
    ? [["github"], ["html", { open: "never" }]]
    : "list",

  use: {
    baseURL,
    actionTimeout: 10_000,
    navigationTimeout: 15_000,
    trace: "retain-on-failure",
    video: "retain-on-failure",
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

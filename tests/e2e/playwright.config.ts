// Playwright configuration for the v0 cloud-browser-webrtc E2E suite.
//
// The default flow brings up the full stack via docker compose
// (infra/compose.yaml) and waits for the client to be reachable at
// CBWRTC_E2E_BASE_URL (default http://localhost:3000). For local iteration
// or CI environments where the stack is already running (e.g. a
// self-hosted runner with a long-lived dev stack, or a manual
// `docker compose up` in another terminal), set
// CBWRTC_E2E_USE_RUNNING_STACK=1 and Playwright will skip the webServer
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
  process.env["CBWRTC_E2E_BASE_URL"] ?? "http://localhost:3000";

const useRunningStack =
  process.env["CBWRTC_E2E_USE_RUNNING_STACK"] === "1" ||
  process.env["CBWRTC_E2E_USE_RUNNING_STACK"] === "true";

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
  },

  projects: [
    {
      name: "chromium",
      use: {
        ...devices["Desktop Chrome"],
        // The cloud-browser client uses WebRTC features that require
        // permissions / fake-media flags; baseline desktop Chrome is
        // fine for the offer/ICE specs and the future receive-video
        // spec (which uses real media from the streamer container).
      },
    },
  ],

  // The webServer launches docker compose with the full stack. We
  // probe baseURL for readiness; compose's own healthchecks gate the
  // client service on the chromium + signaling services being healthy
  // first, so a successful HTTP probe of the client implies the rest
  // of the stack is at least up.
  webServer: useRunningStack
    ? undefined
    : {
        command: "docker compose -f ../../infra/compose.yaml up --build",
        url: baseURL,
        reuseExistingServer: true,
        timeout: 240_000,
        stdout: "pipe",
        stderr: "pipe",
      },
});

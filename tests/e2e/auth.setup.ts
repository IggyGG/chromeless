// Signs in to the gateway once and saves the session cookie for every spec.
//
// The whole stack now sits behind a login: `/` redirects to /login without a
// session, and /ws/ and /turn-credentials answer 401. Rather than teaching each
// spec to authenticate, this runs as a Playwright *setup project* that the
// chromium project depends on, and its storageState is loaded by all of them.
// Specs therefore need no changes at all.
//
// The credentials come from playwright.config.ts, which also passes them to
// compose — so the pair the gateway boots with and the pair we sign in with
// cannot drift.

import { test as setup, expect } from "@playwright/test";
import { gatewayUser, gatewayPass, storageStatePath } from "./playwright.config.js";

setup("authenticate", async ({ page }) => {
  await page.goto("/login");

  await page.locator("#u").fill(gatewayUser);
  await page.locator("#p").fill(gatewayPass);
  await page.getByRole("button", { name: "Sign in" }).click();

  // The gateway 303s to "/" on success and re-renders the form with an error
  // on failure, so landing anywhere but /login is the signal. Asserting on the
  // URL rather than page content keeps this independent of the client bundle,
  // which may legitimately be mid-build on a cold `compose up`.
  await expect(page).not.toHaveURL(/\/login/);

  await page.context().storageState({ path: storageStatePath });
});

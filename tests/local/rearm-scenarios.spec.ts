// The scenarios that the single "connect right after a restart" test could
// never catch — and that a real user hits immediately.
//
// WHY THIS FILE EXISTS
//
// real-chrome-validation.spec.ts passed 3/3 while the user's own session sat
// at "waiting for offer" and then failed ICE. Both statements were true: that
// test always connected within seconds of a stack restart, when a fresh offer
// was still sitting in the broker's replay buffer. The user's path is
// different — the stack idles, THEN a viewer arrives. Measured:
//
//   20:13:46  worker starts, joins the broker, creates ONE offer -> buffered
//   ...       81 minutes idle
//   21:34:45  viewer connects. The broker correctly DROPS the 81-minute-old
//             SDP (replaying it is what produced iceConnectionState=failed),
//             so nothing is replayed -> "waiting for offer" forever, because
//             the worker is ALIVE, already in a session, and never re-offers.
//   21:37:24  viewer gives up and closes -> "peer left" -> synthesised `bye`
//             -> worker exits -> supervisord respawns -> fresh offer in 1.16s
//
// So the ONLY thing that ever produced a new offer was the PREVIOUS viewer
// leaving. The first viewer after any idle period longer than the replay cap
// waited ~2.5 minutes or reloaded.
//
// These tests encode the fixed contract: an already-running worker must offer
// to a newly-arrived viewer, WITHOUT restarting — because not restarting is
// the entire point of the re-armable driver. A recycle would also make these
// pass except for `state preserved` and `pid unchanged`, which is exactly why
// those two assertions are here.
import { expect, test } from "@playwright/test";
import { connectAndVerify, login, navigateRemote, readFrames, remoteUrl } from "./helpers";
import { execFileSync } from "node:child_process";

test.describe.configure({ mode: "serial" });

// How long to idle before the "cold arrival" connect. Must exceed the broker's
// SDP replay cap (iceReplayMaxAge, 300 s default) so the buffered offer is
// genuinely gone — that is the precondition being tested. Overridable so a
// quick smoke run does not cost 5+ minutes.
const IDLE_MS = Number(process.env["COLD_IDLE_MS"] ?? 320_000);

const NS = process.env["CHROMELESS_NS"] ?? "chromeless";
const WORKER_SELECTOR = process.env["CHROMELESS_WORKER_SELECTOR"]
  ?? "app.kubernetes.io/name=chromeless-standalone-worker";

/**
 * The chromium PID inside the worker pod.
 *
 * This is the discriminator between "re-armed" and "recycled". A recycle also
 * yields working video for viewer 2, so video alone cannot tell them apart.
 * If this number is unchanged across a viewer change, the browser process
 * survived and its state (tabs, scroll, logins) survived with it.
 */
function workerChromiumPid(): string {
  const pod = execFileSync("kubectl",
    ["get", "pods", "-n", NS, "-l", WORKER_SELECTOR,
     "-o", "jsonpath={.items[0].metadata.name}"],
    { encoding: "utf8" }).trim();
  if (!pod) throw new Error(`no worker pod matched ${WORKER_SELECTOR} in ns ${NS}`);
  // Matching is fussier than it looks, and getting it wrong makes this
  // assertion vacuous rather than failing loudly:
  //
  //   * `pgrep -o -f 'chrome'` returns pid 128, which is
  //     /bin/sh launch-chromeless.sh — the supervisord-managed WRAPPER. That
  //     survives nothing useful; supervisord respawns it on every restart, so
  //     comparing it would pass even when the browser did restart.
  //   * the binary is /usr/local/bin/chromeless, not `chrome`, and its
  //     renderer/zygote children share the same argv0 — they are excluded by
  //     rejecting `--type=`, since only the browser process lacks it.
  //
  // So: exact binary path, no --type=, oldest match.
  const out = execFileSync("kubectl",
    ["exec", "-n", NS, pod, "--", "bash", "-lc",
     "ps -eo pid,args --sort=start_time | grep '/usr/local/bin/chromeless' " +
     "| grep -v -- '--type=' | grep -v grep | head -1 | awk '{print $1}'"],
    { encoding: "utf8" }).trim();
  if (!out) throw new Error(`no browser process found in ${pod}`);
  return `${pod}:${out}`;
}

test("cold arrival: a viewer who shows up on an IDLE stack still gets video", async ({ page }) => {
  // Precondition: the buffered offer must be expired. Idle long enough that
  // the broker's age gate drops it, reproducing the user's exact path.
  // Idle + the 150s connect budget + headroom for login and frame polling.
  test.setTimeout(IDLE_MS + 300_000);

  await login(page);
  console.log(`  idling ${Math.round(IDLE_MS / 1000)}s so the buffered offer expires...`);
  await page.waitForTimeout(IDLE_MS);

  const pidBefore = workerChromiumPid();

  // No reload, no restart, no manual intervention: just connect.
  //
  // 150s rather than the default 90s, and the number is derived, not padded:
  // the recovery path here is the CLIENT's offer watchdog, which fires at 45s
  // (OFFER_WAIT_MS in client/src/session.ts) and sends request_renegotiate.
  // The worker then re-arms and offers, and ICE has to complete over a relay.
  // A 90s budget leaves almost nothing after the 45s wait, so it would fail a
  // stack that is working correctly, just slowly.
  //
  // This does NOT hide the defect it was written for: without the fix the
  // worker never offers at all and the viewer waits forever — measured at 240s
  // with no offer. The distinction the timeout has to preserve is
  // "recovers via the watchdog" vs "never recovers", and 150s preserves it.
  const frames = await connectAndVerify(page, "cold arrival", 150_000);
  console.log(`  cold arrival framesDecoded = ${frames}`);

  // The browser must NOT have restarted to serve this viewer. Without the
  // OnNewViewerNeedsOffer path, the only thing that ever produced an offer was
  // the viewer giving up and its bye recycling the worker — which also ends
  // with a connected viewer, just ~2.5 minutes later and on a fresh browser
  // that has lost every tab. Asserting the pid is what separates "the fix
  // worked" from "the old recycle eventually got there".
  const pidAfter = workerChromiumPid();
  console.log(`  worker chromium ${pidBefore} -> ${pidAfter}`);
  expect(pidAfter,
    "the browser restarted to serve the cold-arriving viewer — that is the OLD " +
    "recycle behaviour, not a re-arm")
    .toBe(pidBefore);
});

test("second viewer: connect, leave, and reconnect — with state preserved", async ({ browser }) => {
  test.setTimeout(300_000);

  const pidBefore = workerChromiumPid();
  console.log(`  worker chromium before = ${pidBefore}`);

  // ---- viewer 1: connect and navigate somewhere identifiable --------------
  const ctx1 = await browser.newContext({ ignoreHTTPSErrors: true });
  const p1 = await ctx1.newPage();
  await login(p1);
  await connectAndVerify(p1, "viewer 1");

  // Wikipedia rather than example.com: a real page with real JS and real
  // subresources, and one whose URL is unmistakable in /api/current-url.
  const marker = "https://en.wikipedia.org/wiki/WebRTC";
  await navigateRemote(p1, marker);
  await p1.waitForTimeout(8000);
  const urlBefore = await remoteUrl(p1);
  console.log(`  remote URL before disconnect = ${urlBefore}`);
  expect(urlBefore, "viewer 1's navigation did not take effect").toContain("wikipedia.org");

  // ---- viewer 1 leaves, the way real users leave --------------------------
  // Closing the CONTEXT (not calling any teardown API) is deliberate: this is
  // the byeless disconnect. `beforeunload` does NOT fire on a programmatic
  // context close, which is why the broker had to synthesise the `bye`.
  await ctx1.close();
  await new Promise((r) => setTimeout(r, 5000));

  // ---- viewer 2 arrives ---------------------------------------------------
  const ctx2 = await browser.newContext({ ignoreHTTPSErrors: true });
  const p2 = await ctx2.newPage();
  await login(p2);
  const frames2 = await connectAndVerify(p2, "viewer 2");
  console.log(`  viewer 2 framesDecoded = ${frames2}`);

  // ---- the payoff: did the browser SURVIVE the viewer change? ------------
  // This is the assertion that distinguishes the re-armable driver from the
  // recycle. A recycled worker comes back on its start page, having thrown
  // away every tab, scroll position and in-memory login.
  const urlAfter = await remoteUrl(p2);
  console.log(`  remote URL after reconnect  = ${urlAfter}`);
  expect(urlAfter,
    "the remote browser lost its page across the viewer change — it RESTARTED " +
    "rather than re-armed, which defeats the point of a persistent browser")
    .toContain("wikipedia.org");

  const pidAfter = workerChromiumPid();
  console.log(`  worker chromium after  = ${pidAfter}`);
  expect(pidAfter,
    "the chromium process changed across a viewer change — the worker restarted")
    .toBe(pidBefore);

  await ctx2.close();
});

test("real sites: frames keep climbing across genuinely different pages", async ({ page }) => {
  test.setTimeout(300_000);

  await login(page);
  let frames = await connectAndVerify(page, "real sites");

  // Deliberately heterogeneous: a heavy JS app, a media-ish page, and a plain
  // document. example.com alone is a static 1 KB file and proves very little
  // about the encoder under real load.
  const sites = [
    "https://en.wikipedia.org/wiki/Special:Random",
    "https://news.ycombinator.com",
    "https://example.com",
  ];

  for (const site of sites) {
    const before = frames;
    await navigateRemote(page, site);
    await page.waitForTimeout(10_000);
    frames = await readFrames(page);
    const url = await remoteUrl(page);
    console.log(`  ${site} -> framesDecoded ${before} -> ${frames}  (remote: ${url})`);
    expect(frames, `stream froze after navigating to ${site}`).toBeGreaterThan(before);
  }
});

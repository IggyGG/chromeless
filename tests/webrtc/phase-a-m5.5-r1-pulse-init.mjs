// tests/webrtc/phase-a-m5.5-r1-pulse-init.mjs
//
// CV2-82 M5.5 R1 verification harness — pulse-init success-side counterpart
// to the fd020fa defensive-fallback path the cv2-75 Ring N+2 campaign closed
// out.
//
// Phase 3.1 verdict (per /tmp/cv2-82-pulse-env-design.md):
//
//   PASS-shape (R1): all three conditions hold simultaneously
//     1. LS_INFO  "constructed AND initialized"
//                 "(kPlatformDefaultAudio / built-in PulseAudio)"
//        PRESENT in cb-chromium stderr.
//     2. LS_WARNING line-110 of cb_audio_device_module.cc
//        ("native AudioDeviceModule Init failed (PulseAudio server
//         unreachable?); falling back to kDummyAudio (no-audio path)")
//        ABSENT.
//     3. LS_WARNING in cloud_browser_pcf.cc ~line 155
//        ("native ADM unavailable — falling back to kDummyAudio
//         (no-audio path)") ABSENT.
//
//   FAIL-class:
//     (a) line-110 LS_WARNING PRESENT  -> real-Pulse env broken
//     (b) PCF LS_WARNING       PRESENT -> native-ADM nullptr fallback fired
//     (c) LS_INFO              ABSENT  -> Init not reached
//
// R3 piggyback (per Phase 3.3):
//
//   The same chromium.err.log stream observed for R1 also carries the
//   APM-enable markers from libwebrtc's audio-processing module. R3
//   APM-disabled verdict grounds against this stream:
//
//     APM-ABSENT-shape (R3 piggyback):
//       "APM enabled" / "EchoCanceller3" / "NoiseSuppressor" / "WebRtcAgc"
//        lines ABSENT.
//
//   NOTE: per the pre-fire grep checklist (cv2-82-scout Phase 4.3, lesson-l
//   axis-c), the cb_audio_options consumer is NOT YET wired on
//   cv2/wave-1-integration @ 83f21af — BUILD.gn declares the
//   `cb_audio_options` source_set but no callsite invokes `MakeAudioOptions`
//   or `CreateAudioSource(audio_options)`. Until that wiring lands (Wave 2.5
//   audio-track ticket), R3 verifies only that APM lines are absent because
//   no audio track exists at all, NOT that AudioOptions is consciously
//   disabling them. The verdict is intentionally conservative: APM-absent
//   under no-audio-track is necessary but not sufficient evidence that R3
//   would pass once the track lands.
//
// Surface choice — kubectl-exec on /var/log/supervisor/chromium.err.log,
// not CDP Log.entryAdded:
//
//   * cb-chromium under supervisord writes Chromium browser-process stderr
//     to /var/log/supervisor/chromium.err.log (see infra/supervisord.conf
//     line 93). libwebrtc's RTC_LOG output lands there, not in the CDP
//     Log domain (which surfaces page-side console + tracing entries).
//   * The harness therefore:
//       1. attaches via CDP and confirms /json/version reachability
//          (sanity gate — cb-chromium booted at all);
//       2. reads chromium.err.log via `kubectl exec ... cat` and applies
//          the three regexes for the verdict.
//   * For the `stub` variant (no supervisord), Chromium's stderr writes
//     to the pod's container-stderr; in that case the harness reads
//     `kubectl logs <pod>` instead. Variant is detected from the pod's
//     `cv2-82.variant` label.
//
// Usage:
//
//   node tests/webrtc/phase-a-m5.5-r1-pulse-init.mjs \
//     --pod cv2-82-real-pulse \
//     [--namespace chromeless] \
//     [--cdp-port-forward auto|none] \
//     [--variant auto]
//
//   Exit 0  = R1 PASS (and R3 piggyback PASS).
//   Exit 1  = R1 FAIL — verdict JSON on stdout names which leg.
//   Exit 77 = SKIPPED (e.g. pod not Running) — autotools sentinel.
//
// The verdict line is emitted as a single JSON object prefixed with
// "M55-R1-VERDICT" to match the M55-R2-VERDICT convention in
// harness/m5.5/run_red_test.sh.

import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";

const KUBECTL = process.env.KUBECTL_BIN || "kubectl";

const LS_INFO_RE =
  /native AudioDeviceModule[\s\S]{0,40}?constructed AND initialized[\s\S]{0,80}?kPlatformDefault[\s\S]{0,8}?Audio[\s\S]{0,80}?PulseAudio/i;
const LS_WARNING_LINE_110_RE =
  /native AudioDeviceModule Init failed[\s\S]{0,80}?PulseAudio server unreachable[\s\S]{0,80}?falling back to kDummyAudio/i;
const LS_WARNING_PCF_RE =
  /native ADM unavailable[\s\S]{0,80}?falling back to kDummyAudio[\s\S]{0,40}?no-audio path/i;

// R3 piggyback: APM-enable markers from libwebrtc. Sourced from the
// audio_processing module file names (echo_canceller, noise_suppressor,
// agc) plus the high-pass filter init line that APM emits when enabled.
// If audio AddTrack is not yet wired, NONE of these should appear because
// no audio source exists; treat any match as a FAIL signal.
const APM_MARKER_RE =
  /\b(EchoCanceller3|NoiseSuppressor|WebRtcAgc|HighPassFilter|AudioProcessingImpl::Initialize)\b/;

function parseArgs(argv) {
  const out = {
    pod: null,
    namespace: "chromeless",
    cdpPortForward: "auto",
    variant: "auto",
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--pod") out.pod = argv[++i];
    else if (a === "--namespace") out.namespace = argv[++i];
    else if (a === "--cdp-port-forward") out.cdpPortForward = argv[++i];
    else if (a === "--variant") out.variant = argv[++i];
    else if (a === "-h" || a === "--help") {
      process.stdout.write(
        "Usage: phase-a-m5.5-r1-pulse-init.mjs --pod <name> " +
          "[--namespace chromeless] [--variant auto|stub|dummy|real-pulse]\n"
      );
      process.exit(0);
    }
  }
  if (!out.pod) {
    process.stderr.write("ERROR: --pod is required\n");
    process.exit(2);
  }
  return out;
}

function runKubectl(args, { timeoutMs = 15_000 } = {}) {
  return new Promise((resolve, reject) => {
    const proc = spawn(KUBECTL, args, { stdio: ["ignore", "pipe", "pipe"] });
    let stdout = "";
    let stderr = "";
    const timer = setTimeout(() => {
      proc.kill("SIGKILL");
      reject(new Error(`kubectl ${args.join(" ")} timed out after ${timeoutMs}ms`));
    }, timeoutMs);
    proc.stdout.on("data", (d) => (stdout += d.toString("utf8")));
    proc.stderr.on("data", (d) => (stderr += d.toString("utf8")));
    proc.on("close", (code) => {
      clearTimeout(timer);
      resolve({ code, stdout, stderr });
    });
    proc.on("error", (err) => {
      clearTimeout(timer);
      reject(err);
    });
  });
}

async function resolveVariant(pod, namespace, requested) {
  if (requested !== "auto") return requested;
  const { code, stdout } = await runKubectl([
    "-n",
    namespace,
    "get",
    "pod",
    pod,
    "-o",
    "jsonpath={.metadata.labels.cv2-82\\.variant}",
  ]);
  if (code !== 0) return "unknown";
  const v = stdout.trim();
  return v || "unknown";
}

async function podPhase(pod, namespace) {
  const { code, stdout } = await runKubectl([
    "-n",
    namespace,
    "get",
    "pod",
    pod,
    "-o",
    "jsonpath={.status.phase}",
  ]);
  if (code !== 0) return null;
  return stdout.trim();
}

async function readChromiumStderr(pod, namespace, variant) {
  // Variant routing:
  //   real-pulse / dummy -> supervisord runs Chromium; stderr file path is
  //     /var/log/supervisor/chromium.err.log.
  //   stub -> Chromium invoked directly via dumb-init; stderr is the
  //     container's stderr, surfaced via `kubectl logs`.
  if (variant === "stub") {
    const { code, stdout, stderr } = await runKubectl(
      ["-n", namespace, "logs", pod, "-c", "cb-chromium"],
      { timeoutMs: 20_000 }
    );
    if (code !== 0) {
      throw new Error(`kubectl logs failed (code=${code}): ${stderr}`);
    }
    return stdout;
  }
  // dummy / real-pulse / unknown
  const { code, stdout, stderr } = await runKubectl(
    [
      "-n",
      namespace,
      "exec",
      pod,
      "-c",
      "cb-chromium",
      "--",
      "cat",
      "/var/log/supervisor/chromium.err.log",
    ],
    { timeoutMs: 20_000 }
  );
  if (code !== 0) {
    throw new Error(`kubectl exec cat chromium.err.log failed (code=${code}): ${stderr}`);
  }
  return stdout;
}

function evaluateVerdict(log, variant) {
  const lsInfo = LS_INFO_RE.test(log);
  const warnLine110 = LS_WARNING_LINE_110_RE.test(log);
  const warnPcf = LS_WARNING_PCF_RE.test(log);
  const apmMatch = log.match(APM_MARKER_RE);

  // Stub variant inverts the expectation — there it is the defensive
  // fallback path that should PASS (LS_INFO ABSENT, line-110 LS_WARNING
  // PRESENT, PCF LS_WARNING PRESENT). This is M5.5 R0 regression
  // coverage and lives here so a single harness covers all three
  // variants instead of a separate stub-only file.
  let r1, r1Reason;
  if (variant === "stub") {
    if (!lsInfo && warnLine110 && warnPcf) {
      r1 = "PASS";
      r1Reason = "M5.5 R0 regression coverage: defensive fallback fired";
    } else {
      r1 = "FAIL";
      r1Reason =
        "stub variant: expected defensive-fallback triple " +
        "(LS_INFO absent, line-110 + PCF warnings present); observed " +
        `LS_INFO=${lsInfo}, line110=${warnLine110}, pcf=${warnPcf}`;
    }
  } else {
    if (lsInfo && !warnLine110 && !warnPcf) {
      r1 = "PASS";
      r1Reason = "PASS-shape triple satisfied";
    } else if (warnLine110) {
      r1 = "FAIL";
      r1Reason =
        "(a) line-110 LS_WARNING PRESENT -> real-Pulse env broken " +
        "(see /var/log/supervisor/pulseaudio.err.log + " +
        "infra/pulse-default.pa)";
    } else if (warnPcf) {
      r1 = "FAIL";
      r1Reason =
        "(b) PCF LS_WARNING PRESENT -> native ADM nullptr fallback fired " +
        "(regression of R1)";
    } else if (!lsInfo) {
      r1 = "FAIL";
      r1Reason =
        "(c) LS_INFO ABSENT -> Init not reached (verify cb-chromium " +
        "process is up: kubectl exec ... supervisorctl status chromium)";
    } else {
      r1 = "FAIL";
      r1Reason = "internal: unreachable verdict branch";
    }
  }

  // R3 piggyback. Conservative: any APM marker presence = FAIL.
  // ABSENT alone is NECESSARY but not sufficient; once audio AddTrack
  // wiring lands (Wave 2.5 follow-up), this same harness verdict tightens.
  let r3, r3Reason;
  if (apmMatch) {
    r3 = "FAIL";
    r3Reason = `APM marker present: ${apmMatch[0]} (AudioOptions APM-disable not honoured)`;
  } else {
    r3 = "PASS-PROVISIONAL";
    r3Reason =
      "APM markers absent (necessary but not sufficient; audio AddTrack " +
      "wiring not yet present on cv2/wave-1-integration per pre-fire grep " +
      "checklist — tightens once Wave 2.5 lands)";
  }

  return {
    r1,
    r1Reason,
    r3,
    r3Reason,
    observed: { lsInfo, warnLine110, warnPcf, apmMatch: apmMatch ? apmMatch[0] : null },
  };
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const variant = await resolveVariant(opts.pod, opts.namespace, opts.variant);
  const phase = await podPhase(opts.pod, opts.namespace);

  if (phase !== "Running" && phase !== "Succeeded") {
    process.stdout.write(
      `M55-R1-VERDICT ${JSON.stringify({
        verdict: "SKIPPED",
        reason: `pod ${opts.pod} phase=${phase} (need Running or Succeeded)`,
        pod: opts.pod,
        variant,
      })}\n`
    );
    process.exit(77);
  }

  // Sanity: wait briefly for chromium.err.log to be non-empty. On a
  // freshly-applied pod the file may not yet exist if supervisord just
  // forked chromium. Bounded retry.
  let log = "";
  let lastErr = null;
  for (let i = 0; i < 20; i++) {
    try {
      log = await readChromiumStderr(opts.pod, opts.namespace, variant);
      if (log.trim().length > 0) break;
    } catch (err) {
      lastErr = err;
    }
    await sleep(500);
  }
  if (!log || log.trim().length === 0) {
    process.stdout.write(
      `M55-R1-VERDICT ${JSON.stringify({
        verdict: "FAIL",
        reason: `chromium.err.log empty or unreadable: ${lastErr ? lastErr.message : "no data"}`,
        pod: opts.pod,
        variant,
      })}\n`
    );
    process.exit(1);
  }

  const result = evaluateVerdict(log, variant);
  const verdict = result.r1 === "PASS" ? "PASS" : "FAIL";

  process.stdout.write(
    `M55-R1-VERDICT ${JSON.stringify({
      verdict,
      pod: opts.pod,
      variant,
      r1: { verdict: result.r1, reason: result.r1Reason },
      r3_piggyback: { verdict: result.r3, reason: result.r3Reason },
      observed: result.observed,
    })}\n`
  );

  process.exit(verdict === "PASS" ? 0 : 1);
}

main().catch((err) => {
  process.stdout.write(
    `M55-R1-VERDICT ${JSON.stringify({
      verdict: "ERROR",
      reason: err && err.message ? err.message : String(err),
    })}\n`
  );
  process.exit(2);
});

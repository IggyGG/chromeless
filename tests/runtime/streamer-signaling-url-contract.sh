#!/usr/bin/env bash
#
# streamer-signaling-url-contract.sh — the launcher's env → peer plumbing.
#
# WHAT THIS ASSERTS AND WHY IT CHANGED
#
# The original version of this file asserted a pre-M7 (Pattern-A) shape:
#
#   1. `token=<jwt>` appears in the streamer URL
#   2. the launch log prints `url=...token=<redacted>`
#   3. capture/streamer-page/streamer.js keeps a buildSignalingWsUrl helper
#
# All three are now wrong, and the script had been failing on (1) for so long
# that nothing ran it — it is in tests/runtime/, which no make target and no
# workflow invoked. A red test nobody runs is indistinguishable from no test.
#
# What actually happens today:
#   * The token is NOT a URL parameter. infra/launch-chromeless.sh:134-136
#     exports it as WEBRTC_SIGNALING_TOKEN and the native peer reads it from
#     the environment. There is no URL to redact because there is no URL.
#   * capture/streamer-page/ was DELETED in M7 (commit 6d347f7). Assertion (3)
#     read a file that does not exist.
#
# So this rewrite keeps the two assertions that still describe reality
# (Pattern-C session id reaches the launch; autostart=0 launches blank) and
# replaces the token assertions with the property that actually matters now:
#
#   THE RAW TOKEN MUST NEVER APPEAR IN OUTPUT.
#
# That is the security-relevant invariant the old redaction check was reaching
# for, and it holds regardless of whether the token travels by URL, env, or
# anything else — so it will not go stale the next time the mechanism moves.
# Verified against the live launcher: the raw token appears 0 times today.
#
# Hermetic: CHROMELESS_BROWSER_BIN=/bin/echo means nothing is executed, so this
# needs no Chromium, no Docker, and no cluster. Runs in well under a second.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

FAKE_TOKEN='header.payload.signature'
FAKE_SESSION='cb:11111111-2222-3333-4444-555555555555'

out="$(
  SESSION_ID="${FAKE_SESSION}" \
  SIGNALING_URL='ws://triform.triform-wtf.svc.cluster.local:3000/api/webrtc/signaling' \
  SIGNALING_TOKEN="${FAKE_TOKEN}" \
  STREAMER_LAUNCH_WAIT_ATTEMPTS=1 \
  CHROMELESS_BROWSER_BIN='/bin/echo' \
  sh infra/launch-chromeless.sh 2>&1
)"

# 1. Pattern-C session id must reach the launch. This is the contract the
#    isolator depends on: a guest that loses its session id cannot rejoin.
case "$out" in
  *"session=${FAKE_SESSION}"* ) ;;
  *) echo "missing Pattern-C session id in launch output" >&2
     echo "$out" >&2; exit 1 ;;
esac

# 2. The raw token must NEVER be printed. Not in the URL, not in a flag dump,
#    not in a debug line. Launch output lands in pod logs, which are shipped to
#    a log aggregator and retained — a leaked signaling token there is a real
#    credential in a searchable index.
if printf '%s' "$out" | grep -qF "${FAKE_TOKEN}"; then
  echo "SECURITY: raw signaling token leaked into launch output" >&2
  printf '%s\n' "$out" | grep -nF "${FAKE_TOKEN}" >&2
  exit 1
fi

# 3. The token must still be PLUMBED, not merely absent. A launcher that
#    silently dropped it would pass check 2 trivially — this is the
#    prove-the-negative-was-reachable rule: absence only means something once
#    presence is established.
if ! grep -q 'WEBRTC_SIGNALING_TOKEN' infra/launch-chromeless.sh; then
  echo "launch-chromeless.sh no longer plumbs WEBRTC_SIGNALING_TOKEN;" >&2
  echo "if the mechanism moved again, update this contract deliberately." >&2
  exit 1
fi

# 4. Chromium must launch BLANK.
#
#    The original script drove this with CHROMELESS_AUTOSTART_STREAMER=0 and
#    asserted `streamer_autostart=0` was logged. Writing this rewrite surfaced
#    that infra/launch-chromeless.sh does not reference that variable AT ALL —
#    it died with capture/streamer-page/ in M7. The old assertion was testing a
#    feature that no longer exists, which is why it could only ever fail.
#
#    Post-M7 there is no streamer page to autostart: the native peer owns the
#    session and Chromium always comes up at about:blank. So assert THAT — the
#    invariant that actually holds — rather than the removed toggle.
case "$out" in
  *'--app=about:blank'* ) ;;
  *) echo "Chromium must launch blank (native peer owns the session post-M7)" >&2
     echo "$out" >&2; exit 1 ;;
esac

echo "[streamer-signaling-url-contract] OK"

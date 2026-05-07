#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

out="$(
  SESSION_ID='cb:11111111-2222-3333-4444-555555555555' \
  SIGNALING_URL='ws://triform.triform-wtf.svc.cluster.local:3000/api/webrtc/signaling' \
  SIGNALING_TOKEN='header.payload.signature' \
  CHROMELESS_BROWSER_BIN='/bin/echo' \
  sh infra/launch-chromeless.sh 2>&1
)"

case "$out" in
  *'session=cb:11111111-2222-3333-4444-555555555555'* ) ;;
  *) echo "missing Pattern-C session id in launch output" >&2; echo "$out" >&2; exit 1 ;;
esac

case "$out" in
  *'token=header.payload.signature'* ) ;;
  *) echo "missing signaling token in streamer URL" >&2; echo "$out" >&2; exit 1 ;;
esac

node <<'NODE'
const fs = require("node:fs");
const vm = require("node:vm");
const src = fs.readFileSync("capture/streamer-page/streamer.js", "utf8");
if (!src.includes("buildSignalingWsUrl")) {
  throw new Error("streamer.js must keep buildSignalingWsUrl helper");
}
const sample = new URL("ws://triform/api/webrtc/signaling/cb%3Aabc?role=browser&token=tok");
if (sample.searchParams.get("role") !== "browser" || sample.searchParams.get("token") !== "tok") {
  throw new Error("URLSearchParams sanity check failed");
}
new vm.Script(src, { filename: "streamer.js" });
NODE

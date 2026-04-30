#!/usr/bin/env bash
#
# T91: rendering-fixture orchestrator (canonical CI path).
#
# Boots the cloud-browser-webrtc:dev container, navigates the
# already-running Chromium (the streamer Chromium serves DevTools
# on :9222 — same instance that the streamer page is loaded into,
# so the rendering tests run under the production launch flag set:
# X11 ozone, ANGLE+SwiftShader-webgl, Vulkan disabled).
#
# Loads each fixture, captures a screenshot, scrapes
# `window.__results__` from the page, writes one summary JSON +
# one screenshot per fixture under `tests/rendering/results/`.
#
# Pair this with `local-probe.js` (Playwright on the dev host)
# for fixture-validation iteration without docker. This script is
# the canonical "what does the cloud-browser-webrtc:dev image
# actually do" gate.
#
# Usage:
#   tests/rendering/run-rendering-tests.sh           # run against
#                                                    # an already-up
#                                                    # compose stack.
#   tests/rendering/run-rendering-tests.sh --boot    # `docker compose
#                                                    # up -d --build`
#                                                    # first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
RESULTS_DIR="${SCRIPT_DIR}/results"
COMPOSE_FILE="${REPO_ROOT}/infra/compose.yaml"

DEVTOOLS_HOST="${CBWRTC_RENDERING_DEVTOOLS:-http://127.0.0.1:9222}"
PROFILE="${CBWRTC_RENDERING_PROFILE:-swiftshader-v1}"
BOOT_STACK=0
SAVE_BASELINES=0

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--boot] [--save-baselines] [--devtools URL] [--profile NAME]

  --boot              Run \`docker compose up -d --build\` first.
  --save-baselines    Copy this run's screenshots over the baseline
                      checked into tests/rendering/baselines/.
  --devtools URL      DevTools URL to drive (default: $DEVTOOLS_HOST).
  --profile NAME      Tag the results dir with this profile name
                      (default: $PROFILE). Useful when comparing
                      multiple Chromium configurations.
  -h, --help          Show this message.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --boot)            BOOT_STACK=1; shift;;
        --save-baselines)  SAVE_BASELINES=1; shift;;
        --devtools)        DEVTOOLS_HOST="$2"; shift 2;;
        --profile)         PROFILE="$2"; shift 2;;
        -h|--help)         usage; exit 0;;
        *)                 echo "unknown arg: $1" >&2; usage; exit 2;;
    esac
done

mkdir -p "${RESULTS_DIR}"

log() { printf '[rendering] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

require_tool() {
    local tool="$1"
    if ! command -v "${tool}" >/dev/null 2>&1; then
        die "missing required tool: ${tool}"
    fi
}
require_tool curl
require_tool jq
require_tool python3

if [[ "${BOOT_STACK}" -eq 1 ]]; then
    require_tool docker
    log "docker compose up -d --build"
    docker compose -f "${COMPOSE_FILE}" up -d --build >/dev/null
    log "waiting for chromium DevTools on ${DEVTOOLS_HOST}..."
    for _ in $(seq 1 60); do
        if curl -fsS "${DEVTOOLS_HOST}/json/version" >/dev/null 2>&1; then
            log "DevTools is up"
            break
        fi
        sleep 2
    done
fi

if ! curl -fsS "${DEVTOOLS_HOST}/json/version" >/dev/null 2>&1; then
    die "DevTools unreachable at ${DEVTOOLS_HOST} — start the stack with --boot or point --devtools at it"
fi

TARGET_JSON="$(curl -fsS "${DEVTOOLS_HOST}/json/list")"
WS_URL="$(printf '%s' "${TARGET_JSON}" | jq -r '.[] | select(.type=="page") | .webSocketDebuggerUrl' | head -n1)"
if [[ -z "${WS_URL}" || "${WS_URL}" == "null" ]]; then
    die "no page target in DevTools list"
fi
log "driving target via ${WS_URL}"

run_fixture() {
    local fixture="$1"
    local fixture_url="$2"
    local out_json="${RESULTS_DIR}/${PROFILE}-${fixture}-results.json"
    local out_png="${RESULTS_DIR}/${PROFILE}-${fixture}-screenshot.png"

    log "running ${fixture} fixture: ${fixture_url}"

    /usr/bin/env python3 - "${WS_URL}" "${fixture_url}" "${out_json}" "${out_png}" "${fixture}" <<'PY'
import base64
import json
import sys
import time

import websocket

ws_url, fixture_url, out_json, out_png, fixture_name = sys.argv[1:]

ws = websocket.create_connection(ws_url, timeout=30)
_id = [0]

def call(method, params=None):
    _id[0] += 1
    rid = _id[0]
    ws.send(json.dumps({"id": rid, "method": method, "params": params or {}}))
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == rid:
            if "error" in msg:
                raise RuntimeError(f"{method}: {msg['error']}")
            return msg.get("result", {})

call("Page.enable")
call("Runtime.enable")
call("Page.navigate", {"url": fixture_url})

deadline = time.time() + 30
result_value = None
while time.time() < deadline:
    res = call("Runtime.evaluate", {
        "expression":
          "window.__results__ && window.__results__.summary "
          "? JSON.stringify(window.__results__) : null",
        "returnByValue": True,
    })
    val = res.get("result", {}).get("value")
    if val:
        result_value = val
        break
    time.sleep(0.5)

if result_value is None:
    print(f"[orchestrator] {fixture_name}: TIMEOUT (no __results__ within 30s)",
          file=sys.stderr)
    json.dump({"error": "timeout", "fixture": fixture_name},
              open(out_json, "w"))
    sys.exit(2)

with open(out_json, "w") as f:
    f.write(result_value)

shot = call("Page.captureScreenshot", {"format": "png"})
with open(out_png, "wb") as f:
    f.write(base64.b64decode(shot["data"]))

summary = json.loads(result_value).get("summary", {})
print(f"[orchestrator] {fixture_name}: ok={summary.get('ok')} "
      f"warn={summary.get('warn')} err={summary.get('err')} "
      f"total={summary.get('total')}", file=sys.stderr)
PY
    return $?
}

WEBGL_URL="${CBWRTC_RENDERING_WEBGL_URL:-http://localhost:9000/streamer-tests/rendering/webgl-fixture.html}"
WEBGPU_URL="${CBWRTC_RENDERING_WEBGPU_URL:-http://localhost:9000/streamer-tests/rendering/webgpu-fixture.html}"

run_fixture webgl  "${WEBGL_URL}"
run_fixture webgpu "${WEBGPU_URL}"

BASELINES_DIR="${SCRIPT_DIR}/baselines"
mkdir -p "${BASELINES_DIR}"

if [[ "${SAVE_BASELINES}" -eq 1 ]]; then
    log "copying screenshots to baselines/"
    cp -f "${RESULTS_DIR}"/"${PROFILE}"-*-screenshot.png "${BASELINES_DIR}/"
fi

fail=0
for fixture in webgl webgpu; do
    new="${RESULTS_DIR}/${PROFILE}-${fixture}-screenshot.png"
    base="${BASELINES_DIR}/${PROFILE}-${fixture}-screenshot.png"
    if [[ ! -f "${new}" ]]; then
        log "WARN: no ${PROFILE}-${fixture} screenshot produced"
        fail=1
        continue
    fi
    if [[ ! -f "${base}" ]]; then
        log "INFO: no baseline for ${PROFILE}-${fixture} yet — re-run with --save-baselines once you've vetted the result"
        continue
    fi
    if ! cmp -s "${new}" "${base}"; then
        log "DIFF: ${PROFILE}-${fixture} screenshot differs from baseline"
        fail=1
    fi
done

log ""
log "summary:"
for fixture in webgl webgpu; do
    j="${RESULTS_DIR}/${PROFILE}-${fixture}-results.json"
    [[ -f "${j}" ]] || { log "  ${fixture}: no result"; continue; }
    s="$(jq -c '.summary // {error:"missing"}' "${j}")"
    log "  ${fixture}: ${s}"
done

exit "${fail}"

#!/usr/bin/env bash
# tests/smoke/metrics-presence.sh — assert that both Prometheus
# endpoints (signaling and the chromium-side metrics sidecar) return
# non-empty exposition output and at least the metric names we declared
# in T38.
#
# Usage:
#   ./tests/smoke/metrics-presence.sh [SIGNALING_BASE] [SIDECAR_BASE]
#
# Defaults assume the compose stack from infra/compose.yaml is up and
# ports are published to the host:
#   SIGNALING_BASE = http://127.0.0.1:8080
#   SIDECAR_BASE   = http://127.0.0.1:9100
#
# Exit codes:
#   0  PASS — both endpoints responded with the expected metric names
#   1  FAIL — endpoint reachable but missing expected metrics
#   2  ENV  — required tool missing (curl, grep)
#   3  PRE  — endpoint not reachable

set -euo pipefail

SIGNALING_BASE="${1:-http://127.0.0.1:8080}"
SIDECAR_BASE="${2:-http://127.0.0.1:9100}"

log() { printf '[metrics-smoke] %s\n' "$*" >&2; }

require() {
    command -v "$1" >/dev/null 2>&1 || {
        log "missing required tool: $1"
        exit 2
    }
}
require curl
require grep

# fetch_metrics URL -> stdout body, or exit 3 with a clear message.
fetch_metrics() {
    local url="$1"
    local body
    if ! body=$(curl -fsS --max-time 5 "$url"); then
        log "cannot reach $url — is the service up?"
        exit 3
    fi
    if [ -z "$body" ]; then
        log "empty response from $url"
        exit 1
    fi
    printf '%s' "$body"
}

# expect_metrics body label expected_names...
expect_metrics() {
    local body="$1"; shift
    local label="$1"; shift
    local missing=()
    for name in "$@"; do
        # The exposition format publishes either `# HELP name ...` or
        # `name{...} value` / `name value` lines. Either is sufficient
        # evidence the metric exists.
        if ! grep -qE "^(# HELP ${name}|${name}[ {])" <<<"$body"; then
            missing+=("$name")
        fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        log "FAIL: ${label} missing metrics: ${missing[*]}"
        return 1
    fi
    log "PASS: ${label} exposes all expected metrics"
    return 0
}

# ---- signaling --------------------------------------------------------

log "scraping signaling at ${SIGNALING_BASE}/metrics"
SIGNALING_BODY=$(fetch_metrics "${SIGNALING_BASE}/metrics")

SIGNALING_EXPECTED=(
    cb_signaling_sessions_total
    cb_signaling_messages_total
    cb_signaling_close_total
    cb_signaling_active_sessions
    cb_signaling_active_connections
)

expect_metrics "$SIGNALING_BODY" "signaling" "${SIGNALING_EXPECTED[@]}" \
    || exit 1

# ---- sidecar ----------------------------------------------------------

log "scraping sidecar at ${SIDECAR_BASE}/metrics"
SIDECAR_BODY=$(fetch_metrics "${SIDECAR_BASE}/metrics")

SIDECAR_EXPECTED=(
    cb_chromium_cpu_pct
    cb_chromium_rss_bytes
    cb_webrtc_outbound_bitrate_bps
    cb_webrtc_outbound_frames_per_second
    cb_webrtc_outbound_dropped_frames_total
    cb_webrtc_outbound_qp
    cb_webrtc_remote_inbound_packets_lost_total
    cb_webrtc_round_trip_time_ms
)

expect_metrics "$SIDECAR_BODY" "sidecar" "${SIDECAR_EXPECTED[@]}" \
    || exit 1

log "PASS: both /metrics endpoints expose the expected names"

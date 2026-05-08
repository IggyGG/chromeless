#!/usr/bin/env bash
# Static contract for the Phase 2 runtime image build context.
#
# The cr7727-* image path is staged by build/chromeless-build.sh and built
# by build/Dockerfile.runtime. Keep this test cheap: it catches packaging
# regressions before an operator spends hours on a Chromium rebuild.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

fail() {
  printf 'phase2-runtime-contract: %s\n' "$*" >&2
  exit 1
}

require_file() {
  [[ -f "$1" ]] || fail "missing required file: $1"
}

require_grep() {
  local pattern="$1"
  local file="$2"
  rg -n -- "$pattern" "$file" >/dev/null || fail "expected pattern '$pattern' in $file"
}

reject_grep() {
  local pattern="$1"
  local file="$2"
  if rg -n -- "$pattern" "$file" >/dev/null; then
    fail "unexpected pattern '$pattern' in $file"
  fi
}

require_file build/Dockerfile.runtime
require_file build/chromeless-build.sh
require_file infra/launch-chromeless.sh
require_file infra/supervisord.phase2.conf
require_file infra/streamer-static-server.py
require_file capture/chromeless-metrics-sidecar/Dockerfile

require_grep 'cp -R "\$\{CHROMELESS_REPO\}/capture/streamer-page/\."' build/chromeless-build.sh
require_grep 'cp "\$\{CHROMELESS_REPO\}/infra/supervisord.phase2.conf"' build/chromeless-build.sh
require_grep 'cp "\$\{CHROMELESS_REPO\}/infra/pulse-default.pa"' build/chromeless-build.sh
require_grep 'cp "\$\{CHROMELESS_REPO\}/infra/devtools-proxy.sh"' build/chromeless-build.sh
require_grep 'cp "\$\{CHROMELESS_REPO\}/infra/streamer-static-server.py"' build/chromeless-build.sh
require_grep 'cp -R "\$\{CHROMELESS_REPO\}/infra/lifecycle"' build/chromeless-build.sh
require_grep 'for runtime_asset in icudtl.dat libEGL.so libGLESv2.so libvk_swiftshader.so' build/chromeless-build.sh

require_grep 'COPY[[:space:]]+streamer/[[:space:]]+/opt/cloud-browser/streamer/' build/Dockerfile.runtime
require_grep 'COPY[[:space:]]+pulse-default.pa[[:space:]]+/etc/pulse/default.pa' build/Dockerfile.runtime
require_grep 'COPY[[:space:]]+devtools-proxy.sh[[:space:]]+/usr/local/bin/devtools-proxy.sh' build/Dockerfile.runtime
require_grep 'COPY[[:space:]]+streamer-static-server.py[[:space:]]+/usr/local/bin/streamer-static-server.py' build/Dockerfile.runtime
require_grep 'COPY[[:space:]]+lifecycle/[[:space:]]+/usr/local/lib/cloud-browser/lifecycle/' build/Dockerfile.runtime
require_grep 'ENTRYPOINT \["/usr/bin/dumb-init", "--", "/usr/local/bin/entrypoint.sh"\]' build/Dockerfile.runtime

require_grep 'CHROMELESS_BROWSER_BIN' infra/launch-chromeless.sh
require_grep 'STREAMER_INPUT_URL' infra/launch-chromeless.sh
require_grep 'STREAMER_CDP_URL' infra/launch-chromeless.sh
require_grep 'CHROMELESS_AUTOSTART_STREAMER' infra/launch-chromeless.sh
require_grep 'input=\$\{STREAMER_INPUT_URL\}' infra/launch-chromeless.sh
require_grep 'cdp=\$\{STREAMER_CDP_URL\}' infra/launch-chromeless.sh
require_grep '/cdp/json/version' infra/streamer-static-server.py

reject_grep 'chromeless-metrics-sidecar' infra/supervisord.phase2.conf
reject_grep 'IDLE_TIMEOUT_S="600"' infra/supervisord.phase2.conf
require_grep 'streamer-static-server.py' infra/supervisord.phase2.conf
require_grep 'OTEL_EXPORTER_OTLP_LOGS_ENDPOINT' infra/helm/chromeless/templates/default-pool.yaml
require_grep 'OTEL_EXPORTER_OTLP_LOGS_ENDPOINT' infra/helm/chromeless/templates/sw-pool.yaml
require_grep 'CHROMELESS_AUTOSTART_STREAMER' infra/helm/chromeless/templates/default-pool.yaml
require_grep 'CHROMELESS_AUTOSTART_STREAMER' infra/helm/chromeless/templates/sw-pool.yaml
require_grep 'name: input-bridge' infra/helm/chromeless/templates/default-pool.yaml
require_grep 'name: input-bridge' infra/helm/chromeless/templates/sw-pool.yaml
require_grep 'skipping cluster CDP validation' build/chromeless-build.sh

printf 'phase2-runtime-contract: ok\n'

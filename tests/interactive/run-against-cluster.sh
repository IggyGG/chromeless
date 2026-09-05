#!/usr/bin/env bash
# tests/interactive/run-against-cluster.sh — the whole interactive run, one command.
#
#   ./tests/interactive/run-against-cluster.sh                 # all suites
#   ./tests/interactive/run-against-cluster.sh --only mouse,keyboard
#   ./tests/interactive/run-against-cluster.sh --restart       # fresh worker first
#
# WHY THIS EXISTS
# ---------------
# The suite needs five things arranged before it can say anything, and every
# one of them produced a wrong answer at least once while this was done by
# hand on 2026-08-17:
#
#   1. A LIVE SESSION SLOT. The worker re-arms its peer connection when a
#      viewer leaves (CV2-REARM), so a restart between runs is no longer
#      required — but a guest that predates the re-armable driver serves one
#      session per process, and a stray client from an interrupted run holds
#      the session's single `client` slot either way. Both present as "client
#      never decoded a frame", which reads as a broken product. `--restart`
#      buys a known-clean guest when you want one.
#   2. THE TEST FIXTURE ENABLED. The gateway's plain-HTTP fixture endpoint is
#      opt-in. Without it the suite dies on a bare `HTTP Error 404` from a POST,
#      which reads as a broken gateway.
#   3. BOTH PORT-FORWARDS, re-established AFTER any restart. A forward to a pod
#      that has been replaced fails with RemoteDisconnected mid-run, taking out
#      whichever suite happened to be running.
#   4. NO STRAY TEST CHROME. Chrome outlives an interrupted harness and keeps
#      the session's `client` slot; the next run then gets no video. 57 had
#      accumulated before anyone noticed.
#   5. CREDENTIALS, which deploy.sh writes to .standalone-creds and rotates on
#      every deploy — so a stale cookie 401s against credentials that look
#      correct in the file.
#
# Getting any of these wrong does not fail loudly; it produces a plausible
# WRONG RESULT. That is the whole reason this is a script and not a README.
#
# It does NOT deploy the stack — that is deploy.sh's job, and re-deploying
# between runs would rotate credentials for no reason. Run it first if needed:
#
#   ./infra/k8s/standalone/deploy.sh --with-test-fixture
set -euo pipefail

NS=${CHROMELESS_NS:-chromeless}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
CREDS="$REPO_ROOT/infra/k8s/standalone/.standalone-creds"
GATEWAY_PORT=8443
WORKER_CDP_PORT=19222

RESTART=0
PASSTHRU=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --restart)    RESTART=1; shift ;;
        --no-restart) RESTART=0; shift ;;   # accepted for callers of the older shape
        *)            PASSTHRU+=("$1"); shift ;;
    esac
done

cleanup() {
    # Always drop the forwards we started. Leaving them running makes the NEXT
    # run bind a port that already has a stale tunnel on it.
    [[ -n "${PF_GW:-}"  ]] && kill "$PF_GW"  2>/dev/null || true
    [[ -n "${PF_CDP:-}" ]] && kill "$PF_CDP" 2>/dev/null || true
}
trap cleanup EXIT

die() { echo "run-against-cluster: $*" >&2; exit 1; }

# ---- preconditions, checked before anything is torn down -------------------

command -v kubectl >/dev/null 2>&1 || die "kubectl not on PATH"
kubectl get ns "$NS" >/dev/null 2>&1 || die "namespace $NS not reachable"
kubectl get deploy chromeless-standalone-worker -n "$NS" >/dev/null 2>&1 \
    || die "the standalone stack is not deployed. Run:
    ./infra/k8s/standalone/deploy.sh --with-test-fixture"
[[ -f "$CREDS" ]] || die "no credentials at $CREDS — run deploy.sh first"

# The fixture endpoint is opt-in and its absence is a confusing 404 mid-run.
if ! kubectl get deploy chromeless-standalone-gateway -n "$NS" \
     -o jsonpath='{.spec.template.spec.containers[0].env[?(@.name=="CHROMELESS_ENABLE_TEST_FIXTURE")].value}' \
     2>/dev/null | grep -q 1; then
    die "the gateway has no test fixture. The suite needs it to put a known
    page in front of the worker. Redeploy with:
      ./infra/k8s/standalone/deploy.sh --with-test-fixture"
fi

# ---- 1. reap strays --------------------------------------------------------

# The profile path is the harness's own (harness.py ClientDriver.profile), so
# an unrelated Chrome is never touched.
pkill -9 -f "chromeless-itest-profile" 2>/dev/null || true
pkill -f "port-forward.*${GATEWAY_PORT}:${GATEWAY_PORT}" 2>/dev/null || true
pkill -f "port-forward.*${WORKER_CDP_PORT}:9222" 2>/dev/null || true
sleep 2

# ---- 2. optionally, a fresh worker ----------------------------------------

if [[ "$RESTART" == "1" ]]; then
    echo ">>> restarting the worker (--restart)"
    kubectl rollout restart deploy/chromeless-standalone-worker -n "$NS" >/dev/null
    kubectl rollout status deploy/chromeless-standalone-worker -n "$NS" \
        --timeout=300s >/dev/null || die "worker did not become ready"
fi

# ---- 3. port-forwards, AFTER any restart so they target the live pod -------

POD="$(kubectl get pod -n "$NS" \
    -l app.kubernetes.io/name=chromeless-standalone-worker \
    -o jsonpath='{.items[0].metadata.name}')"
[[ -n "$POD" ]] || die "no worker pod found"

kubectl port-forward -n "$NS" svc/chromeless-standalone-gateway \
    "${GATEWAY_PORT}:${GATEWAY_PORT}" >/tmp/pf-gateway.log 2>&1 &
PF_GW=$!
kubectl port-forward -n "$NS" "$POD" \
    "${WORKER_CDP_PORT}:9222" >/tmp/pf-worker-cdp.log 2>&1 &
PF_CDP=$!

# Poll rather than sleep: a fixed sleep is either too short (flaky) or too long
# (wasted on every run).
echo ">>> waiting for port-forwards"
gw=000; cdp=000
for _ in $(seq 1 30); do
    gw=$(curl -sk -o /dev/null -w '%{http_code}' \
         "https://localhost:${GATEWAY_PORT}/healthz" 2>/dev/null || echo 000)
    # Host: localhost is load-bearing — the worker's DNS-rebinding guard answers
    # 403 to any other Host header (infra/gateway/cdp.go, trap 1).
    cdp=$(curl -s -H "Host: localhost" -o /dev/null -w '%{http_code}' \
          "http://127.0.0.1:${WORKER_CDP_PORT}/json/version" 2>/dev/null || echo 000)
    if [[ "$gw" == "200" && "$cdp" == "200" ]]; then
        echo "    gateway=$gw worker-cdp=$cdp"
        break
    fi
    sleep 1
done
[[ "$gw" == "200" ]]  || die "gateway never answered on :${GATEWAY_PORT} (see /tmp/pf-gateway.log)"
[[ "$cdp" == "200" ]] || die "worker CDP never answered on :${WORKER_CDP_PORT} (see /tmp/pf-worker-cdp.log)"

# ---- 4. run ----------------------------------------------------------------

echo ">>> running the interactive suite"
set +e
python3 "$HERE/run.py" ${PASSTHRU[@]+"${PASSTHRU[@]}"}
rc=$?
set -e

if [[ $rc -ne 0 ]]; then
    echo
    echo "run-against-cluster: suite reported failures (exit $rc)."
    echo "  The expected score is in tests/interactive/README.md. Before debugging"
    echo "  the product, check which image and which bundle you actually tested:"
    echo "    kubectl get pod -n $NS $POD -o jsonpath='{.spec.containers[0].image}'"
    echo "    (the suite's preflight also reports a stale gateway bundle by name)"
    echo "  A guest that predates the re-armable driver needs --restart between runs."
fi
exit $rc

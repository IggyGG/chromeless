#!/usr/bin/env bash
# infra/k8s/standalone/deploy.sh — bring the standalone stack up on k8s.
#
# Everything the README describes, in one script, because doing it by hand
# means re-deriving the worker's token every time and getting the TURN
# credential subtly wrong.
#
#   ./infra/k8s/standalone/deploy.sh                     # deploy
#   ./infra/k8s/standalone/deploy.sh --with-test-fixture # + tests/interactive/
#   ./infra/k8s/standalone/deploy.sh --teardown          # remove what it made
#
# Writes the login password to .standalone-creds in this directory (gitignored)
# so a test harness can read it without it passing through a shell history.
#
# ASSUMES the images are already built — `kubectl apply -f build-images.yaml`
# and wait for both Jobs. They are built from a PUSHED branch; kaniko clones
# from the remote.
set -euo pipefail

NS=chromeless
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
LABEL=app.kubernetes.io/part-of=chromeless-standalone
CREDS="$HERE/.standalone-creds"

# --with-test-fixture enables the gateway's plain-HTTP fixture endpoint, which
# tests/interactive/ needs to put a known page in front of the worker. OFF by
# default and deliberately opt-in: the listener is unauthenticated (its consumer
# is the worker, which has no session cookie).
#
# It lives here rather than in stack.yaml because it was set by hand once, and
# the next ./deploy.sh silently dropped it — the suite then failed with a bare
# `HTTP Error 404` from a POST, which reads as a broken gateway rather than as a
# missing flag.
WITH_FIXTURE=""
for a in "$@"; do
    [[ "$a" == "--with-test-fixture" ]] && WITH_FIXTURE=1
done

if [[ "${1:-}" == "--check-turn" ]]; then
    # Is the DEPLOYED TURN credential still valid? This is the first thing to
    # check when a working stack starts failing to pair: an expired credential
    # presents as "connects, negotiates, no video", exactly like a NAT problem.
    U="$(kubectl get deploy chromeless-standalone-signaling -n "$NS" \
        -o jsonpath='{.spec.template.spec.containers[0].env[?(@.name=="TURN_USER")].value}')"
    python3 -c "
import sys,time
u='$U'
if not u: print('  no TURN_USER set on the broker'); sys.exit(1)
left=int(u.split(':')[0])-int(time.time())
print(f'  TURN credential: {\"EXPIRED \" + str(-left) + \"s ago — run ./deploy.sh\" if left<0 else str(left)+\"s remaining\"}')
sys.exit(0 if left>0 else 1)"
    exit $?
fi

if [[ "${1:-}" == "--teardown" ]]; then
    kubectl delete all,secret,cm,networkpolicy -n "$NS" -l "$LABEL" 2>&1 | tail -3
    rm -f "$CREDS"
    echo "torn down"
    exit 0
fi

echo ">>> secrets"
# The gateway signs session tokens; the broker verifies them. Both halves come
# from one keygen run — the broker needs the public key at ITS startup, before
# the gateway exists, so neither service can hand the other anything at boot.
eval "$(cd "$REPO_ROOT/infra/gateway" && go run ./cmd/keygen)"
PASS="$(openssl rand -hex 12)"

kubectl create secret generic chromeless-standalone -n "$NS" \
    --from-literal=CHROMELESS_USER=chromeless \
    --from-literal=CHROMELESS_PASS="$PASS" \
    --from-literal=CHROMELESS_AUTH_PRIVKEY="$CHROMELESS_AUTH_PRIVKEY" \
    --from-literal=CHROMELESS_AUTH_PUBKEY="$CHROMELESS_AUTH_PUBKEY" \
    --dry-run=client -o yaml | kubectl label -f - --local -o yaml "$LABEL" 2>/dev/null |
    kubectl apply -f - >/dev/null

# The worker needs a BROWSER-role token and cannot refresh one:
# cb_signaling_reconnect.h is explicit that token refresh is out of scope — the
# process exits and expects an orchestrator to restart it with a fresh one. The
# gateway's issuer mints 15-minute tokens, which is useless for a worker, so
# mint a long-lived one with the same key.
TOKEN="$(cd "$REPO_ROOT/infra/gateway" && \
    CHROMELESS_AUTH_PRIVKEY="$CHROMELESS_AUTH_PRIVKEY" SESSION_ID=dev \
    go run ./cmd/worker-token)"
kubectl create secret generic chromeless-standalone-worker-token -n "$NS" \
    --from-literal=token="$TOKEN" \
    --dry-run=client -o yaml | kubectl label -f - --local -o yaml "$LABEL" 2>/dev/null |
    kubectl apply -f - >/dev/null

echo ">>> TURN credential"
# A relay is effectively mandatory here: the worker is a cluster pod and the
# viewer is a laptop, both behind NAT, so host and srflx candidates never pair.
# This namespace ENFORCES PodSecurity baseline, which forbids the hostNetwork a
# relay needs — hence borrowing the existing one rather than deploying coturn.
# It runs use-auth-secret (HMAC-REST), so the credential is time-limited.
TURN_SECRET="$(kubectl get secret coturn-secrets -n triform-production \
    -o jsonpath='{.data.TURN_STATIC_AUTH_SECRET}' | base64 -d)"
# ⚠️ HARDCODED, and it broke on 2026-09-09. coturn runs with hostNetwork in
# triform-production, so its address is whatever NODE it lands on. It was
# rescheduled off triform-1 (95.217.200.179) onto triform-7/8, and this IP
# then had nothing listening on 3478.
#
# The symptom is NOT "TURN rejected us": it is `Connection with server failed
# with error: 111` (ECONNREFUSED) in the guest log and a session that
# negotiates, gathers candidates, and never decodes a frame — identical to an
# expired credential, which is the wrong thing to go and fix first. Tell them
# apart in one command from inside the worker pod:
#
#   timeout 4 bash -c '</dev/tcp/<ip>/3478' && echo open || echo refused
#
# refused = the relay moved (this); open + no video = check the credential.
#
# Find where it actually is:
#   kubectl get pods -n triform-production -o wide | grep coturn
#   kubectl get nodes -o wide          # map node -> INTERNAL-IP
#
# NOTE the worker ALSO carries its own copy in WEBRTC_ICE_SERVERS. Patching
# the broker's TURN_URLS alone does not reach it, and the guest log keeps
# printing the old address — which reads as the patch not applying.
#
# So: DISCOVER it rather than hardcode it. The hardcoded value here was
# 95.217.200.179 and by 2026-09-10 that address refused 3478 outright —
# coturn had moved to triform-7. A `deploy.sh` run on that day would have
# deployed a dead relay and reported success, which is the failure the
# comment above describes, embedded in the script that documents it.
#
# Ask the cluster where coturn is, then PROVE the answer by dialling it.
# TURN_IP=<addr> overrides for an external relay.
#
# TURN_DEPLOY names WHICH coturn, because there is more than one: on
# 2026-09-10 `coturn` sat on triform-7 and `coturn-2` on triform-8, both
# answering 3478. Picking "the first Running coturn pod" is a coin flip, and
# both halves of the flip pass a reachability test — so if the two ever hold
# different static-auth secrets, the wrong pick yields a relay that refuses
# every allocation while looking healthy, with no error anywhere. (Checked
# on that date: both run `lt-cred-mech` + `use-auth-secret` with
# realm=triform.cloud, so either would have worked. That is luck, not a
# guarantee, and it is not visible from the pod list.)
TURN_NS="${TURN_NS:-triform-production}"
TURN_DEPLOY="${TURN_DEPLOY:-coturn}"
if [[ -z "${TURN_IP:-}" ]]; then
    # Select by the `app=` LABEL, not by a name prefix. "coturn-2" starts with
    # "coturn-" and 2 is a hex digit, so every name-prefix pattern I tried —
    # including ^coturn-[0-9a-f]+- — matched the WRONG deployment and returned
    # a plausible node. The label is exact.
    turn_node="$(kubectl get pods -n "${TURN_NS}" -l "app=${TURN_DEPLOY}" \
        --field-selector=status.phase=Running \
        -o jsonpath='{.items[0].spec.nodeName}' 2>/dev/null)"
    if [[ -n "${turn_node}" ]]; then
        TURN_IP="$(kubectl get node "${turn_node}" \
            -o jsonpath='{.status.addresses[?(@.type=="ExternalIP")].address}' 2>/dev/null)"
        [[ -z "${TURN_IP}" ]] && TURN_IP="$(kubectl get node "${turn_node}" \
            -o jsonpath='{.status.addresses[?(@.type=="InternalIP")].address}' 2>/dev/null)"
        [[ -n "${TURN_IP}" ]] && echo ">>> coturn: ${TURN_DEPLOY} on ${turn_node}"
    fi
fi
if [[ -z "${TURN_IP:-}" ]]; then
    echo "ERROR: could not find a running coturn pod, and TURN_IP is unset." >&2
    echo "  kubectl get pods -A -o wide | grep coturn" >&2
    echo "  then re-run with TURN_IP=<addr>" >&2
    exit 1
fi
# Dial it. A relay that is merely NAMED is the exact failure mode above:
# the stack comes up, negotiates, and shows no video.
if ! timeout 5 bash -c "cat < /dev/null > /dev/tcp/${TURN_IP}/3478" 2>/dev/null; then
    echo "ERROR: ${TURN_IP}:3478 refused the connection — that relay is not there." >&2
    echo "  Deploying it would yield 'connects, negotiates, no video'." >&2
    echo "  kubectl get pods -A -o wide | grep coturn   # where is it now?" >&2
    exit 1
fi
echo ">>> TURN relay ${TURN_IP}:3478 (discovered, reachable)"
read -r TURN_USER TURN_CRED < <(python3 - "$TURN_SECRET" <<'PY'
import base64, hashlib, hmac, sys, time
secret = sys.argv[1]
# RFC 7635: username = <unix expiry>:<name>, password = base64(HMAC-SHA1).
#
# The expiry is a HARD DEADLINE on the whole deployment, not a session TTL:
# nothing re-mints it, so when it passes, coturn answers 401 to every
# allocation and the stack degrades to "connects, negotiates, no video" —
# indistinguishable from a NAT problem unless you test the credential directly.
# Cost me an afternoon of ICE debugging when a 6-hour credential aged out
# mid-session while the deployment stayed up.
#
# 24h, and ./deploy.sh re-mints. If a run starts failing to pair, check this
# first: python3 -c "import time;print(<expiry> - int(time.time()))".
user = f"{int(time.time()) + 24 * 3600}:chromeless-standalone"
cred = base64.b64encode(
    hmac.new(secret.encode(), user.encode(), hashlib.sha1).digest()).decode()
print(user, cred)
PY
)
ICE_JSON="$(python3 -c "
import json,sys
print(json.dumps([
  {'urls':['turn:$TURN_IP:3478?transport=udp','turn:$TURN_IP:3478?transport=tcp'],
   'username':'$TURN_USER','credential':'$TURN_CRED'},
  {'urls':['stun:$TURN_IP:3478']}]))")"

# ---------------------------------------------------------------------------
# Refuse to roll the cluster BACKWARDS.
#
# `kubectl apply -f stack.yaml` below sets the worker image to whatever this
# file pins. On 2026-08-24 that pin was 64 commits behind the fix under test,
# so every run of this script silently reverted a running fix — and every
# `kubectl set image` reverted it back. The worker Deployment reached revision
# 66 in a day with nobody reverting anything on purpose; kubectl showed two
# field managers (`kubectl-client-side-apply` and `kubectl-set`) both owning
# spec.containers[].image, each pulling toward its own answer.
#
# The user lost a day to it: tests passed against an image they never saw, and
# they hit a bug those tests had "proved" fixed.
#
# So: if the live worker is running something NEWER than this file pins, stop
# and say so rather than quietly downgrading it. `make lint-deploy-pin` keeps
# stack.yaml and build/guest-release.json in agreement; this is the runtime
# half, for the case where the cluster has moved and the tree has not.
#
# CHROMELESS_ALLOW_ROLLBACK=1 proceeds anyway — deliberate downgrades are
# legitimate, they just should not be silent.
pinned_worker="$(grep -oE 'chromeless/chromeless:[^ ]+' "$HERE/stack.yaml" | head -1 | sed 's|.*:||')"
live_worker="$(kubectl get deploy chromeless-standalone-worker -n "$NS" \
    -o jsonpath='{.spec.template.spec.containers[0].image}' 2>/dev/null | sed 's|.*:||' || true)"
if [ -n "$live_worker" ] && [ -n "$pinned_worker" ] && [ "$live_worker" != "$pinned_worker" ]; then
    echo "ERROR: applying stack.yaml would CHANGE the running worker image." >&2
    echo "         live:   $live_worker" >&2
    echo "         pinned: $pinned_worker   (infra/k8s/standalone/stack.yaml)" >&2
    echo "" >&2
    echo "  If the live image is the newer one, this apply is a ROLLBACK and is" >&2
    echo "  almost certainly not what you want. Update the pin in stack.yaml and" >&2
    echo "  build/guest-release.json together, then re-run." >&2
    echo "  To proceed anyway: CHROMELESS_ALLOW_ROLLBACK=1 $0 $*" >&2
    [ "${CHROMELESS_ALLOW_ROLLBACK:-}" = "1" ] || exit 1
    echo "  CHROMELESS_ALLOW_ROLLBACK=1 set — proceeding." >&2
fi

echo ">>> network policy + stack"
# Not optional: the namespace carries chromeless-default-deny with
# podSelector:{}, so new pods get nothing. The symptom is a TIMEOUT rather than
# a refusal, which reads as "the other service is down".
kubectl apply -f "$HERE/networkpolicy.yaml" >/dev/null
kubectl apply -f "$HERE/stack.yaml" >/dev/null

kubectl set env deploy/chromeless-standalone-worker -n "$NS" \
    WEBRTC_ICE_SERVERS="$ICE_JSON" >/dev/null
if [[ -n "$WITH_FIXTURE" ]]; then
    echo "    test fixture ENABLED (plain HTTP :8081, unauthenticated)"
    kubectl set env deploy/chromeless-standalone-gateway -n "$NS" \
        CHROMELESS_ENABLE_TEST_FIXTURE=1 >/dev/null
else
    # Explicitly OFF, not merely absent: a redeploy must be able to turn it
    # back off, and `kubectl set env` with no value leaves whatever was there.
    kubectl set env deploy/chromeless-standalone-gateway -n "$NS" \
        CHROMELESS_ENABLE_TEST_FIXTURE- >/dev/null 2>&1 || true
fi

kubectl set env deploy/chromeless-standalone-signaling -n "$NS" \
    TURN_URLS="turn:$TURN_IP:3478?transport=udp,turn:$TURN_IP:3478?transport=tcp" \
    TURN_USER="$TURN_USER" TURN_PASS="$TURN_CRED" \
    STUN_URLS="stun:$TURN_IP:3478" >/dev/null

# Restart everything, THEN wait. A Deployment does not restart when a Secret it
# references changes, so on a re-deploy the pods keep serving the OLD password
# and the OLD signing key while the Secret holds the new ones — login then
# fails with 401 against credentials that look correct in the file.
for d in signaling worker gateway; do
    kubectl rollout restart "deploy/chromeless-standalone-$d" -n "$NS" >/dev/null
done
for d in signaling worker gateway; do
    kubectl rollout status "deploy/chromeless-standalone-$d" -n "$NS" --timeout=240s >/dev/null
    echo "    $d ready"
done

# The worker registers with the broker on its FIRST envelope. If it connected
# while the broker was restarting it holds a live socket that never registered,
# and the client then waits for an offer that will never come. Restarting the
# worker last avoids that ordering entirely.
kubectl rollout restart deploy/chromeless-standalone-worker -n "$NS" >/dev/null
kubectl rollout status deploy/chromeless-standalone-worker -n "$NS" --timeout=240s >/dev/null

umask 077
printf 'CHROMELESS_USER=chromeless\nCHROMELESS_PASS=%s\n' "$PASS" > "$CREDS"

echo ">>> up. credentials in $CREDS"
echo "    kubectl port-forward -n $NS svc/chromeless-standalone-gateway 8443:8443"

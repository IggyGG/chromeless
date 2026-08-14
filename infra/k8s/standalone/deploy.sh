#!/usr/bin/env bash
# infra/k8s/standalone/deploy.sh — bring the standalone stack up on k8s.
#
# Everything the README describes, in one script, because doing it by hand
# means re-deriving the worker's token every time and getting the TURN
# credential subtly wrong.
#
#   ./infra/k8s/standalone/deploy.sh              # deploy
#   ./infra/k8s/standalone/deploy.sh --teardown   # remove everything it made
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
TURN_IP=95.217.200.179
read -r TURN_USER TURN_CRED < <(python3 - "$TURN_SECRET" <<'PY'
import base64, hashlib, hmac, sys, time
secret = sys.argv[1]
# RFC 7635: username = <unix expiry>:<name>, password = base64(HMAC-SHA1).
user = f"{int(time.time()) + 6 * 3600}:chromeless-standalone"
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

echo ">>> network policy + stack"
# Not optional: the namespace carries chromeless-default-deny with
# podSelector:{}, so new pods get nothing. The symptom is a TIMEOUT rather than
# a refusal, which reads as "the other service is down".
kubectl apply -f "$HERE/networkpolicy.yaml" >/dev/null
kubectl apply -f "$HERE/stack.yaml" >/dev/null

kubectl set env deploy/chromeless-standalone-worker -n "$NS" \
    WEBRTC_ICE_SERVERS="$ICE_JSON" >/dev/null
kubectl set env deploy/chromeless-standalone-signaling -n "$NS" \
    TURN_URLS="turn:$TURN_IP:3478?transport=udp,turn:$TURN_IP:3478?transport=tcp" \
    TURN_USER="$TURN_USER" TURN_PASS="$TURN_CRED" \
    STUN_URLS="stun:$TURN_IP:3478" >/dev/null

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

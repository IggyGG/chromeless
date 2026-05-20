#!/usr/bin/env bash
# cv2-wave2-reverify-runbook.sh — verification-lead pre-staged re-verification
# runbook for Wave 2 (rv8). Fill the two tags below, then run section-by-section.
# NOT meant for unattended `bash runbook.sh` — run each numbered block, read output.
set -uo pipefail

# ─── FILL THESE WHEN build-czar RELAYS THE TAGS ───────────────────────────
RV8_TAG=""              # e.g. cr7727-<sha>   (chromeless cb-chromium rv8)
SIGNALING_TAG="cv2-route-alias"   # registry.triform.cloud/chromeless/signaling:<this>
# ──────────────────────────────────────────────────────────────────────────
NS=chromeless-tests
REG=registry.triform.cloud/chromeless
WTREE=/private/tmp/chromeless-wave-2-integration
HARNESS_MAIN=/Users/iggy/Documents/GitHub/chromeless/tests/webrtc
HARNESS_WT=$WTREE/tests/webrtc
[ -n "$RV8_TAG" ] || { echo "SET RV8_TAG FIRST"; }

# ═══ 1. SIGNALING BROKER (can run as soon as the signaling image lands — pre-rv8)
sed "s#chromeless/signaling:cv2-route-alias#chromeless/signaling:${SIGNALING_TAG}#" \
  "$WTREE/infra/k8s/cv2/cv2-signaling-broker.yaml" | kubectl apply -f -
kubectl wait --for=condition=Available deploy/signaling -n $NS --timeout=120s
# lesson-(l) broker-reachability check:
kubectl run cv2-brokerprobe --image=curlimages/curl:8.5.0 -n $NS --restart=Never --rm -i --quiet -- \
  curl -sf -m5 http://signaling.chromeless-tests.svc.cluster.local:8080/healthz && echo "BROKER /healthz OK"

# ═══ 2. RE-IMAGE cv2-82-real-pulse → rv8
sed "s#chromeless:cr7727-24c5c0d#chromeless:${RV8_TAG}#" \
  "$WTREE/infra/k8s/cv2/cv2-82-real-pulse.yaml" > /tmp/cv2-82-real-pulse-rv8.yaml
kubectl delete pod cv2-82-real-pulse -n $NS --ignore-not-found --wait --timeout=90s
kubectl apply -f /tmp/cv2-82-real-pulse-rv8.yaml

# ═══ 3. DEPLOY M4 worker → rv8
sed "s#chromeless:cr7727-rv8-pending#chromeless:${RV8_TAG}#" \
  "$WTREE/infra/k8s/cv2/cv2-81-m4-worker.yaml" > /tmp/cv2-81-m4-worker-rv8.yaml
kubectl delete pod cv2-81-m4-worker -n $NS --ignore-not-found --wait --timeout=90s
kubectl apply -f /tmp/cv2-81-m4-worker-rv8.yaml

# ═══ 4. WAIT + lesson-(l) PRE-VERIFICATION (4-axis: image / pod / signaling / DOM)
kubectl wait --for=condition=Ready pod/cv2-82-real-pulse pod/cv2-81-m4-worker -n $NS --timeout=260s
for P in cv2-82-real-pulse cv2-81-m4-worker; do
  echo "== $P =="
  kubectl get pod $P -n $NS -o jsonpath='{.spec.containers[0].image}{"  "}{.status.phase}{"\n"}'
  kubectl exec $P -n $NS -c cb-chromium -- grep -E "cb_signaling: (dialing|connected)|signaling subsystem disabled" \
    /var/log/supervisor/chromium.err.log | tail -3
done
# expect: each worker logs "cb_signaling: connected" (NOT "...disabled" / NOT ERR_NAME_NOT_RESOLVED)

# ═══ 5. FIRE M5.5  (CV2-82 — R1 audio-init / R2 default-source / R5 teardown-parity)
# 5a. PC-DRIVER: the worker builds its media-PCF/ADM lazily on PeerConnection
#     creation. An idle worker => R1 reads "LS_INFO absent" (false-NEGATIVE).
#     Drive a peer against session cv2-82-real-pulse FIRST so the worker builds
#     the media-PCF -> ADM Init -> the audio log line lands.
kubectl port-forward svc/signaling -n $NS 8080:8080 >/tmp/m55-pf-broker.log 2>&1 & PF0=$!
sleep 4
( cd "$HARNESS_MAIN" && BROKER_URL=ws://localhost:8080/api/webrtc/signaling/cv2-82-real-pulse \
    node phase-a-m5.5-pc-driver.mjs )
kill $PF0 2>/dev/null
# 5b. Now read the audio verdict (worker media-PCF/ADM are now built).
node "$HARNESS_WT/phase-a-m5.5-r1-pulse-init.mjs" --pod cv2-82-real-pulse --namespace $NS --variant real-pulse
bash "$WTREE/harness/m5.5/run_r2_default_source.sh"  --pod cv2-82-real-pulse --namespace $NS
bash "$WTREE/harness/m5.5/run_r5_teardown_parity.sh" --pod cv2-82-real-pulse --namespace $NS
# If R1 STILL shows LS_INFO absent after the PC-driver ran clean (exit 0):
# that IS a real CV2-82 signal — inspect for the line-110 LS_WARNING (real-Pulse
# env broken) or the PCF LS_WARNING (native-ADM nullptr fallback).

# ═══ 6. FIRE M5-R1  (CV2-88 — cursor routing); CDP-only
kubectl port-forward pod/cv2-82-real-pulse -n $NS 9222:9222 >/tmp/m5r1-pf.log 2>&1 & PF1=$!
sleep 4
( cd "$HARNESS_MAIN" && node phase-a-m5-r1-percursor-event.mjs )
kill $PF1 2>/dev/null
# verdict grep:
kubectl exec cv2-82-real-pulse -n $NS -c cb-chromium -- \
  grep -E "CbCursorClient::SetCursor.*new_type=2|NOTIMPLEMENTED.*display::ScreenBase::(IsWindowUnderCursor|GetCursorScreenPoint|GetDisplayNearestWindow)" \
  /var/log/supervisor/chromium.err.log || echo "(no SetCursor / no NOTIMPLEMENTED)"

# ═══ 7. FIRE M4  (CV2-81 — typed dispatch); needs broker :8080 + worker CDP :9222
kubectl port-forward svc/signaling             -n $NS 8080:8080 >/tmp/m4-pf-broker.log 2>&1 & PF2=$!
kubectl port-forward pod/cv2-81-m4-worker      -n $NS 9222:9222 >/tmp/m4-pf-cdp.log    2>&1 & PF3=$!
sleep 4
( cd "$HARNESS_MAIN" && node phase-a-m4-typed-dispatch.mjs )   # BROKER_URL default already matches session
kill $PF2 $PF3 2>/dev/null
# verdict grep (ANCHOR-A/B present, NEG-C/D absent):
kubectl exec cv2-81-m4-worker -n $NS -c cb-chromium -- sh -c '
  echo "ANCHOR-A:"; grep -c "CbInputDispatchCompositeDelegate ctor" /var/log/supervisor/chromium.err.log
  echo "ANCHOR-B:"; grep -c "composite delegate = R3..R8 typed pipeline" /var/log/supervisor/chromium.err.log
  echo "NEG-C (must be 0):"; grep -c "CbInputDispatch: dispatched type=" /var/log/supervisor/chromium.err.log
  echo "NEG-D (must be 0):"; grep -cE "CbInputDispatch(Mouse|Keyboard|Drag|Clipboard):|cb-ime:.*dropped|touch_start: no active" /var/log/supervisor/chromium.err.log
'

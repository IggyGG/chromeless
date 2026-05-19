# CV2 Wave functional-test — M5 R1 per-event verification template (CV2-78/87).
#
# Stable template for drafter-generated phase-A M5 R1 verification pods.
# Drafters copy this into /tmp/cv2-phase-a-m5-r1-<rev>.yaml and substitute:
#   __IMAGE_REF__   — pinned digest, e.g. `registry.triform.cloud/chromeless/chromeless@sha256:<hex>`
#   __REV_TAG__     — short rev label, e.g. `rv6` (used for pod/configmap/labels)
#   __HARNESS_CM__  — ConfigMap name carrying the harness, e.g. `cv2-m5-r1-<rev>-harness`
#
# Why this template lives in-tree
# ───────────────────────────────
# CV2-87 (2026-05-20) revealed cross-boundary literal drift: chromium-7727
# split `--use-gl` into `--use-gl` + `--use-angle`, the in-image
# `infra/launch-chromeless.sh` was updated, BUT prior drafter-generated
# phase-A pod manifests in /tmp/ carried the stale `--use-gl=swiftshader`
# form. Each new drafter re-derived from the stale /tmp/ copy.
#
# Landing the template here gives drafters a single source-of-truth to
# regenerate from, and any future chromium-N flag drift is patched in one
# place (this file + infra/launch-chromeless.sh).
#
# Sub-lesson (g.4): cross-boundary literal drift can occur INDEPENDENTLY
# in parallel codepaths consuming the same external convention. The image's
# launch script and the test-pod manifest are independent consumers of the
# chromium GL-flag convention — enumerate ALL consumers when source-
# verifying literals.
#
# Purpose: verify CbHeadlessScreen subclass (CV2-78 commit f4062a9) closes
# the Aura cursor-routing gate by exercising the renderer-driven mousemove
# path:
#
#   Input.dispatchMouseEvent mouseMoved (10,10) over `data:text/html,<a>link</a>`
#     → aura asks display::Screen::IsWindowUnderCursor (now CbHeadlessScreen → true)
#     → aura proceeds into CursorClient routing
#     → CbCursorClient::SetCursor LOG fires with new_type=2 (kHand) within ~2s
#
# Container layout (2 containers; bootstrap is a tiny initContainer copying
# the ConfigMap-injected harness into the workspace volume):
#   bootstrap (initContainer)   : alpine. Copies the harness files mounted at
#                                 /harness into /workspace/tests/webrtc.
#   cb-chromium                 : __IMAGE_REF__. Boots Xvfb + cloud_browser_worker
#                                 with --remote-debugging-port=9222.
#   client-test-driver          : node:20-bookworm. Waits for CDP up, then runs
#                                 phase-a-m5-r1-percursor-event.mjs.
#
# Acceptance (verdict derived OUTSIDE this pod from `kubectl logs ... -c cb-chromium`):
#   PASS:  cb-chromium stderr contains `CbCursorClient::SetCursor` with `new_type=2`
#          (kHand, ui::mojom::CursorType) within ~2s after the stimulus timestamp,
#          AND NOTIMPLEMENTED LOG_ONCE lines for display::ScreenBase::IsWindowUnderCursor
#          / GetCursorScreenPoint are ABSENT post-Screen-registration.
#   FAIL:  NOTIMPLEMENTED still present → CbHeadlessScreen not installed (build-czar).
#   FAIL:  NOTIMPLEMENTED absent but SetCursor not reached → downstream wiring gap.
#   FAIL:  Worker crash post-stimulus → thread-discipline regression.
#
# Cleanup:
#   kubectl -n chromeless-tests delete pod cv2-phase-a-m5-r1-__REV_TAG__ configmap/__HARNESS_CM__
---
apiVersion: v1
kind: Pod
metadata:
  name: cv2-phase-a-m5-r1-__REV_TAG__
  namespace: chromeless-tests
  labels:
    app.kubernetes.io/name: chromeless
    app.kubernetes.io/component: functional-test
    app.kubernetes.io/variant: cv2-m5-r1-percursor-event-__REV_TAG__
    chromeless.cv2/track: software
    chromeless.cv2/campaign: wave-1
    chromeless.cv2/ticket: cv2-78
spec:
  restartPolicy: Never
  nodeSelector:
    kubernetes.io/hostname: triform-8
  imagePullSecrets:
    - name: registry-pull
  activeDeadlineSeconds: 1800
  shareProcessNamespace: true
  initContainers:
    - name: bootstrap
      image: alpine:3.20
      command:
        - sh
        - -euxc
        - |
          mkdir -p /workspace/tests/webrtc
          cp /harness/phase-a-m5-r1-percursor-event.mjs /workspace/tests/webrtc/
          cp /harness/package.json /workspace/tests/webrtc/
          cp /harness/package-lock.json /workspace/tests/webrtc/
          ls -la /workspace/tests/webrtc/
      securityContext:
        allowPrivilegeEscalation: false
        capabilities: { drop: ["ALL"] }
      volumeMounts:
        - { name: workspace, mountPath: /workspace }
        - { name: harness,   mountPath: /harness }

  containers:
    # ────────────────── 1/2 cb-chromium ──────────────────
    - name: cb-chromium
      image: __IMAGE_REF__
      imagePullPolicy: IfNotPresent
      env:
        - { name: CHROMELESS_REGION, value: "triform-eu-test" }
        - { name: HOME,              value: "/tmp/cb-home" }
      command:
        - bash
        - -euxc
        - |
          mkdir -p $HOME /tmp/out
          Xvfb :99 -screen 0 1280x720x24 -nolisten tcp -ac &
          XVFB_PID=$!
          sleep 1
          export DISPLAY=:99

          # GL flags: post-chromium-7727 form. The split is:
          #   --use-gl=angle               selects ANGLE as the GL backend
          #   --use-angle=swiftshader-webgl  software-rasterise via SwiftShader
          #   --enable-unsafe-swiftshader  allow SwiftShader in production-shape
          # Mirror infra/launch-chromeless.sh; do NOT regress to --use-gl=swiftshader
          # (the pre-7727 form). See CV2-87.
          exec /usr/local/bin/cloud_browser_worker \
            --no-sandbox \
            --disable-dev-shm-usage \
            --display=:99 \
            --remote-debugging-port=9222 \
            --remote-debugging-address=0.0.0.0 \
            --remote-allow-origins=* \
            --user-data-dir=$HOME/profile \
            --no-first-run \
            --no-default-browser-check \
            --use-gl=angle \
            --use-angle=swiftshader-webgl \
            --enable-unsafe-swiftshader \
            --ozone-platform=x11 \
            --window-size=1280,720 \
            --disable-features=TranslateUI,MediaRouter,Vulkan,VaapiVideoDecodeLinuxGL,IntensiveWakeUpThrottling,CalculateNativeWinOcclusion,BackForwardCacheMemoryControls \
            --disable-background-timer-throttling \
            --disable-renderer-backgrounding \
            --disable-backgrounding-occluded-windows \
            about:blank
      ports:
        - { containerPort: 9222, name: cdp, protocol: TCP }
      resources:
        requests: { cpu: "1",   memory: "2Gi", ephemeral-storage: "1Gi" }
        limits:   { cpu: "4",   memory: "8Gi", ephemeral-storage: "2Gi" }
      securityContext:
        allowPrivilegeEscalation: false
        capabilities: { drop: ["ALL"] }
      volumeMounts:
        - { name: dshm,       mountPath: /dev/shm }
        - { name: cb-tmp,     mountPath: /tmp }
        - { name: out-vol,    mountPath: /tmp/out }
        - { name: x11-socket, mountPath: /tmp/.X11-unix }

    # ────────────────── 2/2 client test driver ──────────────────
    - name: client-test-driver
      image: node:20-bookworm
      workingDir: /workspace
      command:
        - bash
        - -euxc
        - |
          # Wait for cb-chromium's CDP to come up.
          for i in $(seq 1 60); do
            if curl -fsS http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
              echo "cb-chromium CDP up after ${i}s"
              break
            fi
            sleep 1
          done

          cd /workspace/tests/webrtc
          npm install --no-audit --no-fund

          export CDP_HOST="127.0.0.1"
          export CDP_PORT=9222
          export CDP_CONNECT_TIMEOUT_MS=60000
          export POST_STIMULUS_WAIT_MS=3000

          echo "M5 R1 per-event cursor-routing test starting..."
          if node phase-a-m5-r1-percursor-event.mjs 2>&1 | tee /tmp/out/m5-r1.log; then
            HARNESS_EXIT=0
            echo "M5 R1 wire-test: STIMULUS COMPLETE (verdict derived externally from cb-chromium stderr)"
          else
            HARNESS_EXIT=$?
            echo "M5 R1 wire-test: FAIL (exit=${HARNESS_EXIT}) — see /tmp/out/m5-r1.log for diagnostic"
          fi
          echo "m5_r1_harness_exit_code=${HARNESS_EXIT}" > /tmp/out/m5-r1-exit-code

          echo "M5 R1 harness done (exit=${HARNESS_EXIT}); parking for log retrieval."
          sleep infinity
      resources:
        requests: { cpu: "500m", memory: "1Gi", ephemeral-storage: "2Gi" }
        limits:   { cpu: "2",    memory: "4Gi", ephemeral-storage: "4Gi" }
      securityContext:
        allowPrivilegeEscalation: false
        capabilities: { drop: ["ALL"] }
      volumeMounts:
        - { name: workspace,  mountPath: /workspace }
        - { name: out-vol,    mountPath: /tmp/out }
        - { name: node-cache, mountPath: /root/.npm }

  volumes:
    - name: workspace
      emptyDir: { sizeLimit: "1Gi" }
    - name: harness
      configMap:
        name: __HARNESS_CM__
    - name: node-cache
      emptyDir: { sizeLimit: "1Gi" }
    - name: dshm
      emptyDir: { medium: Memory, sizeLimit: "1Gi" }
    - name: cb-tmp
      emptyDir: { sizeLimit: "1Gi" }
    - name: x11-socket
      emptyDir: { sizeLimit: "16Mi" }
    - name: out-vol
      emptyDir: { sizeLimit: "512Mi" }

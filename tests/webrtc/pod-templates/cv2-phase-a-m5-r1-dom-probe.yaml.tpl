# CV2 Wave functional-test — M5 R1 renderer-DOM PROBE template (CV2-78 axis diagnosis).
#
# Sibling template to cv2-phase-a-m5-r1.yaml.tpl. This pod runs a DIFFERENT
# harness (phase-a-m5-r1-dom-probe.mjs) that does NOT fire a cursor stimulus —
# it only probes the renderer state after Page.loadEventFired + a 2 s settle.
# Goal: disambiguate two axes that could explain an absent
# `CbCursorClient::SetCursor` log on the rv6.b verification fire:
#
#   Axis 1 (Screen-routing gate)  — Aura short-circuits at
#     display::ScreenBase::GetDisplayNearestWindow before reaching
#     IsWindowUnderCursor.
#   Axis 2 (Renderer-DOM gate)    — renderer never laid out the document,
#     so there's nothing under the cursor to derive a style from.
#
# Drafter substitution:
#   __IMAGE_REF__   — pinned digest, e.g. `registry.triform.cloud/chromeless/chromeless@sha256:<hex>`
#   __REV_TAG__     — short rev label, e.g. `rv6-dom-probe`
#   __HARNESS_CM__  — ConfigMap name carrying the harness
#
# Probe outputs (in client-test-driver stdout, last line is the structured
# `PROBE COMPLETE` JSON):
#   document.readyState, body.innerHTML length+excerpt, link outerHTML,
#   link.getBoundingClientRect, computed cursor style, document dims,
#   Page.captureScreenshot (base64 length only), DOM.getDocument tree size.
#
# Verdict mapping (orchestrator interprets):
#   * link rect has real w/h + screenshot ok                 → renderer healthy
#                                                            → Axis 1 sole gate
#   * innerHTML present but rect=null and/or screenshot fail → renderer parsed,
#                                                              never composed
#                                                            → Axis 2 partial
#   * innerHTML empty AND rect=null AND screenshot fail
#     AND DOM.getDocument empty                              → renderer broken
#                                                            → Axis 2 sole gate
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
    app.kubernetes.io/variant: cv2-m5-r1-dom-probe
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
          cp /harness/phase-a-m5-r1-dom-probe.mjs /workspace/tests/webrtc/
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

          # GL flags: post-chromium-7727 form (see cv2-phase-a-m5-r1.yaml.tpl).
          # Do NOT regress to --use-gl=swiftshader.
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
          export POST_LOAD_SETTLE_MS=2000

          echo "M5 R1 renderer-DOM probe starting..."
          if node phase-a-m5-r1-dom-probe.mjs 2>&1 | tee /tmp/out/m5-r1-dom-probe.log; then
            HARNESS_EXIT=0
            echo "M5 R1 DOM probe: COMPLETE (structured results in log)"
          else
            HARNESS_EXIT=$?
            echo "M5 R1 DOM probe: FAIL (exit=${HARNESS_EXIT}) — see /tmp/out/m5-r1-dom-probe.log for diagnostic"
          fi
          echo "m5_r1_dom_probe_exit_code=${HARNESS_EXIT}" > /tmp/out/m5-r1-dom-probe-exit-code

          echo "M5 R1 DOM probe done (exit=${HARNESS_EXIT}); parking for log retrieval."
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

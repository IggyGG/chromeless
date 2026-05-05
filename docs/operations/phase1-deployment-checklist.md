# Phase 1 deployment-readiness checklist

A pre-flight checklist for taking chromeless from "the
team's dev cluster" to "a real Phase 1 deployment that real users
hit." Everything below should be ✓ before you let traffic in.

Pair with [`runbook.md`](./runbook.md) (the day-2 operations) and
[`sla.md`](./sla.md) (the SLOs your alerting must enforce).

## 0. Decide what Phase 1 means for you

Phase 1 isn't multi-tenant production. It's:

- One company / one tenant (or auth disabled with deliberate
  acceptance of the trust model).
- Software encode only (NVENC / VAAPI live in the codebase but are
  Phase 4 verified).
- Single region (multi-region is Phase 3+).
- Daily-bounce tolerance: planned restarts of the whole stack are
  acceptable; no SLO commitment yet on uptime.

If your deployment doesn't match this shape, defer until the
relevant phase — Phase 1 won't carry you.

---

## 1. Code-level TODOs cleared

### Hard requirements

- [ ] Every `TODO(<task-id>)` comment in code paths the deploy will
      execute is either resolved or explicitly accepted in your
      deploy notes. Search:
      ```
      git grep -nE 'TODO\(T[0-9]+'
      ```
- [ ] T17 chromium-from-source build environment markers cleared if
      the deploy uses a custom Chromium image. Otherwise stock
      `infra/Dockerfile` is fine; document that you're on the stock
      Debian Chromium package.
- [ ] T63/T69 NVENC/VAAPI HW encoder TODOs cleared **only if** the
      deploy enables HW encode. Phase 1 should default to software;
      flip later.

### Soft (acceptable to defer)

- [ ] T23 `--use-fake-ui-for-media-stream` is documented in
      `docs/security/auth.md` as a known v1 expedient.
- [ ] T57's `--no-sandbox` is documented in
      `infra/security-hardening.md`. Phase 3 sandbox decision (T44)
      replaces this.
- [ ] T68 CRIU snapshot/restore is **not** yet wired through the
      controller (`chromeless.io/snapshot-id` is read-only metadata in T71).
      Phase 1 cold-starts; that's fine.

---

## 2. Auth and secrets

- [ ] **Ed25519 keypair generated** and the pubkey base64-encoded
      lives in:
  - `Secret signaling-auth` (consumed by `signaling`)
  - `Secret turn-issuer-secrets.CHROMELESS_AUTH_PUBKEY` (consumed by
    `turn-issuer`)
  Both must hold the same value or sessions can't fetch TURN creds.
- [ ] **Auth issuer** (the upstream system that mints session tokens
      with the matching private key) is wired to your auth provider
      (Auth0 / Okta / Cognito / homegrown). Out of scope for this
      repo; Phase 1 typically uses a small internal issuer or the
      dev issuer in `signaling/dev-issuer.go` flagged behind a
      production-blocking env var.
- [ ] **TURN shared secret** generated with `openssl rand -hex 32`
      and synced between:
  - `Secret turn-rest-secret.static-auth-secret` (coturn)
  - `Secret turn-issuer-secrets.CHROMELESS_TURN_SHARED_SECRET`
- [ ] TLS certificates:
  - `signaling.<your-domain>` cert provisioned (cert-manager or
    pre-imported).
  - `turn.<your-domain>` cert provisioned for TLS-TURN (5349).
- [ ] All Secret values are **encrypted at rest** in your secret
      store (Vault, sealed-secrets, SOPS, External Secrets — pick
      one and stick to it).

---

## 3. TURN issuer + coturn

- [ ] TURN issuer Deployment running (`turn-issuer`, replicas: 2).
- [ ] coturn StatefulSet running with `hostNetwork: true` on a
      dedicated node pool with the right firewall:
  - UDP 3478, 5349 inbound from the public internet.
  - UDP 49152-65535 outbound for relay allocations.
  - For TURN-on-cloud-LB: confirm UDP support on your load balancer
    (GCP/AWS NLB/Azure LB all support UDP with caveats — check).
- [ ] Sample curl-from-the-internet to verify:
      ```
      TOKEN=<a valid token from your issuer>
      curl -fsS https://turn-issuer.<domain>/issue-turn-cred \
          -H "Authorization: Bearer $TOKEN" \
          -d '{"sessionId":"smoketest","ttlSeconds":300}'
      ```
      Should return `{username, credential, ttl, urls, iceServers}`.

---

## 4. Observability

- [ ] **Prometheus** scraping:
  - `signaling:8080/metrics` → `cb_signaling_*`
  - `chromeless-metrics-sidecar` per session → `cb_chromium_*`,
    `cb_webrtc_*`, `cb_client_*` (T72).
  - `turn-issuer:8090/metrics` → `cb_turn_issuer_*`.
  - `browser-session-controller:8080/metrics` →
    `controller_runtime_*`, custom `cb_session_*`.
- [ ] **Grafana** dashboards imported (T66):
  - `chromeless-cluster-overview` (default home).
  - `chromeless-session-detail`.
- [ ] **AlertManager** rules loaded from
      `infra/observability/alerts/chromeless-alerts.yaml`. `promtool check
      rules` clean.
- [ ] **Alert routing** wired to your paging system (PagerDuty,
      Slack, whatever):
  - `severity=critical` → page.
  - `severity=warning` → channel notification.

---

## 5. Smoke tests against the deployment image

Run **against the actual image you intend to ship** — not whatever
qa-tester ran during dev:

- [ ] `tests/smoke/container-boot.sh` — Chromium renders a test page.
- [ ] `tests/smoke/audio-presence.sh` — outbound audio bytes flowing.
- [ ] `tests/smoke/security-posture.sh` — readOnlyRootFilesystem,
      non-root user, dropped caps.
- [ ] `tests/smoke/metrics-presence.sh` — both /metrics endpoints
      respond with the expected metric names.
- [ ] (If snapshots enabled) `tests/smoke/snapshot-restore.sh` —
      snapshot+restore round-trip < 2 s.

---

## 6. Harness baseline

The latency harness (T10/T11/T12) is **the** v1 acceptance gate:

- [ ] Set up the harness rig per `harness/latency/README.md` (camera
      pointed at a client display loading the harness page).
- [ ] Run a 5-minute baseline against a representative session
      (real user simulating common interaction patterns).
- [ ] Glass-to-glass latency p95:
  - **LAN target**: < 100 ms.
  - **Regional target** (cross-zone): < 200 ms.
- [ ] Numbers recorded against
      [`docs/v1-success-criteria.md`](../v1-success-criteria.md). If
      below target — congrats, you're shipping. If above — diagnose
      via [`runbook.md#debugging-a-slow-session`](./runbook.md#debugging-a-slow-session).

---

## 7. Sandbox runtime decision

T44's sandbox decision must be made before public deploy. Pick one:

- [ ] **runc + AppArmor + Seccomp (T57)**. Acceptable for
      single-tenant Phase 1 where the trust boundary is the company
      perimeter, not the container. Document this.
- [ ] **gVisor (`runsc`)**. Best on GKE; document driver-version
      pinning if GPU is in scope.
- [ ] **Kata + Cloud Hypervisor**. Strongest isolation + GPU-friendly;
      most operational complexity.

The choice goes in your deploy README + the Pod manifest's
`runtimeClassName` (or stays runc with the comment in
`infra/k8s/cloud-browser-session.yaml`).

---

## 8. Networking

- [ ] **Ingress** `signaling.<domain>` reachable from the public
      internet, terminates TLS, supports WebSocket Upgrade
      (proxy-read-timeout ≥ 60 m for long-lived signaling).
- [ ] **NetworkPolicy** in place limiting session pods to:
  - egress-only NAT to the public internet.
  - drop RFC1918 destinations (10/8, 172.16/12, 192.168/16).
  - drop cloud metadata IPs (169.254.169.254, fd00:ec2::254).
  - allow signaling + turn-issuer + coturn services in-cluster.
- [ ] **PodDisruptionBudgets** for stateless tiers (signaling,
      turn-issuer, controller) so cluster maintenance can't take
      everything offline at once.

---

## 9. Capacity sizing

Document the published "sessions per host" number in
[`docs/v1-success-criteria.md`](../v1-success-criteria.md):

- [ ] Run 1, 2, 4, 8, 16 concurrent sessions on a node-class you'll
      ship. Record glass-to-glass p95 + CPU saturation point.
- [ ] Pick the concurrency where the v1 latency target still holds.
- [ ] Cluster-autoscaler thresholds (CPU 60%, memory 70%) tuned
      against this.

---

## 10. Day-2 operational artefacts

- [ ] [`runbook.md`](./runbook.md) read by everyone on call.
- [ ] [`sla.md`](./sla.md) signed off by leadership.
- [ ] [`postmortem-template.md`](./postmortem-template.md) saved
      somewhere your incident tooling can find it.
- [ ] On-call rotation set up.
- [ ] Cluster-admin access confirmed for the on-call.

---

## 11. Sign-off

- [ ] Engineering sign-off: this checklist is complete.
- [ ] SRE/ops sign-off: alerts fire, runbook is real, on-call is real.
- [ ] Product/leadership sign-off: SLA is acceptable, the deploy
      timeline is acceptable, the rollback plan is acceptable.

When all three boxes are checked, we ship. If any box can't be
checked, **don't ship** — the gap will surface in production at
3 AM and someone will write a postmortem about why the checklist
was a fiction. Don't let that someone be you.

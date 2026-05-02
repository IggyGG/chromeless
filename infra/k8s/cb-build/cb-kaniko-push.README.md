# cb-kaniko-push.yaml — push the runtime cb-chromium image after a build

This Job pushes the runtime image `cb-chromium:<tag>` to
`registry.triform.cloud/cloud-browser-webrtc/` using kaniko, reading
the build context staged by `cb-build.sh` Step 9 from the chromium-src
hostPath of whichever node ran the variant.

It is **not** part of the kustomize bundle (see `kustomization.yaml`).
Apply it explicitly after a successful `cb-build-${variant}` Job:

```bash
kubectl apply -f infra/k8s/cb-build/cb-kaniko-push.yaml
```

The manifest defaults to the **sw / x264** profile on **triform-2**.
For other variants, edit the file in place per § Variant overrides
below before applying.

## Why a separate manifest

`build-job-{x264,vaapi,nvenc}.yaml` runs `cb-build.sh`. Step 9 of
that script stages a kaniko-ready build context to
`${CB_WORK_ROOT}/artifacts/context/` (typically
`/mnt/data/cb-build-t{2,5}/chromium-src/artifacts/context/`) but
deliberately does **not** push. The push is split out so:

- Registry credentials live in a single `registry-pull` Secret
  consumed only by this Job — the build container never sees them.
- The BUGS-513 registry replica scale-down (see below) can be
  performed without touching the long-running build Pod.
- Variant builds can complete in parallel; the pushes serialize
  through this single Job at operator-controlled cadence.

A future T112 wiring would re-merge the push into the build Job as a
sidecar container, at which point this manifest is deletable. Until
then it's the canonical push path and is re-applied after every
build cycle.

## Activation procedure

1. **Wait for cb-build-${variant} to reach Succeeded.**
   ```bash
   kubectl get -n cb-build job cb-build-x264 -o jsonpath=
'{.status.conditions[?(@.type=="Complete")].status}{"\n"}'
   # Should print: True
   ```

2. **Confirm the build context is staged on the right node.**
   ```bash
   # x264 / vaapi (triform-2):
   ssh triform-2 ls /mnt/data/cb-build-t2/chromium-src/artifacts/context
   # Expect: Dockerfile  IMAGE_TAG  cloud_browser_worker  launch-chromium.sh  supervisord.conf

   # nvenc (triform-5):
   ssh triform-5 ls /mnt/data/cb-build-t5/chromium-src/artifacts/context
   ```

3. **Apply the BUGS-513 registry scale-to-1 workaround** (see
   below). You can skip this step for the first push attempt and
   only run it if kaniko reports a 5xx on manifest PUT.

4. **Apply the Job.** If it's already applied from a prior cycle,
   delete it first — Jobs are immutable and `apply` won't overwrite.
   ```bash
   kubectl -n cb-build delete job cb-kaniko-push --ignore-not-found
   kubectl apply -f infra/k8s/cb-build/cb-kaniko-push.yaml
   kubectl logs -n cb-build job/cb-kaniko-push -f
   ```

5. **Verify the tag landed in the registry.**
   ```bash
   curl -sf -u "${FORGEJO_USERNAME}:${FORGEJO_PASSWORD}" \
     https://registry.triform.cloud/v2/cloud-browser-webrtc/cb-chromium/manifests/cr7727-sw \
     -H 'Accept: application/vnd.docker.distribution.manifest.v2+json' \
     -o /dev/null -w '%{http_code}\n'
   # Expect: 200
   ```

6. **Restore registry replicas** (post-push half of the BUGS-513
   workaround).

## Variant overrides

| Variant | nodeName | hostPath path | CB_KANIKO_TAG |
|---------|----------|----------------|---------------|
| sw / x264 (default) | `triform-2` | `/mnt/data/cb-build-t2/chromium-src/artifacts/context` | `cr7727-sw` |
| vaapi | `triform-2` | `/mnt/data/cb-build-t2/chromium-src/artifacts/context` | `cr7727-vaapi` |
| nvenc | `triform-5` | `/mnt/data/cb-build-t5/chromium-src/artifacts/context` | `cr7727-nvenc` |

The fields to edit live at three call sites in the manifest:

- `spec.template.spec.nodeName`
- `spec.template.spec.nodeSelector["kubernetes.io/hostname"]`
- `spec.template.spec.volumes[name=workspace].hostPath.path`
- `spec.template.spec.containers[0].env[name=CB_KANIKO_TAG].value`
- `metadata.labels["app.kubernetes.io/variant"]` and the matching
  pod-template label (informational only; the Job still runs)

For nvenc:

```yaml
spec:
  template:
    spec:
      nodeName: triform-5
      nodeSelector:
        kubernetes.io/hostname: triform-5
      containers:
        - name: kaniko
          env:
            - name: CB_KANIKO_TAG
              value: "cr7727-nvenc"
      volumes:
        - name: workspace
          hostPath:
            path: /mnt/data/cb-build-t5/chromium-src/artifacts/context
            type: Directory
```

For vaapi (only the tag changes from default):

```yaml
            - name: CB_KANIKO_TAG
              value: "cr7727-vaapi"
```

A future improvement (out of scope for this manifest) would lift
the per-variant fields into a kustomize overlay set:
`overlays/{x264,vaapi,nvenc}/`. Today the in-place edit is simpler
than wiring three overlays and never breaks because each variant
push is a discrete operator action.

## BUGS-513 — registry replica scale-down workaround

The `registry` Deployment runs 2 replicas behind a ClusterIP
Service for HA. Without `REGISTRY_HTTP_SECRET` shared across
replicas (BUGS-513), an HTTP/2-multiplexed kaniko upload that hops
between replicas during a single upload session 5xxs because the
upload-UUID is opaque per-replica. The portable fix is to teach
the registry to share its session secret; until that ships,
operators scale the registry to 1 replica around the push.

```bash
# Pre-push: scale to 1 replica.
kubectl -n registry scale deployment registry --replicas=1
kubectl -n registry rollout status deployment registry --timeout=60s

# Apply the kaniko-push Job.
kubectl -n cb-build delete job cb-kaniko-push --ignore-not-found
kubectl apply -f infra/k8s/cb-build/cb-kaniko-push.yaml
kubectl logs -n cb-build job/cb-kaniko-push -f

# Post-push: restore HA.
kubectl -n registry scale deployment registry --replicas=2
kubectl -n registry rollout status deployment registry --timeout=60s
```

This dance is intentionally **not** baked into the manifest as
init / poststop hooks. Coupling kaniko-push to cluster-admin
operations on a different namespace mixes blast radii: a kaniko
exit code mid-push leaves the registry stuck at 1 replica, and the
poststop hook running on Pod failure is unreliable.

If the workaround proves durable (still needed > 1 round from now),
the right next step is fixing `REGISTRY_HTTP_SECRET` in the
registry chart, not adding init/poststop coupling here.

## Future improvement — in-cluster crictl tag

The kaniko round-trip (build context → kaniko → registry → image
pull on the destination node) is wasteful when the source binary
already lives on a hostPath we control. The destination ultimately
just needs the runtime image referenced by tag in containerd's
content store.

A future variant of this push step could:

1. Build the runtime image once (kaniko or buildah) into a tarball.
2. `crictl tag` the loaded image to `cb-chromium:cr7727-${variant}`
   on each node that runs cb-chromium-smoke / production.
3. Skip the registry round-trip entirely.

Trade-off: tag-distribution becomes a fan-out across nodes
(currently registry pulls fan-in via `imagePullPolicy: Always`).
Worth doing only when the registry stops being a single trusted
distribution point — until then the kaniko push is the cleaner
mental model.

## Cross-references

- `infra/k8s/cb-build/build-job-x264.yaml` — produces the build
  context this Job consumes (Step 9 of cb-build.sh).
- `infra/k8s/cb-build/build-job-vaapi.yaml` — same, vaapi profile.
- `infra/k8s/cb-build/build-job-nvenc.yaml` — same, nvenc profile,
  triform-5.
- `build/cb-build.sh` — the Step 9 staging logic that produces
  `${CB_WORK_ROOT}/artifacts/context/`.
- `infra/k8s/cb-build/README.md` — main cb-build runbook,
  including the `registry-pull` Secret bootstrap.
- BUGS-513 — registry session-affinity / `REGISTRY_HTTP_SECRET`.

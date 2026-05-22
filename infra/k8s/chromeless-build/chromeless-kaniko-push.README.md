# chromeless-kaniko-push.yaml — push the runtime chromeless image after a build

This Job pushes the runtime image `chromeless:<tag>` to
`registry.triform.cloud/chromeless/` using kaniko, reading
the build context staged by `chromeless-build.sh` Step 9 from the chromium-src
hostPath of whichever node ran the variant.

The manifest is a **template** — three fields are envsubst placeholders
(`${CHROMELESS_KANIKO_TAG}`, `${KANIKO_NODE}`, `${KANIKO_VARIANT_LABEL}`). The
**canonical apply path** is the wrapper script:

```bash
./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-7
```

The wrapper reads `IMAGE_TAG` over ssh from the build node's staged context
(written by chromeless-build.sh Step 9), derives the `cr<branch>-<sha>` tag,
substitutes the three placeholders, applies the Job, tails the kaniko logs
into `/tmp/kaniko-push-<tag>.log`, and reports PASS/FAIL.

This Job is **not** part of the kustomize bundle (see `kustomization.yaml`);
the wrapper is the only intended entry point.

## Why a separate manifest

`build-job-{x264,x264-t7,vaapi,nvenc}.yaml` runs `chromeless-build.sh`.
Step 9 of that script stages a kaniko-ready build context to
`${CHROMELESS_WORK_ROOT}/artifacts/context/` — for sw/x264/vaapi on
triform-8 and x264-t7 on triform-7 that's
`/var/lib/longhorn/chromeless-build/chromium-src/artifacts/context/`;
nvenc on triform-5 still uses `/mnt/data/chromeless-build-t5/...`. The
script deliberately does **not** push. The push is split out so:

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

## Why the BLOCKING tag fix (auditable unique tags)

Before this change, the manifest hardcoded `CHROMELESS_KANIKO_TAG=cr7727-sw`.
Every push overwrote the prior `cr7727-sw` tag in the registry, so two
successive builds with different chromeless SHAs would both end up at
the same registry tag. The first build's image was silently destroyed
by the second, and downstream `imagePullPolicy: Always` deployments
could roll a build the operator never approved.

The fix:

1. The manifest tag is now driven by `${CHROMELESS_KANIKO_TAG}` substituted
   at apply time, not hardcoded.
2. The wrapper script derives the tag from the `IMAGE_TAG` file that
   `chromeless-build.sh` Step 9 wrote on the build node. That file is
   `cr${CHROMIUM_BRANCH_NUMBER}-${CHROMELESS_GIT_SHA}` (e.g.
   `cr7727-8367359` for chromeless commit `8367359`).
3. The Job's `metadata.name` is now `chromeless-kaniko-push-<tag>` so two
   different-tag pushes coexist as distinct k8s objects (no delete-and-
   recreate dance, no "Job is immutable" trap).

Net effect: every push is uniquely addressable in the registry AND in
k8s, the IMAGE_TAG file is the single source of truth for the tag, and
silent overwrites are impossible.

## Activation procedure (canonical, via wrapper)

1. **Wait for chromeless-build-<variant> to reach Succeeded.**
   ```bash
   kubectl get -n chromeless-build job chromeless-build-x264-t7 -o jsonpath=
'{.status.conditions[?(@.type=="Complete")].status}{"\n"}'
   # Should print: True
   ```

2. **Confirm the build context is staged on the right node.**
   ```bash
   # x264-t7 (triform-7), x264 / sw / vaapi (triform-8):
   ssh triform-7 ls /var/lib/longhorn/chromeless-build/chromium-src/artifacts/context
   # Expect: Dockerfile  IMAGE_TAG  cloud_browser_worker  launch-chromeless.sh
   #         supervisord.conf  pulse-default.pa  devtools-proxy.sh
   #         streamer-static-server.py  streamer/  lifecycle/

   # Verify the tag file:
   ssh triform-7 cat /var/lib/longhorn/chromeless-build/chromium-src/artifacts/context/IMAGE_TAG
   # Expect: cr7727-<chromeless-sha>
   ```

3. **(If needed) apply the BUGS-513 registry scale-to-1 workaround.**
   See § BUGS-513 below. Skippable on the first push attempt; required
   only if kaniko reports a 5xx on manifest PUT.

4. **Fire the push via the wrapper.**
   ```bash
   ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-7
   # or for the T8 lane:
   ./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-8
   ```
   The wrapper streams kaniko logs to stdout AND `/tmp/kaniko-push-<tag>.log`
   and exits 0 on success / 1 on failure. The Job has
   `ttlSecondsAfterFinished: 86400`, so the pod remains inspectable
   for 24 h after exit.

5. **Verify the tag landed in the registry.**
   ```bash
   TAG=$(ssh triform-7 cat /var/lib/longhorn/chromeless-build/chromium-src/artifacts/context/IMAGE_TAG)
   curl -sf -u "${FORGEJO_USERNAME}:${FORGEJO_PASSWORD}" \
     "https://registry.triform.cloud/v2/chromeless/chromeless/manifests/${TAG}" \
     -H 'Accept: application/vnd.docker.distribution.manifest.v2+json' \
     -o /dev/null -w '%{http_code}\n'
   # Expect: 200
   ```

6. **(If you scaled the registry down) restore replicas** — see
   § BUGS-513 below.

## Manual apply (fallback, without the wrapper)

Useful for diagnostics or when ssh-to-node is unavailable. The operator
provides all three placeholders by hand:

```bash
# Pick the destination tag (read from IMAGE_TAG manually if you can).
export CHROMELESS_KANIKO_TAG=cr7727-8367359   # MUST be unique per build
export KANIKO_NODE=triform-7                  # triform-7 or triform-8 only
export KANIKO_VARIANT_LABEL=x264-t7            # informational label

envsubst '${CHROMELESS_KANIKO_TAG} ${KANIKO_NODE} ${KANIKO_VARIANT_LABEL}' \
  < infra/k8s/chromeless-build/chromeless-kaniko-push.yaml \
  | kubectl apply -f -

kubectl -n chromeless-build logs -f \
  job/chromeless-kaniko-push-${CHROMELESS_KANIKO_TAG}
```

The `envsubst '${...}'` allowlist form is **critical** — without it,
envsubst would also strip `${VARIANT}` / `${CHROMELESS_WORK_ROOT}`
mentions inside the manifest's header comments. The kaniko args use
`$(VAR)` (K8s downward-API) and are never touched by envsubst.

## Variant overrides

| Variant | Node | Pass to wrapper | hostPath path |
|---------|------|------------------|----------------|
| x264-t7 (CV2 M-stack default) | triform-7 | `./chromeless-kaniko-push.sh triform-7` | `/var/lib/longhorn/chromeless-build/chromium-src/artifacts/context` |
| sw / x264 | triform-8 | `./chromeless-kaniko-push.sh triform-8` | `/var/lib/longhorn/chromeless-build/chromium-src/artifacts/context` |
| vaapi | triform-8 | `./chromeless-kaniko-push.sh triform-8 vaapi` | `/var/lib/longhorn/chromeless-build/chromium-src/artifacts/context` |
| nvenc (out of CV2 scope) | triform-5 | `CONTEXT_PATH=/mnt/data/chromeless-build-t5/chromium-src/artifacts/context ./chromeless-kaniko-push.sh triform-5 nvenc` | `/mnt/data/chromeless-build-t5/chromium-src/artifacts/context` |

The wrapper rejects nodes outside `{triform-5, triform-7, triform-8}`
to enforce the CV2 operator T7/T8 hard-constraint at the source. T5
is allowed only when `CONTEXT_PATH` is explicitly set (nvenc lane).

A future improvement (out of scope for this manifest) would lift
the per-variant fields into a kustomize overlay set:
`overlays/{x264,x264-t7,vaapi,nvenc}/`. Today the wrapper + envsubst
combo is simpler than wiring four overlays and never breaks because
each variant push is a discrete operator action.

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

# Apply the kaniko-push Job (via wrapper).
./infra/k8s/chromeless-build/chromeless-kaniko-push.sh triform-7

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
2. `crictl tag` the loaded image to `chromeless:cr7727-<sha>`
   on each node that runs chromeless-smoke / production.
3. Skip the registry round-trip entirely.

Trade-off: tag-distribution becomes a fan-out across nodes
(currently registry pulls fan-in via `imagePullPolicy: Always`).
Worth doing only when the registry stops being a single trusted
distribution point — until then the kaniko push is the cleaner
mental model.

## Cross-references

- `infra/k8s/chromeless-build/build-job-x264-t7.yaml` — T7 build lane
  (CV2 M-stack default), produces the build context this Job consumes.
- `infra/k8s/chromeless-build/build-job-x264.yaml` — T8 sw/x264 lane.
- `infra/k8s/chromeless-build/build-job-vaapi.yaml` — vaapi profile, T8.
- `infra/k8s/chromeless-build/build-job-nvenc.yaml` — nvenc profile, T5
  (out of CV2 M-stack scope).
- `infra/k8s/chromeless-build/chromeless-kaniko-push.sh` — canonical
  apply wrapper (envsubst + log tail + status surface).
- `build/chromeless-build.sh` — Step 9 staging logic that produces
  `${CHROMELESS_WORK_ROOT}/artifacts/context/` and writes `IMAGE_TAG`.
- `infra/k8s/chromeless-build/README.md` — main chromeless-build runbook,
  including the `registry-pull` Secret bootstrap.
- BUGS-513 — registry session-affinity / `REGISTRY_HTTP_SECRET`.

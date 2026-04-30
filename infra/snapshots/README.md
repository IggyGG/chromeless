# CRIU snapshot/restore for fast warm-pool start (Phase 3 stretch)

> **Status:** design + scaffolding (T68). The scripts in this
> directory are operationally correct and shellcheck-clean, but real
> snapshot/restore is Linux-only — the dev box this was authored on is
> macOS without CRIU. The README's "Validation" section is the
> procedure for a Linux box to take this from "design lands" to
> "real perf numbers in the dashboard."

T31 cold-starts a Chromium container in roughly 30-45 s on first boot
(`start_period: 45s` in `infra/compose.yaml`). T50's session
controller assumes the warm pool eats that cost up front so user-side
assignment is fast. CRIU snapshot/restore is the next lever: take
a single snapshot of a *just-booted, pre-tenant* Chromium and use it
as the warm-pool source-of-truth instead of repeating the cold-start
work for every replacement pod.

## Why CRIU specifically

| Option | What it does | Why we don't use it for this |
|---|---|---|
| `docker pause` / `kill -STOP` | SIGSTOP all PIDs, leave them in memory | Doesn't survive host reboot; doesn't help with horizontal scale. |
| Container live-migration (LXC/Podman) | Move a running container between hosts | Same as CRIU under the hood (CRIU is the engine). Adds orchestration we don't need yet. |
| **CRIU `dump` / `restore`** | Page-level memory snapshot to disk; restore in any later boot or on any compatible host | This is what we want. |
| K8s-native warm pool only | Deployment.replicas + readiness gate | What T50 does today. Reaches "warm" but can't go faster than Chromium's own cold-boot cost. |

CRIU's specific superpower for our case: **the snapshot is just a
directory of memory pages and metadata**, totally portable to any
host running a compatible kernel + CRIU, and it can be replayed N
times to launch N restored copies of the same starting state.

## Lifecycle

```
              one-time per image build:                          per-session restore:

   ┌────────────────────┐                            ┌─────────────────────────────┐
   │ docker run         │                            │ create overlayfs            │
   │   cloud-browser-…  │                            │   - lower: snapshot rootfs  │
   │   :dev             │                            │   - upper: per-pod RW       │
   └─────────┬──────────┘                            └──────────────┬──────────────┘
             │ supervisord boots                                    │
             │ Chromium loads about:blank                           ▼
             │ /json/version returns                       ┌────────────────────┐
             ▼                                             │ criu restore       │
   ┌────────────────────┐                                  │   --images-dir <s> │
   │ snapshot.sh        │                                  │   --tcp-established │
   │   pid = $(docker top)                                 └─────────┬──────────┘
   │   criu dump --tree $pid    (~5-10s, ~1-3GB on disk)             │ pid resumes
   │   docker rm $cid                                                ▼
   └─────────┬──────────┘                                  ┌────────────────────┐
             │ snapshot dir at                             │ DevTools: Page.    │
             │ /var/lib/cb-snapshots/<sha>/                │   navigate(streamerURL)
             ▼                                             │   with session_id  │
   ┌────────────────────┐                                  │ ready_at - now < 1s
   │ uploaded to a      │                                  └────────────────────┘
   │ snapshot store     │
   │ (object storage,   │
   │  daemonset cache)  │
   └────────────────────┘
```

The crux: the snapshot is taken *before* any tenant has touched the
container. The user-data-dir is empty, no signaling URL is dialed,
and Chromium is sitting on `about:blank` ready to receive a
`Page.navigate` over DevTools. On restore, the controller's session
agent injects the tenant's session_id + streamer URL via DevTools and
Chromium synthesises the rest of the boot in milliseconds.

## What this saves

(Numbers are projections; real numbers go here after a Linux validator
runs `tests/smoke/snapshot-restore.sh`. CRIU's published benchmarks for
a comparable Chromium-class workload are 800ms-1.5s restore.)

| Phase | Cold start (T31) | Snapshot restore (this task) |
|---|---|---|
| dumb-init + entrypoint.sh + cold-start.sh | 0.5 s | 0 s (skipped) |
| supervisord + Xvfb + PulseAudio | 2-3 s | 0 s (skipped) |
| Chromium first boot + about:blank | 8-12 s | 0 s (skipped) |
| CRIU restore | — | 0.8-1.5 s |
| DevTools `Page.navigate` to streamer URL | 0.5-1 s (on top of cold start) | 0.5-1 s |
| **Total to "user can connect"** | **30-45 s** | **<2 s** |

## Limitations and design constraints

### TCP-established connections are perilous

CRIU supports `--tcp-established`, but it can only restore a connection
if the *peer* believes the connection is still alive. For our case:

- **DevTools listener** (Chromium :9222 on loopback): when we snapshot,
  no peer is connected. The listening socket itself snapshots and
  restores fine; new clients dial after restore.
- **PulseAudio socket**: same — listener, no clients.
- **WebRTC peer connection**: We deliberately snapshot *before* a user
  is connected. There is no peer connection in the snapshot. **If we
  ever try to snapshot mid-session, this is where it falls apart**:
  the user's browser does not know we restored on a different host
  with a different OS-level TCP stack, ICE candidates have
  expired, etc.

**Decision: snapshot only at the pre-tenant state.** A restored pod
is functionally equivalent to a freshly-cold-booted pod that just
happens to start in <2s.

### Filesystem strategy

The snapshot directory is the *memory* state. The *filesystem* state
is separate. We use an overlayfs layout:

```
/var/lib/cb-snapshots/<sha>/
├── images/                ← criu's memory-pages dump
└── rootfs.tar.zst         ← the container's rootfs at snapshot time
```

On restore:

```
overlayfs:
  lower = unpacked rootfs.tar.zst   (read-only, shared across all restores from this snapshot)
  upper = per-pod emptyDir          (read-write, dies with the pod)
```

This means the snapshot's lower layer is **immutable and shared**.
Tenants can never write to it; they only see their own upper. So
even if a tenant's upper layer carries cookies or cache, no other
tenant can see those. Cross-tenant isolation is preserved by the same
contract today's `emptyDir` mounts give us.

### Threat model

#### Snapshots must be *tenant-clean*

A snapshot taken after any tenant has loaded a real page is not safe
to share across tenants — DOM, cache, IndexedDB, Service Worker
state, and form data may all be in memory. **Snapshots are taken at
about:blank, before the streamer page loads.** This is the contract
T50's controller depends on.

If a tenant's first action triggers a snapshot in some future
optimisation (e.g., snapshot-after-warmup-of-a-popular-page), that
snapshot becomes *tenant-namespaced* and must never be served to a
different tenant. The directory layout already supports this:

```
/var/lib/cb-snapshots/
├── shared/<sha>/          ← about:blank snapshots, any-tenant
└── tenant/<tenant_id>/<sha>/   ← post-page snapshots, single-tenant
```

The session controller (T50) consults the BrowserSessionPool spec to
decide which subdirectory to restore from.

#### Snapshots interact subtly with the seccomp profile (T57)

CRIU `dump` and `restore` use syscalls that our T57 seccomp profile
denies (`ptrace`, `process_vm_*`, `kcmp`). That's fine for the
*restored* container — those denials apply to the runtime. But the
*restoring* process (the controller's CRIU helper) needs them. The
restoring process runs *outside* the seccomp profile, on the node
itself, with `CAP_CHECKPOINT_RESTORE` (Linux ≥ 5.9). Document this
in the kubelet runtime config. See `infra/security-hardening.md` for
the seccomp posture and `infra/k8s/cloud-browser-session.yaml` for
the runtime annotation.

#### Snapshot integrity

The snapshot directory is build-time output that gets restored
N times. A tampered snapshot is a privilege-escalation primitive
(it could include malicious memory state). Protect with:

1. **Sign the snapshot tarball** at build time with cosign (or a
   private key in the build pipeline). Verify before restore.
2. **Store on read-only object storage** with bucket-level immutable
   policies.
3. **Restore over a content-addressed path** (`/var/lib/cb-snapshots/<sha>`):
   the sha matches a manifest signed at build time, the restore
   helper refuses to restore from a sha not on the allow-list.

These are deploy-side concerns; the scripts here take a sha as input
and trust the caller (the controller) to have done the verification.

## Validation

The DoD asks for "real perf numbers." That means running on a Linux
host with CRIU installed. Procedure:

1. **Linux box prerequisites:**
   ```
   apt install criu iproute2     # criu and a couple of helpers
   sysctl -w kernel.unprivileged_userns_clone=1
   sysctl -w kernel.yama.ptrace_scope=0
   ```
   Or run as root inside a privileged container; CRIU usually needs
   `CAP_CHECKPOINT_RESTORE` + `CAP_SYS_PTRACE`.
2. **Build the image** (build context = repo root):
   ```
   docker build -t cloud-browser-webrtc:dev -f infra/Dockerfile .
   ```
3. **Take a snapshot:**
   ```
   sudo ./infra/snapshots/snapshot.sh cb-snapshot-blank
   ```
   The script prints the snapshot's content sha and writes to
   `/var/lib/cb-snapshots/<sha>/`.
4. **Restore:**
   ```
   sudo ./infra/snapshots/restore.sh <sha>
   ```
   The script reports `time-to-ready` (the gap between
   `criu restore` exit and DevTools `/json/version` returning 200).
5. **Smoke test:**
   ```
   bash tests/smoke/snapshot-restore.sh
   ```
   Asserts time-to-ready < 2 s.

When you do that on a real Linux runner, paste the resulting numbers
into the table above and replace "(Numbers are projections...)" with
"(Numbers measured on <host spec>, <CRIU version>, <date>)".

## ScrubAndReturn vs RecreatePod (T90)

T71's controller ships two `BrowserSessionPool.spec.recyclePolicy`
options. T68 snapshots interact with each subtly:

| Policy | What happens at session end | Compatible with snapshots? |
|---|---|---|
| `RecreatePod` (default) | Pod is deleted; the pool reconciler creates a fresh one. New pod restores from `cb.io/snapshot-id` if set, else cold-starts. | Yes. Each session gets a fresh process-tree-from-snapshot. Highest isolation. |
| `ScrubAndReturn` | `scrub-pod.sh` wipes user-data-dir + tmp inside the live pod; pod stays alive and goes back to warm. | Yes — but the snapshot only matters at the *original* container boot. Once a pod is in the warm pool, subsequent reuses don't re-restore the snapshot; they just scrub-in-place. |

### Acceptable when

ScrubAndReturn is safe under any of:

1. **Single-tenant pool.** Every session in this pool comes from
   the same trust boundary (one company, one user, internal-only
   deploy). The scrub is hygiene, not a security boundary.
2. **gVisor or Kata + Cloud Hypervisor as the runtime
   (T44-decided).** The kernel-level state that scrub-pod.sh
   doesn't reach (page cache, slab caches, Sentry state) lives
   *inside* the sandbox. The sandbox is the boundary; scrub clears
   userland.
3. **Pre-populated tenant-clean snapshot.** If the pool's pods are
   restored from a `shared/<sha>/` snapshot taken at about:blank
   (the contract earlier in this doc), AND the controller
   supplements scrub-pod.sh with a CRIU re-restore at recycle time
   (Phase 4 work, not implemented today), the pool gets the
   snapshot-clean property without paying the full pod-recreation
   cost.

### NOT acceptable when

ScrubAndReturn is **not** safe when:

- The pool serves **mutually distrustful tenants** under bare runc.
  The `--no-sandbox` Chromium plus a renderer-side compromise can
  leave kernel-state residue scrub-pod.sh has no path to clear.
  Tenant N's compromise can reach tenant N+1 through page cache
  side channels; scrub doesn't touch the kernel.
- The pool's pods are **stateful by design** (they hold
  long-lived resources we'd rather not torch). Phase 1's stateless
  Chromium pods don't fit this; if Phase 4+ adds caching layers
  inside the pod (e.g., a pre-warmed extension store), reconsider.
- The pool's `scrub-pod.sh` exit cannot be **trusted under
  attack**. The controller treats a non-zero exit / missing OK
  marker as failure and falls back to RecreatePod
  (`scrub_failed_fallback_recreate` metric). But a *compromised*
  pod can still print "[scrub] OK" while leaving residue;
  ScrubAndReturn fundamentally trusts the pod's userland.

The pool examples in
[`infra/k8s/browsersessionpool-examples.yaml`](../k8s/browsersessionpool-examples.yaml)
ship both:

- `warm-pool-shared` — `ScrubAndReturn`, single-trust-boundary use.
- `warm-pool-strict` — `RecreatePod`, untrusted-multi-tenant use.

Pick per pool, not per session — the pool is the unit of trust.

## Coordination with other tasks

- **T31 (lifecycle):** restore replaces cold-start.sh's role. The
  entrypoint chain isn't run on restored pods; the snapshot already
  encodes everything cold-start would have done. New tenants still
  need a fresh SESSION_ID, SIGNALING_URL, etc. — those are injected
  via DevTools `Runtime.evaluate` on `window.__CBWRTC_CONFIG__`
  (or, for the streamer, a `Page.navigate` to a fresh URL).
- **T38 (metrics):** the metrics sidecar's prev-state accumulators
  (bytes-sent deltas, framesDropped baseline) snapshot too. Restored
  metrics are continuous from snapshot time; this is acceptable for
  rate(...) but not for "time since restore" panels. Add
  `cb_snapshot_restored_at_seconds` (gauge with restore epoch) so
  Grafana panels can `time() - cb_snapshot_restored_at_seconds`. Not
  in scope for T68; tracked as follow-up.
- **T44 (sandbox):** gVisor's Sentry can't be CRIU-snapshotted today
  (the Sentry runs in user-space, opaque to host CRIU). If we adopt
  gVisor, snapshot/restore happens *inside* the Sentry's own
  checkpoint mechanism, which is a different code path. Kata + Cloud
  Hypervisor is similar — the VM-level snapshot is the unit, and
  CRIU at the container level is the wrong tool. **CRIU
  snapshot/restore as designed here only applies under runc + the
  T57 seccomp profile.** When T44 picks a different runtime, this
  task gets re-evaluated.
- **T50 (controller):** the controller chooses
  `runc + restore-from-snapshot` vs `cold-start fresh pod` based on
  the pod's `cb.io/snapshot-id` annotation. Empty annotation =
  cold-start path. See `infra/k8s/cloud-browser-session.yaml`.

## Files in this directory

| File | Purpose |
|---|---|
| `snapshot.sh` | One-shot: boot, wait, dump, tear down. |
| `restore.sh` | One-shot: restore, inject session config, report time. |
| `README.md` | This file. |
| (smoke) `tests/smoke/snapshot-restore.sh` | End-to-end timing assertion. |

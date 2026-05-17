# Container security hardening

T57: Phase 1+ minimum-viable hardening of the chromeless
container. This is what we ship today; T44's Phase 3 sandbox decision
(gVisor / Kata + Cloud Hypervisor / runc + AppArmor) builds *on top*
of this rather than replacing it.

The threat model is the same as
[`docs/research/sandbox-isolation.md`](../docs/research/sandbox-isolation.md):
a malicious page → renderer compromise → container escape attempt.
Hardening here narrows the renderer's syscall surface and removes
ambient privileges so a successful renderer compromise is
maximally contained without yet relying on a microVM.

## What changed in T57

| Layer | Before T57 | After T57 |
|---|---|---|
| Default user | root, with chromium dropping to cbuser via supervisord `user=cbuser` | cbuser (uid 1000) for the whole entrypoint chain — supervisord runs as cbuser too |
| Root filesystem | writeable | read-only in K8s (`readOnlyRootFilesystem: true`); writeable paths are explicit emptyDir volumes |
| Capabilities | Docker default set | `drop: ["ALL"]`, no `add` |
| Privilege escalation | implicit-allowed | `allowPrivilegeEscalation: false` |
| Seccomp | Docker's default profile (only) | `Localhost: chromeless.json` — DENY-list of dangerous syscalls beyond Docker's default |
| Pod-level | (none) | `runAsNonRoot: true`, `runAsUser/Group: 1000`, `fsGroup: 1000` |
| supervisord | ran as root, dropped privs per program | runs as cbuser; programs inherit |

## Per-line rationale

### `infra/Dockerfile`

- **`USER cbuser` at the end.** With this, `docker run chromeless:dev`
  starts a container whose PID 1 is cbuser. Combined with K8s's
  `runAsNonRoot: true`, this is the difference between "we attempt to
  drop to a user" and "we cannot become root, ever, in this image."
- **`mkdir -p /run/supervisor /run/chromeless-session /var/log/supervisor`
  + `chown -R cbuser:cbuser ...`** at build time. cbuser owns its
  state dirs in the image fs. In K8s the emptyDir overlays wipe these
  paths, so `cold-start.sh` re-creates them at startup.
- **`COPY infra/seccomp/chromeless.json /etc/chromeless-seccomp.json`.** The
  baked-in copy lets `docker run --security-opt seccomp=/etc/chromeless-seccomp.json`
  work without an external file.

### `infra/supervisord.conf`

- **No `user=root`** in `[supervisord]`. Used to be there because
  supervisord lived as root and dropped privs per program; now it
  starts as cbuser and there's no privilege boundary inside.
- **`pidfile=/run/supervisor/supervisord.pid` + `file=/run/supervisor/supervisor.sock`.**
  cbuser-owned subdir avoids needing root to write at /run.
- **No `user=cbuser` in any `[program:*]`.** Inherited from
  supervisord; explicit lines were a tripwire that would force a
  setuid attempt under K8s.

### `infra/lifecycle/cold-start.sh`

- **Step 0 mkdir block.** Re-creates the writable subtree under /run,
  /var/log/supervisor, /home/cbuser when K8s emptyDir mounts arrive
  empty. Idempotent — no-op when the image-fs paths are already
  there (plain `docker run`).
- **`chown` removed** from the user-data-dir cleanup. cold-start
  runs as cbuser; can't chown what it doesn't own. Doesn't need to:
  cbuser owns the dir from the Dockerfile chown.

### `infra/k8s/cloud-browser-session.yaml`

- **Pod `securityContext`**: runAsNonRoot, runAsUser/Group/fsGroup,
  seccomp localhost profile, fsGroupChangePolicy=OnRootMismatch
  (cheaper recursive chown than `Always` for emptyDir).
- **Per-container `securityContext`**: `readOnlyRootFilesystem: true`
  on every container including chromeless, `drop: ALL` capabilities,
  `allowPrivilegeEscalation: false`. Defense in depth — even if pod
  defaults regress, individual containers stay locked.
- **Volumes**:
  - `dshm` (medium=Memory, 1Gi): Chromium's /dev/shm.
  - `run` (medium=Memory, 64Mi): tmpfs for /run; supervisord +
    pulse + chromeless-session live here.
  - `supervisor-log` (64Mi): /var/log/supervisor.
  - `home` (512Mi): /home/cbuser; chromium profile + caches.
  - `tmp` (64Mi): /tmp.
  - `x11-socket`: /tmp/.X11-unix shared with the clipboard-bridge
    sidecar. M7 R4: cursor-watcher retired; native cursor egress
    (M5) reads cursor state inside the browser process.

### `infra/seccomp/chromeless.json`

- **`defaultAction: SCMP_ACT_ALLOW`.** Chromium has a wide and
  evolving syscall surface. ALLOW-listing is brittle; the renderer
  hits an unmapped syscall, gets EPERM, and crashes opaque. We
  DENY-list instead — block syscalls that Chromium has *no* legitimate
  use for, and let everything else through.
- The denied set covers:
  - **Module ops** — `init_module`, `delete_module`, `finit_module`,
    `create_module`, `query_module`, `get_kernel_syms`. Kernel
    rooting paths.
  - **Hardware / firmware** — `iopl`, `ioperm`, `kexec_load`,
    `kexec_file_load`, `vm86`, `uselib`, `ustat`, `sysfs`, `_sysctl`.
  - **Mount / namespace** — `mount`, `umount`, `umount2`,
    `pivot_root`, `chroot`, `unshare`, `setns`, `name_to_handle_at`,
    `open_by_handle_at`, `fanotify_init`. Namespace re-negotiation
    is a sandbox-escape primitive.
  - **Time / hostname** — `clock_settime`, `settimeofday`, `stime`,
    `adjtimex`, `clock_adjtime`, `setdomainname`, `sethostname`.
  - **Reboot, BPF, userfaultfd, swap, quotactl, acct, nfsservctl** —
    high-privilege or CVE-prone.
  - **NUMA memory policy** — `mbind`, `set_mempolicy`,
    `migrate_pages`, `move_pages`. Powerful, container-irrelevant.
  - **`personality`, `perf_event_open`** — historically used in
    kernel-exploit chains.
  - **`ptrace`, `process_vm_readv`, `process_vm_writev`, `kcmp`** —
    inter-process inspection. Especially dangerous because we run
    `shareProcessNamespace: true` so any container could use ptrace
    to attack any other container without this profile.

  Each block has an inline `_rationale` field so the JSON is
  self-documenting.

## Trade-offs we accepted

- **Chromium runs with `--no-sandbox`.** Chromium's own seccomp-bpf
  layer requires CAP_SYS_ADMIN at startup to install and we drop
  ALL caps. Until T44's Phase 3 sandbox decision is made, the
  OS-level seccomp profile here is the only seccomp layer between
  the renderer and the host. Document on the `--no-sandbox` line in
  `infra/launch-chromeless.sh` notes this.
- **`shareProcessNamespace: true`.** Required so chromeless-metrics-sidecar
  (T38) can read /proc to aggregate Chromium CPU/RSS. Mitigated by
  the seccomp profile blocking ptrace + process_vm_*. Phase 3
  alternatives: (1) move metrics scraping to a host-side
  Prometheus that hits `/metrics` directly without /proc reads, (2)
  build a tiny eBPF cgroup-stats reader (deny bpf in seccomp would
  need narrowing).

## K8s seccomp install

The seccomp profile lives at `infra/seccomp/chromeless.json` in this
repo. K8s does NOT read it from the container image. Each kubelet
node must have the profile on disk at:

```
/var/lib/kubelet/seccomp/chromeless.json
```

before a pod with `seccompProfile.localhostProfile: chromeless.json`
can start. The recommended pattern:

1. Bake the profile into your node image (Packer / Image Builder /
   GKE node image extension).
2. Or ship via a privileged DaemonSet that drops the file into
   `/var/lib/kubelet/seccomp/` on every node and exits. The
   [security-profiles-operator](https://github.com/kubernetes-sigs/security-profiles-operator)
   project automates this — recommended for any production
   deployment.

## Validation

- `tests/smoke/security-posture.sh` (T57) asserts the image at run
  time:
  - `id -u` returns a non-zero number (non-root).
  - `touch /test` fails with EROFS (read-only fs).
  - `getcap /usr/bin/chromium` is empty (no setuid /
    file-capabilities elevation paths).
- `kubectl --dry-run=client apply -k infra/k8s/` validates the
  manifest changes against the K8s API.

## Next steps (Phase 3+)

1. **T44 sandbox decision.** Pick gVisor or Kata + Cloud
   Hypervisor; stop relying on `--no-sandbox`.
2. **AppArmor profile** alongside seccomp. Belt-and-braces.
3. **NetworkPolicy** per session; egress-only NAT to public
   internet, drop RFC1918 + cloud metadata.
4. **PodDisruptionBudget** to keep tenant capacity during node
   maintenance.
5. **Re-evaluate `shareProcessNamespace`** once we've moved
   /proc-based metrics off the sidecar (see "Trade-offs"
   above).

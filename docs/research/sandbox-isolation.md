# Sandbox isolation primer (Phase 3 prep)

> **Status:** research, not a decision. We do not implement sandboxing
> in v1. This doc lays the fact base so the Phase 3 isolation task can
> move quickly. Re-read [`PROJECT_BRIEF.md`](../../PROJECT_BRIEF.md) §
> Phase 3 and § Risks alongside this.

The v1 image runs Chromium with `--no-sandbox` (see
`infra/Dockerfile`). That is a v1 expedient: the image works on any
host kernel without `CAP_SYS_ADMIN` gymnastics, but it means a
compromised tab on a shared host can reach every other tenant's
session. Phase 3 is when we replace `--no-sandbox` plus default
container isolation with something that actually defends against an
untrusted page.

The candidates are gVisor, Firecracker (via firecracker-containerd),
Kata Containers, and "stay on `runc` and lean on AppArmor + Seccomp."
This primer compares them on the axes that matter for a cloud browser
service: Chromium compatibility, GPU passthrough, performance overhead
on graphics-heavy workloads, operational complexity, and Kubernetes
integration maturity.

## 1. Threat model

A public cloud-browser service runs untrusted JavaScript inside a
real Chromium binary on shared infrastructure. The attacks we have to
defend against, in order of severity:

1. **Tab → host kernel escape.** A renderer-side V8 or Blink CVE
   compromises Chromium's renderer; the Chromium sandbox holds (or it
   doesn't); the attacker gains code execution at the container's
   namespace boundary; their next move is a kernel exploit (e.g.
   `keyctl`, `io_uring`, `eBPF`, `setuid` net subsystem) for full
   host compromise. This is what `--no-sandbox` actively invites: we
   skip Chromium's own seccomp-bpf layer and rely entirely on the
   container runtime's protections.
2. **Cross-tenant data leakage via shared kernel state.** Side-channel
   reads of another tenant's memory via `procfs`, `/dev/mem`, shared
   slab caches, or microarchitectural attacks (Spectre/MDS) that
   require co-tenancy on the same physical core.
3. **Resource exhaustion / noisy neighbour.** A malicious page
   pegging CPU, RAM, GPU, or PIDs to make every other tenant's
   session unusable.
4. **Network egress to internal infrastructure.** A compromised
   container reaching cloud metadata services (`169.254.169.254`),
   internal databases, control-plane APIs, or other tenants' sidecars
   on the same node.
5. **Persistence on restart.** A tenant writing to a shared host
   path and surviving the container's lifecycle. Our T31 cold-start
   already wipes Chromium's user-data-dir; we need that property to
   hold under sandbox compromise too.

Any sandbox we pick must cut off (1) and (2). (3), (4), (5) are
orthogonal — solved with cgroups, network namespaces + egress NAT,
and disk quota / tmpfs respectively, regardless of which sandbox is
in use.

## 2. gVisor (`runsc`)

**How it works.** gVisor is a user-space kernel (the "Sentry") that
sits between the workload and the host. Every syscall the workload
makes is intercepted; the Sentry implements the syscall in user
space using a small, audited set of host syscalls. Filesystem
operations are forwarded to a separate **Gofer** process over a
private socket — the workload never holds a host file descriptor.
Two Sentry execution platforms are supported in production: KVM (the
Sentry runs as a tiny KVM guest) and `systrap` (ptrace-style
interception in user space). KVM is faster on hardware that supports
nested virt; systrap is the portable fallback.

**Chromium compatibility.** Chromium's renderer issues a wide
syscall surface — clone, futex, ioctl, perf_event_open, io_uring,
keyctl. gVisor has documented gaps around `io_uring` (the Sentry
doesn't expose all opcodes), perf counters, and some `ioctl()`
families. In practice Chromium runs on gVisor — Google itself uses
gVisor to run user code in Cloud Run and App Engine — but the
specific `--no-sandbox` flag is still required because Chromium's
own seccomp-bpf layer doesn't compose well with the Sentry. **You
trade Chromium's per-process sandbox for gVisor's whole-container
sandbox.** That's a defensible swap if the gVisor sandbox itself
holds; our threat model already assumed Chromium's sandbox might
fail, so the net security position arguably improves.

**Performance.** gVisor publishes measurements in the order of
2-3× slower for syscall-heavy workloads vs `runc` on the systrap
platform; KVM is closer to 1.2-1.5×. CPU-bound user code (which
includes most Chromium work after startup — V8 JIT, Blink layout,
software encoder loops) is much closer to native because syscalls
are infrequent inside the hot loop. Software encoders (libvpx VP9,
x264) should run within 5-15% of native; we should measure.

**GPU passthrough.** Per the [official gVisor GPU
docs](https://gvisor.dev/docs/user_guide/gpu/), gVisor supports
**only NVIDIA GPUs**, only on Turing/Ampere/Ada/Hopper architectures
(T4, A100, A10G, L4, H100), only with the open-source NVIDIA driver,
only with strict driver-version matching, and only via the
`--nvproxy` flag. MIG (multi-instance GPU), DRM, and modeset are not
supported. Crucially, gVisor's own docs note that **the security
posture against NVIDIA driver vulnerabilities is much weaker** than
against Linux kernel CVEs because nvproxy passes ioctls largely
untouched. For our use case (software encode in v1, optional NVENC
in Phase 4), this is acceptable: we're not betting the Phase-3
isolation story on the GPU sandbox holding. AMD and Intel GPUs are
unsupported.

**Operations.** Replace the runtime in containerd's config:

```
[plugins."io.containerd.grpc.v1.cri".containerd.runtimes.runsc]
  runtime_type = "io.containerd.runsc.v1"
```

Then attach a Kubernetes RuntimeClass and set `runtimeClassName:
gvisor` on the cloud-browser pod template. GKE has first-party gVisor
support; outside GKE the K8s device-plugin story for GPU is still
gappy per the gVisor docs ("does not have integration support for
`k8s-device-plugin` yet").

## 3. Firecracker

**How it works.** Firecracker is a lightweight Rust VMM built on
KVM. It runs each microVM as a single process with a tiny device
model — virtio block, virtio net (TAP), virtio vsock, RTC, an entropy
source, and pmem. No SCSI, no PCI emulation, no graphics, no USB.
Cold-start is famously fast (Firecracker's own claim is <125 ms for a
microVM, hundreds of milliseconds end-to-end via firecracker-
containerd). AWS Lambda and Fly.io both run Firecracker per-tenant.

**Chromium compatibility.** Firecracker runs a real guest kernel, so
Chromium's full syscall surface works — better compatibility than
gVisor by definition. The catch is the device model: no PCI bus, no
DRM, no audio passthrough. PulseAudio inside the guest is fine
(audio routes through cb_audio.monitor as today), but anything that
expects a real GPU device or hardware-accelerated graphics will not
find one.

**Performance.** KVM overhead on modern Intel/AMD with VT-x/AMD-V is
single-digit-percent for CPU and memory once the microVM is warm. I/O
goes via virtio with the usual hypercall round-trips. For our
workload (heavy in-process compute, modest disk IO, network only via
WebRTC's UDP), Firecracker should be within ~5-10% of native.

**GPU passthrough.** **Not supported in any practical sense.**
Firecracker's design rejects PCI emulation; VFIO passthrough is on
the project's "explicit non-goals" list. If we want hardware NVENC in
Phase 4, Firecracker is out unless we accept a heterogeneous fleet
(some hosts run Firecracker for security-critical multi-tenant; some
hosts run with GPU and weaker isolation).

**Operations.** Bare Firecracker has no orchestration. The accepted
production stacks are:

- **firecracker-containerd**: official integration, exposes Firecracker
  through containerd's runtime interface. Reasonable but small
  ecosystem.
- **Kata Containers** with the Firecracker hypervisor backend (see § 4).
  This is what most people use when they say "Firecracker for K8s."
- **Fly.io's Flintlock + flyd**: Firecracker control plane Fly.io
  open-sourced. Production-tested, but the surrounding ecosystem is
  Fly-specific.
- **Ignite (Weaveworks)**: deprecated, archived in 2023. Don't.

## 4. Kata Containers

**How it works.** Kata is the OCI-runtime layer: it accepts a normal
container spec and runs each container as a tiny VM. Hypervisor
backends are pluggable — QEMU (default, mature, broad device
support), Firecracker (lightweight, no GPU, no virtio-fs), and Cloud
Hypervisor (newer, Rust, supports VFIO). Kata 3.x ships a runtime
shim that integrates into containerd via the standard runtime-class
mechanism, the same way `runsc` does for gVisor. K8s integration is
mature: you set `runtimeClassName: kata-qemu` or `kata-fc` or
`kata-clh` per pod.

**Why Kata over bare Firecracker.** You almost always want Kata in
front of Firecracker. Bare Firecracker requires you to build the
guest kernel, the rootfs, the network plumbing, and the orchestration
glue. Kata does all of that with the same OCI image you already
build, and lets you switch hypervisor backends with a config flag.

**GPU passthrough.** Available with QEMU and Cloud Hypervisor
backends via VFIO; not available with the Firecracker backend. So if
Phase 4 wants NVENC: Kata + Cloud Hypervisor (or Kata + QEMU) is the
combination that gets us both microVM isolation **and** real GPU
access. The NVIDIA Container Toolkit + Kata + Cloud Hypervisor is a
documented configuration.

**Performance.** Roughly equivalent to bare Firecracker for the
Firecracker backend, plus roughly 50-200 MB extra RAM per microVM for
the QEMU/CLH device model. Cold-start is slower than Firecracker
alone (full Kata bring-up is ~1-2 s on QEMU, ~500 ms on Firecracker).

**Operational complexity.** This is the highest of the options.
Running Kata in production means kernel image and rootfs management,
hypervisor-specific tuning, and an ecosystem of Kata-only debugging
tools (`kata-runtime`, `kata-collect-data`). Worth it if isolation
matters more than ops simplicity.

## 5. Network isolation

Independent of which runtime sandbox we pick, every session pod
should land in its own network namespace with the following:

- **Egress-only NAT** to a dedicated IP block. Drop all RFC1918
  destinations and the cloud metadata IPs (`169.254.169.254`,
  `fd00:ec2::254`).
- **DNS via a per-tenant resolver** (split-horizon) that can't see
  internal service records.
- **No cross-pod traffic** within the same node — enforced by
  Kubernetes NetworkPolicy or by Cilium/Calico equivalent.
- **Egress rate limiting** via tc/htb so a malicious tenant can't
  DDoS from inside.

This is the same pattern Fly.io and Cloudflare Workers use; the
sandbox choice is orthogonal to network policy.

## 6. GPU sharing without trust (Phase 4)

v1 is software encode, so this matters only for Phase 4 (NVENC,
VAAPI). For untrusted GPU sharing on NVIDIA:

- **MIG (Multi-Instance GPU)**: hardware-partitioned slices on
  Ampere+ datacenter cards. Strong isolation; no oversubscription;
  the slice count is fixed by the SKU. Not available on consumer
  cards.
- **MPS (Multi-Process Service)**: software multiplexing on a single
  GPU context. Better throughput but **not a security boundary** —
  cuda kernels share a context. Wrong fit for untrusted tenants.
- **Time-sliced sharing** (NVIDIA k8s device plugin): the device
  plugin advertises one GPU as N "slots"; the driver multiplexes.
  Same caveat as MPS: throughput, not isolation.

For our service: MIG-on-A100/A10G/L40S is the only option that gives
real isolation. If MIG isn't available we run one tenant per GPU,
which scales linearly in cost.

## 7. Recommendation matrix

Score 1 (worst) to 5 (best) per criterion. Weights are subjective
but are what I'd advocate for if asked to pick today.

| | Chromium compat | GPU support | Perf overhead | Ops complexity | K8s integration | **Weighted** |
|---|---|---|---|---|---|---|
| weight | 5 | 3 | 4 | 3 | 4 | |
| **runc + AppArmor + Seccomp** | 5 (native) | 5 (whatever the host has) | 5 | 5 (zero new infra) | 5 | **88** |
| **gVisor (`runsc`)** | 4 (with `--no-sandbox`; small syscall gaps) | 3 (NVIDIA only, datacenter SKUs, fragile driver matching) | 3 (KVM) / 2 (systrap) | 3 (containerd config + RuntimeClass) | 4 (first-party on GKE; gappy elsewhere) | **65** |
| **Kata (Firecracker backend)** | 5 (real kernel) | 1 (no VFIO with FC) | 4 | 2 (Kata ops + per-microVM kernel/rootfs) | 4 (RuntimeClass) | **65** |
| **Kata (Cloud Hypervisor backend)** | 5 | 4 (VFIO works; NVIDIA toolkit integrates) | 4 | 2 | 4 | **77** |
| **Bare Firecracker (firecracker-containerd)** | 5 | 1 | 4 | 1 (small ecosystem) | 2 | **53** |

**runc + AppArmor + Seccomp scores highest because it gives us none
of what the threat model needs.** That's the point of putting it in
the matrix: it shows what we lose against a single-tenant threat
model that we tolerate today, so the eventual Phase 3 decision is
explicit, not defaulted.

The two real candidates are **gVisor** (operationally lighter,
weaker against kernel-level GPU CVEs, GKE-first ergonomics) and
**Kata + Cloud Hypervisor** (stronger isolation by virtue of being
a real VM, GPU-friendly, more ops surface).

## 8. Decision deferred to Phase 3

What we need to know before we can choose:

1. **Real measured overhead** running our exact pipeline (Chromium +
   Xvfb + PulseAudio + libwebrtc + software encoder) under each
   candidate. Glass-to-glass latency under sandbox should be within
   one bucket of latency under runc (i.e. <125 ms on LAN if our
   v1 LAN target is 100 ms).
2. **Concurrent-session ceiling per host** under each candidate.
   Latency under load, not just at idle.
3. **Crash blast radius**: kill the renderer, kill the streamer
   page, kill the gVisor Sentry, kill the Kata VM — does the host
   keep serving other tenants?
4. **GPU plan for Phase 4** as constrained by the sandbox.
   gVisor + nvproxy (and accept the weaker NVIDIA-CVE story) vs Kata
   + Cloud Hypervisor + VFIO (and accept the ops complexity).
5. **Whether GKE is the deploy target.** GKE pulls toward gVisor
   (first-party, easy RuntimeClass). Self-managed K8s pulls toward
   Kata (better cross-cloud parity).

The prototype task is small: build one cloud-browser-webrtc image
that runs under each candidate, run the latency harness against it,
and publish the numbers next to the v1 success criteria. That's the
ticket I'd file at the top of Phase 3.

## References

- gVisor security model: <https://gvisor.dev/docs/architecture_guide/security/>
- gVisor GPU support: <https://gvisor.dev/docs/user_guide/gpu/>
- Firecracker on GitHub: <https://github.com/firecracker-microvm/firecracker>
- Kata Containers: <https://katacontainers.io/learn/>
- Fly.io Flintlock: <https://github.com/weaveworks-liquidmetal/flintlock>
- NVIDIA MIG user guide:
  <https://docs.nvidia.com/datacenter/tesla/mig-user-guide/>

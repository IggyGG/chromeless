# GPU passthrough integration design

**Status:** Phase 2/3/4 prep — design only.
**Owner:** chromium-dev.
**Cross-references:**
`docs/research/sandbox-isolation.md` (T44 — sandbox primer; the
threat-model sibling of this doc),
`capture/encoder/nvenc_encoder.h` + `docs/internal/nvenc-tuning-rationale.md`
(T63),
`capture/encoder/vaapi_encoder.h` + `docs/internal/vaapi-tuning-rationale.md`
(T70),
`docs/research/rendering-matrix.md` (T91),
`docs/research/av1-encoders.md` (T43 — cloud-GPU coverage matrix).

The encoder factory (T19/T63/T70/T75/T83), the rendering pipeline
(T91), and the multi-tenant sandboxing story (T44) all converge on
the same plumbing: **per-session GPU access that is both functional
and isolation-safe**. This doc reconciles the four constraints — what
needs the GPU, what isolation we want, what hardware can deliver
each, and what runtime config wires them together.

## 1. What needs GPU access (and what doesn't)

| Component                                          | Needs GPU? | Path                                                             |
|----------------------------------------------------|------------|-------------------------------------------------------------------|
| NVENC encoder (T63)                                | yes        | `/dev/nvidia*` + libcuda + libnvidia-encode; one CUDA context per session. |
| VAAPI encoder (T70)                                | yes        | `/dev/dri/renderD128` + libva-drm + iHD/radeonsi userspace driver. |
| WebGL/WebGPU page rendering (T91)                  | yes        | Chromium GPU process; ANGLE → Vulkan/Metal/GL on real GPU; SwiftShader otherwise. |
| HW video decode for `<video>` elements             | yes        | Chrome's media pipeline picks VA-API / NVDEC at runtime.           |
| Encoder factory **SW** wrappers (VP9 T35, x264 T36, SVT-AV1 T75) | no         | Pure CPU. Phase 1 v1 ships these only.                            |
| Capture path (T15 getDisplayMedia / T55 FrameSink) | no*        | * Capture itself is CPU; the *frames being captured* benefit from a working GPU compositor (else the page-side WebGL shows SwiftShader artefacts the encoder then ships). |
| Signaling / input bridge / metrics                 | no         | None.                                                              |

The four "yes" rows share the same userspace surface — `/dev/nvidia*`
or `/dev/dri/renderD128` — so any one passthrough mechanism that
unlocks one of them unlocks all of them on the same GPU.

## 2. NVIDIA passthrough options

### nvidia-container-runtime + runc (the baseline)

`nvidia-container-runtime` is the OCI hook that injects
`/dev/nvidia0`, `/dev/nvidiactl`, `libcuda.so`, etc. into a runc
container at start. It is the **default and best-tested** path on
both Docker and Kubernetes (via the NVIDIA k8s-device-plugin).

- **Isolation:** none beyond Linux namespaces. Kernel CVEs reach
  the host directly; co-tenant containers share the GPU's MMU
  (a CUDA context in container A can in principle observe certain
  GPU memory regions touched by container B without explicit
  fencing).
- **Compatibility:** universal across NVIDIA generations.
- **Operational shape:** drop-in.
- **Verdict:** the right pick for **single-tenant-per-node**
  deployments. Phase 2's first-cloud-GPU rollout uses this with a
  Pod-anti-affinity rule so two tenant sessions never share a node.

### NVIDIA MIG (Multi-Instance GPU)

A100 / H100 / L40 / B200 only — Ampere and newer datacenter parts.
The driver carves the physical GPU into up to **7 isolated GPU
instances** (each with its own SMs, L2 cache slice, and memory
partition). Each instance appears to userspace as a separate UUID
under `/dev/nvidia*`; the K8s device plugin advertises them as
distinct resources.

- **Isolation:** PCIe-firmware-enforced. Cross-instance memory
  access is blocked at the GPU level — a real isolation boundary,
  not a software one.
- **Compatibility:** datacenter cards only. Consumer / mobile
  parts (T4, A10, RTX 40-series in workstations) cannot MIG.
- **Operational shape:** profiles set at boot via
  `nvidia-smi mig`; can't reprofile a GPU that has live workloads.
- **Verdict:** the right pick for **multi-tenant on MIG-capable
  hardware**. One tenant per MIG slice; the K8s device plugin
  hands out slices like ordinary GPUs.

### NVIDIA MPS (Multi-Process Service)

CUDA's user-space shared-context layer. Multiple processes share
one GPU context; throughput-optimised (no context switches).

- **Isolation: none.** Sibling processes are in the same memory
  protection domain. Single biggest red flag from a multi-tenant
  threat model.
- **Verdict: do not use for cross-tenant.** Useful only when every
  process is yours — not our case.

### gVisor `nvproxy`

gVisor's NVIDIA-userspace-driver shim. With the `--nvproxy` flag,
gVisor's Sentry intercepts `nvidia.ko` ioctls and forwards them to
the host; the gVisor sandbox keeps every other syscall in user
space.

Key facts from gVisor's [GPU docs](https://gvisor.dev/docs/user_guide/gpu/),
already captured in T44:

- Compatibility matrix is narrow: T4 / A10G / A100 / H100 / L4
  documented; consumer cards and AMD/Intel are not supported.
- **`MIG`, `DRM`, and `modeset` are explicitly NOT supported by
  nvproxy.** This is the single most important constraint for our
  story below.
- gVisor's own docs note that the security boundary against
  *NVIDIA driver* CVEs is materially weaker than its boundary
  against Linux kernel CVEs, because nvproxy passes ioctls largely
  unmodified.
- Strict driver-version pinning — nvproxy ships compat shims keyed
  to specific NVIDIA driver releases; mixing produces opaque
  ioctl errors.

- **Verdict:** valuable as the **kernel-syscall boundary** when
  you've *already* solved GPU-side isolation by some other means
  (e.g. each tenant gets a dedicated GPU or MIG slice). gVisor
  nvproxy alone is not a multi-tenant GPU isolation story — and
  per the docs above, it explicitly cannot ride on top of MIG, so
  the task brief's "MIG + gVisor nvproxy" combination is **not
  supported as of early 2026**. See §3 for what we recommend
  instead.

### Time-sliced sharing (driver-level)

Recent NVIDIA drivers expose time-sliced GPU sharing via the K8s
device plugin (`replicas: N` per device). All tenants share one
context, time-multiplexed.

- **Isolation:** same as MPS — none.
- **Verdict:** noted for completeness; **do not use** for
  cross-tenant.

### Kata + GPU PCI passthrough (VFIO)

Kata Containers run each container in a dedicated lightweight VM.
With VFIO PCI passthrough, the entire physical GPU is bound to the
guest — full isolation at the hardware level.

- **Isolation:** strongest — the physical PCIe device disappears
  from the host while assigned to a guest.
- **Compatibility:** any GPU; requires VT-d / IOMMU on the host
  and a 1:1 GPU:tenant ratio (or multiple GPUs per node).
- **Operational shape:** container-runtime swap (`runtime: kata`),
  device plugin to handle the VFIO bind/unbind.
- **Verdict:** the right pick for **multi-tenant on non-MIG
  hardware** when isolation is non-negotiable. Heavyweight (one
  GPU per tenant; no sharing).

### Confidential Compute paths (briefly)

NVIDIA Hopper introduced CC-on confidential MIG slices. We do not
ship customer data through the GPU in any phase before Phase 4, so
the additional attestation surface is overkill for our use case.
Re-evaluate when we have customers who require it.

## 3. Recommendation matrix

Given the gVisor-cannot-ride-MIG constraint surfaced in §2, the
practical matrix is:

| Deployment shape                            | GPU runtime stack                                      | Why                                                                |
|---------------------------------------------|--------------------------------------------------------|---------------------------------------------------------------------|
| **Phase 1 — no GPU**                        | n/a; SW encode + SwiftShader.                          | We're here today. WebGL works degraded, WebGPU broken (per T91).    |
| **Phase 2 — single-tenant per node**        | nvidia-container-runtime + runc + Pod-anti-affinity.   | Simplest. No tenant collocation, so Linux-namespace isolation is enough. |
| **Phase 3 — multi-tenant, MIG hardware**    | NVIDIA MIG + nvidia-container-runtime + runc + AppArmor + seccomp. (gVisor NOT in this row, contra task brief — nvproxy doesn't support MIG.) | MIG provides the GPU isolation boundary; AppArmor/seccomp the kernel-syscall boundary. |
| **Phase 3 — multi-tenant, non-MIG hardware**| Kata + VFIO PCI passthrough; one GPU per tenant.       | Heavyweight but the only path that works without MIG.               |
| **Phase 3 — best-effort, low cost**         | nvidia-container-runtime + runc + per-node single-tenant scheduling (same as Phase 2 but on a multi-tenant cluster). | Acceptable for friendly-multi-tenant — i.e. customers who trust each other, or where a node compromise has bounded impact. |
| **Phase 3 — gVisor "additional defence in depth"** | nvidia-container-runtime + gVisor nvproxy on top of one of the above. | Adds the kernel-syscall sandbox; useful when the tenant runs untrusted code beyond Chromium itself. Costs ~5-15% throughput per gVisor's published numbers (T44 §2). Cannot combine with MIG. |

The task brief's explicit "MIG + gVisor nvproxy" cell is not
realisable today. We replaced it with **MIG + AppArmor/seccomp**:
MIG handles GPU isolation, AppArmor + seccomp handle kernel-syscall
isolation. If a future gVisor release adds MIG support, the cell
upgrades trivially.

## 4. Implementation requirements

Whichever cell from §3 we pick, the wiring touches the same K8s /
runtime surfaces:

1. **GPU node pool labels** — `nvidia.com/gpu.product` (e.g.
   `NVIDIA-L4`), `nvidia.com/gpu.memory`, and our own
   `cb.cloud-browser/gpu-isolation` (values: `none` / `mig` / `vfio`).
   Pod templates set NodeAffinity against the right node pool.
2. **NVIDIA k8s-device-plugin** — single DaemonSet per node pool,
   different `--mig-strategy` flags per cell:
   - `none` for non-MIG.
   - `single` for MIG with one profile per node.
   - `mixed` for MIG with multiple profiles per node (lets one
     node serve sessions of different sizes).
3. **RuntimeClass** — `runc` for Phase 2; `kata` for Kata cells;
   `runsc` for gVisor cells. Pod template sets
   `runtimeClassName` per tenant assignment (the BrowserSession
   controller from T71 already chooses runtime class, so the
   plumbing is in place).
4. **Container runtime config** — both cri-o and containerd handle
   the above transparently when the right runtime classes are
   installed. We should pin **containerd** to keep the surface
   consistent with the rest of the K8s ecosystem; cri-o works but
   adds a second config-format to maintain.
5. **Driver-version pinning** — gVisor nvproxy is brittle here.
   When the gVisor cell is in use, the Helm chart pins
   `nvidia.com/gpu-driver-version` to a specific value matched to
   the gVisor release; node-pool image upgrades go through a
   verified-compat lane.
6. **AppArmor + seccomp profiles** — already shipped in `infra/seccomp/`
   from T57. Container security hardening rides on top of every
   cell above; the GPU cell layer is independent.
7. **Health checks** — every Pod runs the rendering-matrix probe
   (T91 `tests/rendering/run-rendering-tests.sh`) on the first
   minute of life; if the GPU initialised wrong, the Pod fails its
   readiness check and the controller cycles it. This is the
   single best protection against silent SwiftShader fallback when
   the device plugin reports a GPU but the driver is unhealthy.

## 5. Performance expectations

Approximate latency / throughput numbers, drawn from
upstream-published figures and T43's cloud-GPU footnote:

| Path                                  | Encode latency p50 (1080p NVENC) | Throughput vs. native | Source                                |
|---------------------------------------|-----------------------------------|------------------------|----------------------------------------|
| nvidia-container-runtime + runc       | ~5 ms                             | ~100% (baseline)       | NVIDIA dev blog, T43 §2.               |
| MIG slice (1g.10gb on H100)           | ~5 ms within slice                | full slice fraction    | NVIDIA MIG docs.                       |
| gVisor + nvproxy                      | ~5-7 ms (slight syscall overhead) | ~85-95%                | gVisor blog, [perf benchmarks](https://gvisor.dev/blog/2024/06/26/cuda-performance/). |
| Kata + VFIO passthrough               | ~5 ms                             | ~95-99%                | Kata + KubeVirt benches.               |
| SwiftShader (Phase 1)                 | n/a (CPU encode)                  | n/a                    | T91 — degraded but functional.         |

Latency is the metric that matters per the brief; on every path
above, NVENC encode at 1080p is single-digit ms. The choice is
about **isolation**, not perf.

## 6. Phase plan

- **Phase 1 (current).** No GPU. Software encoders (T35/T36/T75)
  + SwiftShader rendering (T78). Documented limitations live in
  README "Known limitations" + T91's matrix doc.
- **Phase 2 (planned).** First GPU integration: nvidia-container-runtime
  + runc per node, NVIDIA k8s-device-plugin in `mig-strategy=none`
  mode, Pod-anti-affinity to keep one tenant per node. NVENC
  (T63) is the first encoder to light up; VAAPI (T70) follows on
  Intel Arc fleets if/when. Rendering matrix (T91) re-runs and
  the WebGL row flips from "degraded" to "works (HW)"; the WebGPU
  row flips from "broken" to "works".
- **Phase 3 (multi-tenant).** Per node-pool MIG-or-VFIO routing.
  H100/L40 pool runs MIG slices, each one tenant. Non-MIG pool
  (L4, T4, A10G) runs Kata + VFIO with a 1:1 GPU:tenant ratio —
  more expensive per session but no shared GPU. The
  BrowserSessionPool controller (T71) routes sessions by tenant
  class to the appropriate node-pool.
- **Phase 4 (production scale + polish).** Everything above
  observable through Grafana (T66): per-tenant GPU utilisation,
  per-MIG-slice usage, p99 nvenc latency, per-pool fallback rate.
  Confidential-compute MIG slices on H100 only when a customer
  needs them. AV1 NVENC (T63) lit up on Ada+ pools per T43.

The earliest cell we ship (Phase 2) is the cheapest one that
unblocks both T63 and T91, with a clean upgrade path to Phase 3's
multi-tenant cells.

## 7. Open items + risks

1. **gVisor nvproxy can't ride MIG.** Reconfirm at every gVisor
   release; the moment this constraint lifts, the §3 matrix gets
   strictly better. Until then, MIG-using cells fall back to
   AppArmor + seccomp for the kernel-syscall boundary.
2. **Kata + VFIO is per-tenant *expensive*.** One physical GPU
   per session caps density at the GPU count. We should bench
   the marginal cost vs. user count before committing a non-MIG
   region to it.
3. **Driver-version churn.** `nvidia.ko` rolls quarterly; gVisor
   nvproxy lags by 1-2 releases. Helm chart needs explicit pins
   on both layers, with a "test before promote" lane.
4. **Confidential-compute boot time.** CC-on MIG slices take
   noticeably longer to attest at session start; if we go there
   in Phase 4, the warm-pool sizing (T50) widens.
5. **AMD / Intel GPU cells.** This doc is NVIDIA-heavy because
   our encoder factory has the deepest NVIDIA support and the
   cloud-GPU survey (T43) is NVIDIA-skewed. VAAPI (T70) on Intel
   Arc Pro / AMD MI300X is a real option for cost-sensitive
   non-MIG cells; the Kata + VFIO row above applies symmetrically
   to AMD/Intel because no equivalent of MIG exists outside
   NVIDIA datacenter parts.

## 8. What this doc explicitly does NOT do

- **Does not pick the Phase 3 default cell.** The choice between
  "MIG everywhere" and "Kata + VFIO everywhere" depends on the
  fleet hardware mix and per-session cost target — both will live
  in production simultaneously, not one or the other.
- **Does not redo T44's sandbox threat-model.** T44 owns the
  attack-surface enumeration; this doc consumes its conclusions
  (gVisor's nvproxy security caveat, Firecracker's no-PCI rule,
  Kata's heavyweight characterisation).
- **Does not specify the BrowserSession controller's
  routing-by-tenant logic.** T71 owns that. This doc constrains
  it: routes must pick a runtime class compatible with the
  target node pool's GPU isolation level.
- **Does not commit Helm chart changes.** Those land alongside
  Phase 2 wiring. This doc is the spec the chart implements.

## 9. Cross-references

- **T44 (sandbox-isolation.md)** — gVisor / Kata / Firecracker
  primer; nvproxy compatibility caveats this doc cites.
- **T63 (nvenc-tuning-rationale.md)** — NVENC encoder + the
  cloud-GPU coverage matrix the Phase 2 hardware shapes against.
- **T70 (vaapi-tuning-rationale.md)** — VAAPI encoder + Intel Arc
  / AMD RDNA 3+ coverage notes that constrain non-MIG cells.
- **T91 (rendering-matrix.md)** — what unlocks "WebGL HW + WebGPU
  works" rows when GPU passthrough lands.
- **T43 (av1-encoders.md)** — AV1 HW path requires Ada Lovelace+
  GPUs; that subset of cloud GPUs overlaps the MIG-capable set,
  which is why Phase 3's H100/L40 MIG pool is also our AV1 target.
- **T71 / T80 / T87** — BrowserSessionPool controller, Phase 1
  deployment runbook, multi-region federation; they will all
  need to consume the runtime-class choice this doc specifies.

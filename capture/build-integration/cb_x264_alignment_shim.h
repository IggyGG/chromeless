// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Prevents libx264's transparent-huge-page alignment request from aborting a
// Chromium process that uses PartitionAlloc.
//
// THE CRASH
//
//   [FATAL:partition_root.h(2412)] Check failed:
//       alignment <= internal::kMaxSupportedAlignment.
//     #03  /usr/lib/x86_64-linux-gnu/libx264.so.164+0x6b21
//
//   The whole browser process aborts. Nothing downstream names the real
//   cause: the worker reports "Upstream WS connect failed: Connection
//   refused (111)", physics reports "native FrameSink capture failed to
//   start", and the portal sees WS close 1011 CDP_SESSION_FAILED.
//
// WHY IT HAPPENS
//
//   x264_malloc() (libx264 common/base.c:104) asks memalign() for 2 MiB
//   alignment on any allocation >= 7/8 * 2 MiB, so the buffer can be backed by
//   a transparent huge page:
//
//     6b0e: cmp $0x1bffff,%rdi   ; size > 1,835,007 B ?
//     6b17: mov $0x200000,%edi   ; yes -> alignment = 2 MiB
//     6b1c: call memalign@plt    ; <- the allocator shim intercepts here
//
//   We link libx264 dynamically (config(":x264") in this directory's
//   BUILD.gn), so that memalign resolves through Chromium's allocator shim
//   into PartitionAlloc, whose kMaxSupportedAlignment is kSuperPageSize/2 =
//   1 MiB on x86-64 Linux. The request is ALWAYS exactly double the ceiling,
//   so the PA_CHECK is not a near-miss — it fires every time that path runs.
//
// MEASURED, NOT ASSUMED (2026-08-19)
//
//   Against the exact production libx264 from the deployed guest rootfs,
//   x264_encoder_open() alone requests oversized alignment at:
//
//     640x360    0 allocations      (safe)
//     854x480    1 allocation
//     1280x720   3 allocations      <- OUR PINNED CAPTURE RESOLUTION
//     1920x1080  3 allocations
//
//   An earlier diagnosis in this repo claimed the crash was frame-size
//   dependent and that 720p was safe. That was WRONG: it modelled the
//   allocation as the I420 payload (w*h*1.5), but x264's internal buffers are
//   padded well beyond that. 720p — what capturer.h:264 pins — is already over
//   the line. Capping resolution is therefore NOT a workaround; it would mean
//   going down to 640x360.
//
//   The reason this presents intermittently is codec negotiation, not
//   geometry: CloudBrowserVideoEncoderFactory::GetSupportedFormats() advertises
//   VP9 first and H.264 second, so x264 only opens when a peer selects H.264.
//   Given H.264, the crash is deterministic.
//
// WHY WE INTERCEPT memalign AND NOT x264_malloc
//
//   The obvious fix — define x264_malloc/x264_free in our binary so they
//   pre-empt libx264's — CANNOT WORK, and this was settled by experiment
//   rather than argument. Debian builds libx264 with -Bsymbolic: objdump shows
//   140 direct calls to x264_malloc@@Base, zero through the PLT, and no
//   dynamic relocations for the symbol. Internal call sites bind locally and
//   are unreachable by interposition. That override was built and measured
//   changing nothing.
//
//   memalign() must resolve dynamically (it lives in libc), which is exactly
//   how Chromium's shim intercepts it at all. The aligned-allocation seam is
//   the only interception point that actually sees these requests.
//
// SAFETY OF THE CLAMP
//
//   Over-alignment is always safe — a buffer aligned to 64 bytes satisfies
//   every caller that asked for less, and 64 is x264's own NATIVE_ALIGN for
//   this architecture (the alignment it uses for every allocation below the
//   huge-page threshold). Only the TLB optimisation is lost.
//
//   Verified output-neutral over 30 encoded frames at 720p against the
//   production libx264:
//
//     baseline : TOTAL_BYTES=462849 CHECKSUM=2870504393985350508
//     clamped  : TOTAL_BYTES=462849 CHECKSUM=2870504393985350508
//
//   Bit-identical bitstreams, identical per-frame NAL counts.
//
// BLAST RADIUS
//
//   The clamp only engages above kMaxSupportedAlignment, i.e. for requests
//   that would otherwise CHECK-fail and kill the process. Anything
//   PartitionAlloc can already service passes through untouched, so the only
//   behaviour this can change is "abort" -> "slightly-less-aligned buffer".
//
// PINNED-TREE FACTS THIS DEPENDS ON (read off the build node 2026-08-19,
// triform-7:/var/lib/longhorn/chromeless-build/chromium-src, 147.0.7727.144 —
// NOT from upstream, because upstream and our pin can differ)
//
//   * out/cb-release's generated buildflags.h has
//     PA_BUILDFLAG_INTERNAL_USE_ALLOCATOR_SHIM() = 1, so this compiles in for
//     real rather than into the no-op branch.
//   * memalign -> ShimMemalign -> chain_head->alloc_aligned_function
//     (shim_alloc_functions.h:219-222). The aligned_malloc_* family is a
//     different, Windows-facing path.
//   * AllocToken is declared at allocator_dispatch.h:13, ABOVE `namespace
//     allocator_shim {` on line 22 — it is a GLOBAL name. A parallel attempt
//     at this fix wrote allocator_shim::AllocToken, which does not exist;
//     grep finds zero uses of that qualified form in base/, content/, or
//     components/.
//   * kMaxSupportedAlignment is re-exported into namespace partition_alloc at
//     partition_alloc_constants.h:453 precisely so non-PA code can name it
//     without reaching into ::internal.
//   * //base public_deps the allocator_shim target when use_allocator_shim is
//     on (base/BUILD.gn:1780-1782), and that target carries
//     public_configs = [":public_includes"], so the partition_alloc/... include
//     path arrives through the //base dep this target already has. No new GN
//     dep was needed.
//   * AllocatorDispatch is an aggregate with no user-provided constructor, so
//     the static instance is zero-initialised (`next` == nullptr) before any
//     code runs.
//
// WHY IT IS SAFE TO INSTALL THIS LATE, FROM InitEncode()
//
//   This is installed from H264Encoder::InitEncode() rather than from process
//   startup, which looks alarming for a process-wide allocator hook. Two facts
//   make it correct:
//
//     * InsertAllocatorDispatch() is explicitly thread-safe — a CAS loop with
//       kMaxRetries = 7 and a seq_cst fence — so a second encoder initialising
//       concurrently cannot corrupt the chain.
//     * Nothing x264 does before this point allocates. The only earlier libx264
//       call is x264_param_default_preset(), and base.c:706-715 shows it only
//       writes fields of a caller-owned x264_param_t.
//
//   Installing here (rather than in BrowserMainParts) is also what makes the
//   fix travel with the encoder: these sources live in
//   source_set("encoder_factory") alongside h264_encoder.cc, so
//   cloud_browser_encoder_unittests links the clamp too. A version installed
//   from BrowserMainParts::PreEarlyInitialization() would leave that test
//   binary — the one that actually opens x264 in CI — unprotected.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_X264_ALIGNMENT_SHIM_H_
#define CAPTURE_BUILD_INTEGRATION_CB_X264_ALIGNMENT_SHIM_H_

namespace cloud_browser {

// Installs the aligned-allocation clamp on the process-wide allocator dispatch
// chain. Idempotent and thread-safe: the insertion runs inside a function-local
// static initialiser, so concurrent callers serialise on it.
//
// MUST be called before x264_encoder_open().
//
// No-op when the allocator shim is not compiled in (PA_BUILDFLAG
// USE_ALLOCATOR_SHIM off), in which case memalign goes straight to libc and
// PartitionAlloc never sees the request.
void InstallX264AlignmentShim();

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_X264_ALIGNMENT_SHIM_H_

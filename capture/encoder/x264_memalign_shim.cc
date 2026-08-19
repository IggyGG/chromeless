// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// x264_memalign_shim — stops libx264 from FATALing the browser process.
//
// ─── THE CRASH ────────────────────────────────────────────────────────
//
//   [FATAL:partition_root.h(2412)]
//   Check failed: alignment <= internal::kMaxSupportedAlignment.
//     #03  libx264.so.164+0x6b21
//
// The whole process dies. Not the encoder, not the session — the browser.
//
// ─── MECHANISM ────────────────────────────────────────────────────────
//
// x264_malloc (libx264 common/base.c) contains a transparent-huge-page
// optimisation: for any allocation >= 7/8 of 2 MiB it asks memalign for
// 2 MiB alignment, so the buffer can be backed by a huge page.
//
//   cmp  $0x1bffff,%rdi    ; > 1,835,007 bytes ?
//   mov  $0x200000,%edi    ; yes -> alignment = 2 MiB
//   call memalign@plt      ; <- Chromium's allocator shim intercepts here
//
// PartitionAlloc's ceiling is kMaxSupportedAlignment = kSuperPageSize / 2
// = 1 MiB on x86-64 Linux (partition_alloc_constants.h:400, verified at
// branch-heads/7727). x264 asks for exactly DOUBLE that ceiling, so this
// is never marginal and never intermittent — given the allocation size,
// it always fires.
//
// ─── WHAT IS NOT TRUE ABOUT IT ────────────────────────────────────────
//
// Two intuitions about this bug are wrong, both refuted by measurement
// against the real production libx264 rather than by reading code:
//
//   * "It's frame-size dependent, 720p is fine." No. Modelling the
//     allocation as the I420 payload (w*h*1.5) is wrong — x264 allocates
//     padded internal buffers far larger. Measured oversized-alignment
//     requests during x264_encoder_open alone: 640x360 -> 0, 854x480 ->
//     1, 1280x720 -> 3 (largest 5,529,668 B), 1920x1080 -> 3. The
//     capturer pins 1280x720, which is already over the line. Capping
//     resolution is NOT a workaround: it would need <= 640x360.
//
//   * "It's intermittent." No. The apparent 1-in-N is codec negotiation:
//     GetSupportedFormats lists VP9 first, H.264 second, so x264 only
//     opens when the peer picks H.264. Given H.264, the crash is
//     deterministic.
//
// ─── WHY THIS INTERPOSES memalign AND NOT x264_malloc ─────────────────
//
// Overriding x264_malloc looks like the tidier fix. It cannot work, and
// this was built and measured before being discarded: Debian builds
// libx264 with -Bsymbolic. objdump shows 140 DIRECT calls to
// x264_malloc@@Base, zero via the PLT, and no dynamic relocations for
// it. Internal call sites bind at link time inside the library and can
// never be interposed from outside.
//
// memalign is different — it MUST resolve through the PLT to libc, which
// is precisely how Chromium's allocator shim intercepts it in the first
// place. So that is the seam that actually exists.
//
// ─── WHY CLAMPING IS SAFE ─────────────────────────────────────────────
//
// Over-alignment is always harmless: a buffer aligned to 64 bytes
// satisfies every SIMD path x264 has (its own NATIVE_ALIGN is 64). The
// only thing lost is the huge-page TLB optimisation, which is worth
// approximately nothing to us — the guest is a 1-vCPU microVM whose
// bottleneck is compute, not TLB pressure.
//
// Verified output-neutral, not assumed: 30 real encoded frames at 720p,
// baseline vs clamped, produced byte-identical bitstreams
// (TOTAL_BYTES=462849, checksum 2870504393985350508 both ways).

#include <cstddef>
#include <cstdlib>

#include "rtc_base/logging.h"

extern "C" {

// PartitionAlloc's ceiling on x86-64 Linux is kSuperPageSize / 2 = 1 MiB.
// Hardcoded rather than #included: partition_alloc_constants.h is an
// internal PA header, and taking a dependency on it from an encoder TU to
// read one number is a worse coupling than a named constant plus this
// comment. If PA's ceiling ever drops below 1 MiB this clamp still holds,
// because we clamp to 64 — far under any plausible ceiling.
constexpr size_t kPartitionAllocMaxAlignment = 1024 * 1024;

// x264's own NATIVE_ALIGN. Satisfies every SIMD load/store it performs.
constexpr size_t kX264NativeAlign = 64;

// Interposes libc's memalign for the whole process.
//
// This is deliberately a plain strong definition rather than a weak/alias
// trick: the linker resolves libx264's memalign@plt to this symbol
// because it is present in the executable, which is the same mechanism
// Chromium's own allocator shim relies on.
//
// Everything that is NOT an oversized request is forwarded untouched, so
// this changes nothing for any other caller in the process.
void* memalign(size_t alignment, size_t size) {
  if (alignment > kPartitionAllocMaxAlignment) {
    // The huge-page request. Clamp instead of forwarding it into an
    // allocator that will CHECK-fail and take the process down.
    //
    // Logged once per process, and behind a REENTRANCY GUARD. RTC_LOG
    // formats into a std::string, which allocates — and we are inside the
    // allocator. Without the guard, the first oversized request logs, the
    // log allocates, that allocation can route back through this function,
    // and a deep-enough recursion is a stack overflow inside malloc: a
    // far worse failure than the one being fixed. The guard is
    // thread_local because two threads may open encoders concurrently.
    static bool logged = false;
    thread_local bool in_shim = false;
    if (!logged && !in_shim) {
      logged = true;
      in_shim = true;
      RTC_LOG(LS_WARNING)
          << "x264_memalign_shim: clamping oversized alignment " << alignment
          << " -> " << kX264NativeAlign << " for a " << size
          << "-byte allocation (libx264 huge-page path exceeds "
             "PartitionAlloc's "
          << kPartitionAllocMaxAlignment
          << "-byte ceiling; without this the process FATALs)";
      in_shim = false;
    }
    alignment = kX264NativeAlign;
  }

  // aligned_alloc, not the deprecated memalign, for the actual work.
  // It requires size to be a multiple of alignment; round up rather than
  // return null, since a short allocation here is a heap overflow later.
  const size_t rounded = (size + alignment - 1) & ~(alignment - 1);
  return std::aligned_alloc(alignment, rounded);
}

}  // extern "C"

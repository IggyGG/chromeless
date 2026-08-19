// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// See cb_x264_alignment_shim.h for the full rationale, the disassembly, and
// the measurements behind this file.

#include "capture/build-integration/cb_x264_alignment_shim.h"

#include <stddef.h>

#include "base/logging.h"
#include "partition_alloc/buildflags.h"

#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)
#include "partition_alloc/partition_alloc_constants.h"
#include "partition_alloc/shim/allocator_dispatch.h"
#include "partition_alloc/shim/allocator_shim.h"
#endif

namespace cloud_browser {

#if PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

namespace {

// x264's NATIVE_ALIGN for x86-64 (common/osdep.h). This is the alignment
// libx264 itself uses for every allocation below its huge-page threshold, so
// it is by construction sufficient for all of its SIMD paths.
constexpr size_t kX264NativeAlign = 64;

// Our link in the allocator chain. Must have static storage duration and
// outlive the process: InsertAllocatorDispatch() stores this pointer in the
// chain and there is no supported way to remove a non-head link
// (RemoveAllocatorDispatchForTesting DCHECKs that the target IS the head).
//
// Defined before the function that reads `.next`, because that function
// refers to it by name — the same shape //base/allocator/dispatcher uses
// (allocator_dispatch_.next->...). Reading `.next` from inside the callback
// is safe: InsertAllocatorDispatch() assigns it before publishing us as the
// chain head (allocator_shim_functions.h:113), so by the time any allocation
// can reach ClampedAllocAligned, `.next` is set.
allocator_shim::AllocatorDispatch g_dispatch;

// Logged once, on the first clamp, so a crash-loop investigation can tell at
// a glance whether this path is live — without a line per allocation.
bool g_logged_first_clamp = false;

void* ClampedAllocAligned(size_t alignment,
                          size_t size,
                          allocator_shim::AllocToken alloc_token,
                          void* context) {
  if (alignment > partition_alloc::internal::kMaxSupportedAlignment) {
    // Without this, the next link is PartitionAlloc, which CHECK-fails and
    // takes the whole browser process with it. See the header: this is
    // libx264 asking for a 2 MiB (huge-page) alignment against a ceiling of
    // 1 MiB.
    if (!g_logged_first_clamp) {
      g_logged_first_clamp = true;
      LOG(WARNING) << "cb: clamped an aligned allocation from " << alignment
                   << " to " << kX264NativeAlign << " bytes (size=" << size
                   << "). PartitionAlloc's ceiling is "
                   << partition_alloc::internal::kMaxSupportedAlignment
                   << "; the request would have been fatal. This is libx264's "
                      "transparent-huge-page path — see "
                      "cb_x264_alignment_shim.h.";
    }
    alignment = kX264NativeAlign;
  }

  return g_dispatch.next->alloc_aligned_function(alignment, size, alloc_token,
                                                 context);
}

}  // namespace

void InstallX264AlignmentShim() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;

  // Only alloc_aligned_function is overridden. Every other entry point stays
  // nullptr, and InsertAllocatorDispatch fills each one in directly from the
  // next link (AllocatorDispatch::OptimizeAllocatorDispatchTable ->
  // COPY_IF_NULLPTR, allocator_dispatch.h). That flattening means the other
  // entry points do not even pay a trampoline hop through us: ordinary
  // allocation is untouched, and only aligned allocation routes through the
  // clamp.
  g_dispatch.alloc_aligned_function = &ClampedAllocAligned;

  allocator_shim::InsertAllocatorDispatch(&g_dispatch);
}

#else  // !PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

void InstallX264AlignmentShim() {
  // Without the shim, memalign() goes straight to libc and PartitionAlloc
  // never sees libx264's oversized alignment request, so there is nothing to
  // clamp.
}

#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

}  // namespace cloud_browser

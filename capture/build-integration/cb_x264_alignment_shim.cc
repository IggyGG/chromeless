// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.

#include "capture/build-integration/cb_x264_alignment_shim.h"

#include <stddef.h>

#include <atomic>

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

constexpr size_t kX264NativeAlign = 64;

// InsertAllocatorDispatch() stores this address for the process lifetime and
// fills every null callback from the next link in the dispatch chain.
allocator_shim::AllocatorDispatch g_dispatch;
std::atomic<bool> g_logged_first_clamp{false};

void* ClampedAllocAligned(size_t alignment,
                          size_t size,
                          AllocToken alloc_token,
                          void* context) {
  if (alignment > partition_alloc::kMaxSupportedAlignment) {
    // Logging from an allocator callback must not allocate. The streaming LOG
    // macros can recurse through this same callback; RAW_LOG cannot.
    if (!g_logged_first_clamp.exchange(true, std::memory_order_relaxed)) {
      RAW_LOG(WARNING,
              "cb: clamped an oversized aligned allocation to 64 bytes; the "
              "request exceeded PartitionAlloc's supported alignment and "
              "would otherwise abort the process");
    }
    alignment = kX264NativeAlign;
  }

  return g_dispatch.next->alloc_aligned_function(alignment, size, alloc_token,
                                                 context);
}

}  // namespace

void InstallX264AlignmentShim() {
  // Function-local static initialization is serialized by C++, so concurrent
  // encoders cannot observe the "installed" state before insertion completes.
  static const bool installed = [] {
    g_dispatch.alloc_aligned_function = &ClampedAllocAligned;
    allocator_shim::InsertAllocatorDispatch(&g_dispatch);
    return true;
  }();
  (void)installed;
}

#else

void InstallX264AlignmentShim() {}

#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

}  // namespace cloud_browser

// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// See cb_x264_alignment_shim.h for the full rationale, the disassembly, and
// the measurements behind this file.

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
// refers to it by name — the same shape //base/allocator/dispatcher and
// //components/gwp_asan/client both use (g_allocator_dispatch.next->...).
// Reading `.next` from inside the callback is safe: InsertAllocatorDispatch()
// assigns it before publishing us as the chain head
// (allocator_shim_functions.h:113), so by the time any allocation can reach
// ClampedAllocAligned, `.next` is set.
allocator_shim::AllocatorDispatch g_dispatch;

// Logged once, on the first clamp, so a crash-loop investigation can tell at
// a glance whether this path is live — without a line per allocation.
//
// std::atomic, not bool: allocator hooks run on every thread in the process
// with no lock held, so a plain bool here is a data race by construction.
// Relaxed is enough — we only need "exactly one thread wins", not ordering
// against other memory.
std::atomic<bool> g_logged_first_clamp{false};

void* ClampedAllocAligned(size_t alignment,
                          size_t size,
                          // NOTE the unqualified AllocToken: it is declared at
                          // allocator_dispatch.h:13, ABOVE `namespace
                          // allocator_shim {` on line 22, so despite
                          // everything around it being namespaced it lives in
                          // the GLOBAL namespace. `allocator_shim::AllocToken`
                          // does not compile, and grep over base/, content/
                          // and components/ on the pinned tree finds zero uses
                          // of that qualified form.
                          AllocToken alloc_token,
                          void* context) {
  if (alignment > partition_alloc::kMaxSupportedAlignment) {
    // Without this, the next link is PartitionAlloc, which CHECK-fails and
    // takes the whole browser process with it. See the header: this is
    // libx264 asking for a 2 MiB (huge-page) alignment against a ceiling of
    // 1 MiB.
    //
    // RAW_LOG, not LOG(WARNING) << ...: we are INSIDE the allocator. The
    // streaming form builds a std::ostringstream, which allocates, which
    // re-enters the shim we are standing in. No in-tree dispatch hook logs
    // via the streaming macros — gwp_asan's three shims and
    // base/allocator/dispatcher contain no LOG() at all. RawLog() is
    // documented as the async-signal-safe mechanism (base/logging.h:670) and
    // writes straight to stderr, so it needs neither an allocation nor
    // initialised logging — which matters because this can fire before much
    // of the browser exists. The cost is that the message must be a literal;
    // the numbers live in the header instead.
    if (!g_logged_first_clamp.exchange(true, std::memory_order_relaxed)) {
      RAW_LOG(WARNING,
              "cb: clamped an oversized aligned allocation to 64 bytes; the "
              "request exceeded PartitionAlloc's kMaxSupportedAlignment and "
              "would have been fatal. This is libx264's transparent-huge-page "
              "path -- see capture/build-integration/cb_x264_alignment_shim.h");
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
  // COPY_IF_NULLPTR, allocator_dispatch.h:156-186). That flattening means the
  // other entry points do not even pay a trampoline hop through us: ordinary
  // allocation is untouched, and only aligned allocation routes through the
  // clamp.
  //
  // g_dispatch starts zero-initialised: AllocatorDispatch is an aggregate
  // with no user-provided constructor (allocator_dispatch.h:24-117), so
  // static zero-init sets every member — including `next` — to nullptr before
  // any code runs.
  g_dispatch.alloc_aligned_function = &ClampedAllocAligned;

  allocator_shim::InsertAllocatorDispatch(&g_dispatch);
}

#else  // !PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

void InstallX264AlignmentShim() {
  // Without the shim, memalign() goes straight to libc and PartitionAlloc
  // never sees libx264's oversized alignment request, so there is nothing to
  // clamp. Note this branch is dead in our own build: out/cb-release's
  // generated buildflags.h has PA_BUILDFLAG_INTERNAL_USE_ALLOCATOR_SHIM() = 1
  // (read off the build node 2026-08-19). It exists for the standalone/other
  // configurations that could turn it off.
}

#endif  // PA_BUILDFLAG(USE_ALLOCATOR_SHIM)

}  // namespace cloud_browser

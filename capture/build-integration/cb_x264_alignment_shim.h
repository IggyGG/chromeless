// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Prevents libx264's transparent-huge-page alignment request from aborting a
// Chromium process that uses PartitionAlloc.
//
// Debian's libx264 asks memalign() for 2 MiB alignment when an internal buffer
// is at least 1.75 MiB. Chromium's x86-64 PartitionAlloc limit is 1 MiB, so the
// allocator CHECK-fails before x264 can encode a 720p frame. x264 otherwise
// uses 64-byte NATIVE_ALIGN for these buffers; clamping only requests above
// PartitionAlloc's ceiling to 64 bytes removes the huge-page optimisation but
// preserves x264's SIMD contract. A measurement against the production
// libx264 produced bit-identical output across 30 720p frames with the clamp.
//
// The dispatch is installed once per process and must run before
// x264_encoder_open(). It is a no-op when Chromium's allocator shim is absent.

#ifndef CAPTURE_BUILD_INTEGRATION_CB_X264_ALIGNMENT_SHIM_H_
#define CAPTURE_BUILD_INTEGRATION_CB_X264_ALIGNMENT_SHIM_H_

namespace cloud_browser {

void InstallX264AlignmentShim();

}  // namespace cloud_browser

#endif  // CAPTURE_BUILD_INTEGRATION_CB_X264_ALIGNMENT_SHIM_H_

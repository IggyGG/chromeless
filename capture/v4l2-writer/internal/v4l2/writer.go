// Package v4l2 implements the platform-portable surface of the
// video-loopback writer. The Linux implementation lives in
// writer_linux.go (build-tagged); other platforms get a stub that
// returns ErrUnsupported so the binary still compiles for
// development on macOS / Windows even though it can't actually
// write to a v4l2 device there.
//
// Wire format consumed: documented in
// docs/protocols/webcam-mic-passthrough.md (T81). The init header
// negotiates resolution + framerate; subsequent frames are I420
// (a.k.a. YUV420p) at the negotiated WxH.
package v4l2

import "errors"

// ErrUnsupported is returned by Open on platforms without v4l2.
var ErrUnsupported = errors.New("v4l2: not supported on this platform")

// VideoFormat is the negotiated stream geometry.
type VideoFormat struct {
	Width  int
	Height int
	FPS    int
}

// FrameSize returns the byte size of one I420 frame at the
// negotiated resolution: width × height × 3 / 2 (Y plane plus U/V
// subsampled at half resolution in each dimension).
func (f VideoFormat) FrameSize() int {
	return f.Width * f.Height * 3 / 2
}

// Writer is the abstract interface the dispatcher writes frames to.
// Tests substitute a mock; real Linux runs use the openLinux()
// implementation in writer_linux.go.
type Writer interface {
	WriteFrame(buf []byte) error
	Close() error
}

//go:build linux

package v4l2

import (
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/unix"
)

// V4L2 ioctl + struct definitions we need. The values come from
// <linux/videodev2.h>; the Linux ABI guarantees them across
// kernel versions, so we can hand-roll them rather than CGO.
//
// We only need a small subset: VIDIOC_S_FMT for the output type,
// V4L2_BUF_TYPE_VIDEO_OUTPUT (the v4l2loopback "write side"
// buffer type), V4L2_PIX_FMT_YUV420 (FourCC "YU12") for I420.
const (
	v4l2BufTypeVideoOutput = 2  // V4L2_BUF_TYPE_VIDEO_OUTPUT
	v4l2FieldNone          = 1  // V4L2_FIELD_NONE — progressive
	v4l2ColorspaceREC709   = 3  // V4L2_COLORSPACE_REC709 — what
	                            // libvpx / x264 call "BT.709"; the
	                            // negotiated output of WebRTC video.

	// FourCC "YU12" → 0x32315559 little-endian. This is
	// V4L2_PIX_FMT_YUV420 in <linux/videodev2.h>.
	v4l2PixFmtYUV420 = 0x32315559
)

// VIDIOC_S_FMT is a write-then-read ioctl ("WR") — it both writes
// the requested format struct in and reads the negotiated result
// out. We compute the encoded ioctl number with the standard
// _IOWR(type, nr, struct_size) macro from <asm-generic/ioctl.h>.
//
// type = 'V' (0x56), nr = 5 (VIDIOC_S_FMT), size = sizeof(v4l2_format)
// = 208 bytes on 64-bit Linux. We hard-code the encoded number to
// avoid pulling in CGO just for the macro.
//
// VIDIOC_S_FMT = 0xC0D05605
const vidiocSFmt = 0xC0D05605

// v4l2PixFormat mirrors `struct v4l2_pix_format` from <linux/videodev2.h>.
// Layout is fixed by the kernel ABI.
type v4l2PixFormat struct {
	Width        uint32
	Height       uint32
	PixelFormat  uint32
	Field        uint32
	BytesPerLine uint32
	SizeImage    uint32
	Colorspace   uint32
	Priv         uint32
	Flags        uint32
	YCbCrEnc     uint32
	Quantization uint32
	XferFunc     uint32
}

// v4l2Format mirrors `struct v4l2_format` — a tagged union over
// `type`. We only ever populate the pixel-format arm, so we
// allocate the full 200-byte fmt[] padding statically.
type v4l2Format struct {
	Type uint32
	// 4 bytes of padding to align fmt to 8 (matches the kernel C struct).
	_   uint32
	Pix v4l2PixFormat
	// Tail padding so total size matches the kernel struct's 208
	// bytes. The kernel reads/writes the full struct via copy_to_user
	// / copy_from_user, so undersize here would be a memory-safety bug.
	_   [200 - 56]byte
}

type linuxWriter struct {
	device string
	file   *os.File
	format VideoFormat
}

// Open opens the v4l2 output device at `path` and configures it
// for I420 at fmt.Width x fmt.Height. The framerate is informational
// only on v4l2loopback — the device accepts whatever rate the
// writer pushes.
func Open(path string, fmt VideoFormat) (Writer, error) {
	if fmt.Width <= 0 || fmt.Height <= 0 {
		return nil, errBadFormat(fmt)
	}
	// O_RDWR is required for VIDIOC_S_FMT even though we only write
	// frames; the kernel's permission check looks for read+write
	// before granting OUTPUT-type buffers.
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return nil, err
	}
	if err := setOutputFormat(f, fmt); err != nil {
		_ = f.Close()
		return nil, err
	}
	return &linuxWriter{device: path, file: f, format: fmt}, nil
}

func setOutputFormat(f *os.File, vf VideoFormat) error {
	frameBytes := uint32(vf.FrameSize())
	// Each I420 row of luma is `width` bytes; v4l2loopback uses this
	// as the bytes-per-line of the Y plane. The chroma planes are
	// implicit at half-resolution per the FourCC.
	cfg := v4l2Format{
		Type: v4l2BufTypeVideoOutput,
		Pix: v4l2PixFormat{
			Width:        uint32(vf.Width),
			Height:       uint32(vf.Height),
			PixelFormat:  v4l2PixFmtYUV420,
			Field:        v4l2FieldNone,
			BytesPerLine: uint32(vf.Width),
			SizeImage:    frameBytes,
			Colorspace:   v4l2ColorspaceREC709,
		},
	}
	_, _, errno := unix.Syscall(
		unix.SYS_IOCTL,
		f.Fd(),
		vidiocSFmt,
		uintptr(unsafe.Pointer(&cfg)),
	)
	if errno != 0 {
		return fmt.Errorf("VIDIOC_S_FMT(%dx%d YUV420): %w",
			vf.Width, vf.Height, errno)
	}
	// The kernel may negotiate a slightly different format
	// (capabilities-driven). v4l2loopback accepts whatever we ask
	// for, but we still log a warning if the read-back differs.
	if cfg.Pix.Width != uint32(vf.Width) || cfg.Pix.Height != uint32(vf.Height) {
		return fmt.Errorf("v4l2: requested %dx%d, kernel returned %dx%d",
			vf.Width, vf.Height, cfg.Pix.Width, cfg.Pix.Height)
	}
	return nil
}

func (w *linuxWriter) WriteFrame(buf []byte) error {
	if want := w.format.FrameSize(); len(buf) != want {
		return fmt.Errorf("v4l2: frame size mismatch — got %d want %d (I420 at %dx%d)",
			len(buf), want, w.format.Width, w.format.Height)
	}
	// Single write(2) — v4l2loopback's read side reassembles the
	// buffer unit on the consumer end. Short writes are an error
	// (kernel write to a v4l2 output device is all-or-nothing).
	n, err := w.file.Write(buf)
	if err != nil {
		return err
	}
	if n != len(buf) {
		return fmt.Errorf("v4l2: short write %d/%d", n, len(buf))
	}
	return nil
}

func (w *linuxWriter) Close() error {
	if w.file == nil {
		return nil
	}
	err := w.file.Close()
	w.file = nil
	return err
}

func errBadFormat(vf VideoFormat) error {
	return fmt.Errorf("v4l2: invalid format: %+v", vf)
}

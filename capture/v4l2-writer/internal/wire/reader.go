// Package wire parses the streamer→writer Unix socket frame stream
// documented in docs/protocols/webcam-mic-passthrough.md (T81).
//
// Two stream variants share the same length-prefixed framing:
//
//   16-byte init header (sent once, before any frames):
//
//     video:  "CBV1" | width(4 BE) | height(4 BE) | fps(4 BE)
//     audio:  "CBA1" | sample_rate(4 BE) | channels(4 BE) | reserved(4 BE)
//
//   per-frame:
//
//     size(4 BE) | pts_ms(4 BE) | data[size]
//
// The choice of magic constants (CBV1 / CBA1) is what tells a writer
// it's looking at the right kind of stream. Mismatched magic ⇒
// ErrBadMagic and the writer disconnects.
package wire

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	// MagicVideo is the leading 4-byte tag on a video stream's init header.
	MagicVideo = "CBV1"
	// MagicAudio is the leading 4-byte tag on an audio stream's init header.
	MagicAudio = "CBA1"

	// initHeaderSize is the wire size of the once-per-stream init header.
	initHeaderSize = 16
	// frameHeaderSize is the wire size of each per-frame header.
	frameHeaderSize = 8

	// MaxFrameSize bounds a single frame body so a buggy / malicious
	// peer can't ask us to allocate gigabytes. 32 MiB is comfortably
	// above 4K I420 (~12 MiB) and 1s of 48k stereo S16LE (~192 KiB).
	MaxFrameSize = 32 * 1024 * 1024
)

// VideoInit holds the once-per-stream metadata for a video stream.
type VideoInit struct {
	Width  int
	Height int
	FPS    int
}

// AudioInit holds the once-per-stream metadata for an audio stream.
type AudioInit struct {
	SampleRate int
	Channels   int
}

// Frame is one parsed frame body + the source-side timestamp.
type Frame struct {
	PTSMillis uint32
	Body      []byte
}

// Errors used by the parser.
var (
	ErrBadMagic    = errors.New("wire: bad magic in init header")
	ErrFrameTooBig = errors.New("wire: frame size exceeds MaxFrameSize")
)

// VideoReader parses a video stream from r.
type VideoReader struct {
	r io.Reader
}

// NewVideoReader constructs a reader bound to r.
func NewVideoReader(r io.Reader) *VideoReader { return &VideoReader{r: r} }

// Init reads and validates the once-per-stream video init header.
// MUST be called before the first call to Frame.
func (vr *VideoReader) Init() (VideoInit, error) {
	buf := make([]byte, initHeaderSize)
	if _, err := io.ReadFull(vr.r, buf); err != nil {
		return VideoInit{}, err
	}
	if string(buf[0:4]) != MagicVideo {
		return VideoInit{}, fmt.Errorf("%w: got %q want %q",
			ErrBadMagic, string(buf[0:4]), MagicVideo)
	}
	return VideoInit{
		Width:  int(binary.BigEndian.Uint32(buf[4:8])),
		Height: int(binary.BigEndian.Uint32(buf[8:12])),
		FPS:    int(binary.BigEndian.Uint32(buf[12:16])),
	}, nil
}

// Frame reads the next length-prefixed frame body. dst is reused
// across calls (returned slice points into it) — copy if you need
// the body to outlive the next call. Returns io.EOF on clean
// stream end.
func (vr *VideoReader) Frame(dst []byte) (Frame, []byte, error) {
	return readFrame(vr.r, dst)
}

// AudioReader parses an audio stream from r.
type AudioReader struct {
	r io.Reader
}

func NewAudioReader(r io.Reader) *AudioReader { return &AudioReader{r: r} }

// Init reads and validates the once-per-stream audio init header.
func (ar *AudioReader) Init() (AudioInit, error) {
	buf := make([]byte, initHeaderSize)
	if _, err := io.ReadFull(ar.r, buf); err != nil {
		return AudioInit{}, err
	}
	if string(buf[0:4]) != MagicAudio {
		return AudioInit{}, fmt.Errorf("%w: got %q want %q",
			ErrBadMagic, string(buf[0:4]), MagicAudio)
	}
	return AudioInit{
		SampleRate: int(binary.BigEndian.Uint32(buf[4:8])),
		Channels:   int(binary.BigEndian.Uint32(buf[8:12])),
		// reserved[12:16] ignored
	}, nil
}

// Frame reads the next length-prefixed audio frame.
func (ar *AudioReader) Frame(dst []byte) (Frame, []byte, error) {
	return readFrame(ar.r, dst)
}

// readFrame implements the per-frame parse shared between video + audio.
// `dst` is the working buffer; it's grown if too small. The returned
// `nextDst` is the buffer to pass to the next call.
func readFrame(r io.Reader, dst []byte) (Frame, []byte, error) {
	var hdr [frameHeaderSize]byte
	if _, err := io.ReadFull(r, hdr[:]); err != nil {
		return Frame{}, dst, err
	}
	size := binary.BigEndian.Uint32(hdr[0:4])
	pts := binary.BigEndian.Uint32(hdr[4:8])
	if int(size) > MaxFrameSize {
		return Frame{}, dst, fmt.Errorf("%w: %d > %d",
			ErrFrameTooBig, size, MaxFrameSize)
	}
	if cap(dst) < int(size) {
		dst = make([]byte, size)
	} else {
		dst = dst[:size]
	}
	if _, err := io.ReadFull(r, dst); err != nil {
		return Frame{}, dst, err
	}
	return Frame{PTSMillis: pts, Body: dst}, dst, nil
}

// EncodeVideoInit returns the on-wire bytes of a video init header.
// Useful for tests + for an in-process pipe-the-stream tool.
func EncodeVideoInit(v VideoInit) []byte {
	buf := make([]byte, initHeaderSize)
	copy(buf[0:4], MagicVideo)
	binary.BigEndian.PutUint32(buf[4:8], uint32(v.Width))
	binary.BigEndian.PutUint32(buf[8:12], uint32(v.Height))
	binary.BigEndian.PutUint32(buf[12:16], uint32(v.FPS))
	return buf
}

// EncodeAudioInit returns the on-wire bytes of an audio init header.
func EncodeAudioInit(a AudioInit) []byte {
	buf := make([]byte, initHeaderSize)
	copy(buf[0:4], MagicAudio)
	binary.BigEndian.PutUint32(buf[4:8], uint32(a.SampleRate))
	binary.BigEndian.PutUint32(buf[8:12], uint32(a.Channels))
	// reserved
	return buf
}

// EncodeFrame returns the on-wire bytes of one frame header + body.
func EncodeFrame(pts uint32, body []byte) []byte {
	out := make([]byte, frameHeaderSize+len(body))
	binary.BigEndian.PutUint32(out[0:4], uint32(len(body)))
	binary.BigEndian.PutUint32(out[4:8], pts)
	copy(out[frameHeaderSize:], body)
	return out
}

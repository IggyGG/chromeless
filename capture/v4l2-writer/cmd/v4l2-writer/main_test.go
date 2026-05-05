// Tests for the v4l2-writer command. Cover:
//
//   - parseFlags edge cases (mode required, socket required, etc.)
//   - handleVideo / handleAudio happy paths against fake sinks
//   - one-frame and many-frames streams
//   - mid-stream truncation surfaces as an error (not silent EOF)
//   - listen + run cycle accepts a connection, dispatches, accepts another
package main

import (
	"bytes"
	"context"
	"errors"
	"io"
	"log/slog"
	"net"
	"os"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/iggy/chromeless/capture/v4l2-writer/internal/pulse"
	"github.com/iggy/chromeless/capture/v4l2-writer/internal/v4l2"
	"github.com/iggy/chromeless/capture/v4l2-writer/internal/wire"
)

func quietLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

type fakeVideoWriter struct {
	mu      sync.Mutex
	frames  [][]byte
	closeErr error
	closed  bool
}

func (f *fakeVideoWriter) WriteFrame(buf []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	cp := make([]byte, len(buf))
	copy(cp, buf)
	f.frames = append(f.frames, cp)
	return nil
}
func (f *fakeVideoWriter) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	return f.closeErr
}

type fakeAudioWriter struct {
	mu     sync.Mutex
	chunks [][]byte
	closed bool
}

func (f *fakeAudioWriter) WriteSamples(s []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	cp := make([]byte, len(s))
	copy(cp, s)
	f.chunks = append(f.chunks, cp)
	return nil
}
func (f *fakeAudioWriter) Close() error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	return nil
}

// installFakeOpeners swaps in test fakes for the package-level
// openVideo / openAudio dispatchers, returning a restore function.
func installFakeOpeners(t *testing.T, vid *fakeVideoWriter, aud *fakeAudioWriter) func() {
	t.Helper()
	prevVid, prevAud := openVideo, openAudio
	openVideo = func(_ string, f v4l2.VideoFormat) (v4l2.Writer, error) {
		_ = f
		return vid, nil
	}
	openAudio = func(_ string, f pulse.AudioFormat) (pulse.Writer, error) {
		_ = f
		return aud, nil
	}
	return func() {
		openVideo, openAudio = prevVid, prevAud
	}
}

// ---------------------------------------------------------------------------
// parseFlags
// ---------------------------------------------------------------------------

func TestParseFlags(t *testing.T) {
	cases := []struct {
		name   string
		args   []string
		wantOK bool
		wantErrSubstr string
	}{
		{"video valid", []string{"--mode=video", "--socket=/tmp/v.sock"}, true, ""},
		{"audio valid", []string{"--mode=audio", "--socket=/tmp/a.sock"}, true, ""},
		{"missing mode", []string{"--socket=/tmp/x.sock"}, false, "invalid --mode"},
		{"bad mode", []string{"--mode=garbage", "--socket=/tmp/x"}, false, "invalid --mode"},
		{"missing socket", []string{"--mode=video"}, false, "--socket is required"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cfg, err := parseFlags(tc.args)
			if tc.wantOK {
				if err != nil {
					t.Fatalf("unexpected error: %v", err)
				}
				if cfg.mode == "" {
					t.Errorf("mode unset")
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tc.wantErrSubstr) {
				t.Errorf("err=%v, want contains %q", err, tc.wantErrSubstr)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// handleVideo
// ---------------------------------------------------------------------------

func TestHandleVideoHappyPath(t *testing.T) {
	vid := &fakeVideoWriter{}
	restore := installFakeOpeners(t, vid, nil)
	defer restore()

	const W, H = 64, 48
	frameSize := W * H * 3 / 2
	frame1 := bytes.Repeat([]byte{0xAA}, frameSize)
	frame2 := bytes.Repeat([]byte{0xBB}, frameSize)

	var buf bytes.Buffer
	buf.Write(wire.EncodeVideoInit(wire.VideoInit{Width: W, Height: H, FPS: 30}))
	buf.Write(wire.EncodeFrame(0, frame1))
	buf.Write(wire.EncodeFrame(33, frame2))

	n, err := handleVideo(context.Background(),
		config{mode: "video", device: "/dev/video10"},
		&buf, quietLogger())
	if err != nil {
		t.Fatalf("handleVideo: %v", err)
	}
	if n != 2 {
		t.Errorf("frames = %d, want 2", n)
	}
	if !vid.closed {
		t.Errorf("video writer should have been closed")
	}
	if len(vid.frames) != 2 {
		t.Fatalf("expected 2 frames buffered, got %d", len(vid.frames))
	}
	if !bytes.Equal(vid.frames[0], frame1) || !bytes.Equal(vid.frames[1], frame2) {
		t.Errorf("frame body mismatch")
	}
}

func TestHandleVideoTruncated(t *testing.T) {
	// Init header followed by half a frame — should surface as an
	// error, NOT a silent successful return.
	vid := &fakeVideoWriter{}
	restore := installFakeOpeners(t, vid, nil)
	defer restore()

	var buf bytes.Buffer
	buf.Write(wire.EncodeVideoInit(wire.VideoInit{Width: 4, Height: 4, FPS: 1}))
	buf.Write(wire.EncodeFrame(0, []byte{0, 0, 0})) // declared size 3 but I420(4x4) is 24
	// Actually EncodeFrame writes exactly len(body) so the above is
	// well-formed (3 bytes). To truncate, hand-roll a header that
	// claims 24 bytes followed by only 4.
	buf.Reset()
	buf.Write(wire.EncodeVideoInit(wire.VideoInit{Width: 4, Height: 4, FPS: 1}))
	hdr := []byte{0, 0, 0, 24, 0, 0, 0, 0} // size=24, pts=0
	buf.Write(hdr)
	buf.Write([]byte{1, 2, 3, 4}) // only 4 of 24 bytes

	_, err := handleVideo(context.Background(),
		config{mode: "video"}, &buf, quietLogger())
	if err == nil {
		t.Errorf("expected error on truncated stream")
	}
}

func TestHandleVideoSizeMismatch(t *testing.T) {
	// A well-formed wire frame whose body length doesn't match the
	// init header's WxH should surface from v4l2.WriteFrame as a
	// "frame size mismatch" error. The fakeVideoWriter doesn't
	// validate frame size, so this test asserts at the wire level —
	// we shape the test around what v4l2.Open does in production.
	//
	// The fake accepts any size; we use it here only to confirm the
	// dispatcher passes whatever the wire delivers without
	// re-shaping. Real v4l2.Open uses VideoFormat.FrameSize() to
	// reject size mismatches at write time.
	vid := &fakeVideoWriter{}
	restore := installFakeOpeners(t, vid, nil)
	defer restore()

	var buf bytes.Buffer
	buf.Write(wire.EncodeVideoInit(wire.VideoInit{Width: 64, Height: 48, FPS: 30}))
	buf.Write(wire.EncodeFrame(0, []byte("too-small-but-the-fake-accepts")))

	n, err := handleVideo(context.Background(), config{mode: "video"},
		&buf, quietLogger())
	if err != nil {
		t.Fatalf("dispatch error: %v", err)
	}
	if n != 1 {
		t.Errorf("frames = %d, want 1", n)
	}
}

// ---------------------------------------------------------------------------
// handleAudio
// ---------------------------------------------------------------------------

func TestHandleAudioHappyPath(t *testing.T) {
	aud := &fakeAudioWriter{}
	restore := installFakeOpeners(t, nil, aud)
	defer restore()

	samples1 := []byte{0x10, 0x00, 0x20, 0x00, 0xff, 0xff, 0xfe, 0xff}
	samples2 := []byte{0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04}

	var buf bytes.Buffer
	buf.Write(wire.EncodeAudioInit(wire.AudioInit{SampleRate: 48000, Channels: 2}))
	buf.Write(wire.EncodeFrame(0, samples1))
	buf.Write(wire.EncodeFrame(20, samples2))

	n, err := handleAudio(context.Background(),
		config{mode: "audio", pulseSink: "cb_passthrough"},
		&buf, quietLogger())
	if err != nil {
		t.Fatalf("handleAudio: %v", err)
	}
	if n != 2 {
		t.Errorf("frames = %d, want 2", n)
	}
	if !aud.closed {
		t.Errorf("audio writer should have been closed")
	}
	if len(aud.chunks) != 2 ||
		!bytes.Equal(aud.chunks[0], samples1) ||
		!bytes.Equal(aud.chunks[1], samples2) {
		t.Errorf("chunk bodies mismatch")
	}
}

func TestHandleAudioBadMagic(t *testing.T) {
	aud := &fakeAudioWriter{}
	restore := installFakeOpeners(t, nil, aud)
	defer restore()

	// Send a video init header to the audio handler — magic mismatch.
	var buf bytes.Buffer
	buf.Write(wire.EncodeVideoInit(wire.VideoInit{Width: 1, Height: 1, FPS: 1}))

	_, err := handleAudio(context.Background(),
		config{mode: "audio"}, &buf, quietLogger())
	if err == nil || !errors.Is(err, wire.ErrBadMagic) {
		t.Errorf("expected ErrBadMagic, got %v", err)
	}
}

// ---------------------------------------------------------------------------
// run + listen — accept a connection, dispatch, accept another
// ---------------------------------------------------------------------------

func TestRunAcceptsMultipleConnectionsSequentially(t *testing.T) {
	vid := &fakeVideoWriter{}
	restore := installFakeOpeners(t, vid, nil)
	defer restore()

	// macOS sockaddr_un.sun_path is 104 bytes; t.TempDir() can
	// already be ≥ 80 bytes which leaves no room for the filename.
	// Use a short /tmp path with our own cleanup.
	socketPath := shortSocketPath(t)
	lst, err := listen(socketPath)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer lst.Close()

	const W, H = 32, 24
	frame := bytes.Repeat([]byte{0x55}, W*H*3/2)

	// Send a one-frame stream over the socket, twice in sequence.
	send := func() {
		conn, err := net.Dial("unix", socketPath)
		if err != nil {
			t.Errorf("dial: %v", err)
			return
		}
		defer conn.Close()
		_, _ = conn.Write(wire.EncodeVideoInit(wire.VideoInit{Width: W, Height: H, FPS: 30}))
		_, _ = conn.Write(wire.EncodeFrame(0, frame))
		// Close the write side — handler reads to EOF and returns.
	}

	ctx, cancel := context.WithCancel(context.Background())
	runDone := make(chan error, 1)
	go func() {
		runDone <- run(ctx, config{
			mode: "video", socketPath: socketPath, device: "/dev/video10",
		}, lst, quietLogger())
	}()

	send()
	send()

	// Give the handler time to drain both connections.
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		vid.mu.Lock()
		n := len(vid.frames)
		vid.mu.Unlock()
		if n >= 2 {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	vid.mu.Lock()
	gotFrames := len(vid.frames)
	vid.mu.Unlock()
	if gotFrames != 2 {
		t.Errorf("got %d frames across 2 connections, want 2", gotFrames)
	}

	cancel()
	_ = lst.Close()
	select {
	case <-runDone:
	case <-time.After(2 * time.Second):
		t.Errorf("run() did not return after cancel + Close")
	}
}

// ---------------------------------------------------------------------------
// listen — error paths
// ---------------------------------------------------------------------------

// shortSocketPath returns an /tmp/cb-<rand>.sock path under macOS's
// 104-byte sockaddr_un.sun_path limit, with cleanup registered.
func shortSocketPath(t *testing.T) string {
	t.Helper()
	f, err := os.CreateTemp("/tmp", "cb-*.sock")
	if err != nil {
		t.Fatal(err)
	}
	path := f.Name()
	_ = f.Close()
	_ = os.Remove(path) // we want the path, not the file
	t.Cleanup(func() { _ = os.Remove(path) })
	return path
}

func TestListenUnlinksStaleSocket(t *testing.T) {
	sockPath := shortSocketPath(t)
	// Pre-create a stale file at sockPath; listen() must unlink
	// before calling net.Listen to avoid EADDRINUSE. We use a
	// regular file (not a real listener) to model the "previous
	// process crashed without unlinking" case.
	if err := os.WriteFile(sockPath, []byte("stale"), 0o644); err != nil {
		t.Fatal(err)
	}
	lst, err := listen(sockPath)
	if err != nil {
		t.Fatalf("listen with stale socket: %v", err)
	}
	defer lst.Close()
}

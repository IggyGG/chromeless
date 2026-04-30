package wire

import (
	"bytes"
	"errors"
	"io"
	"testing"
)

func TestVideoRoundTrip(t *testing.T) {
	init := VideoInit{Width: 1920, Height: 1080, FPS: 30}
	frame1 := bytes.Repeat([]byte{0x42}, 1024)
	frame2 := bytes.Repeat([]byte{0x99}, 7777)

	var buf bytes.Buffer
	buf.Write(EncodeVideoInit(init))
	buf.Write(EncodeFrame(100, frame1))
	buf.Write(EncodeFrame(133, frame2))

	r := NewVideoReader(&buf)
	gotInit, err := r.Init()
	if err != nil {
		t.Fatalf("Init: %v", err)
	}
	if gotInit != init {
		t.Errorf("Init mismatch: got %+v want %+v", gotInit, init)
	}

	work := make([]byte, 0, 4096)
	f1, work, err := r.Frame(work)
	if err != nil {
		t.Fatalf("Frame#1: %v", err)
	}
	if f1.PTSMillis != 100 || !bytes.Equal(f1.Body, frame1) {
		t.Errorf("frame#1 body mismatch")
	}

	f2, work, err := r.Frame(work)
	_ = work
	if err != nil {
		t.Fatalf("Frame#2: %v", err)
	}
	if f2.PTSMillis != 133 || !bytes.Equal(f2.Body, frame2) {
		t.Errorf("frame#2 body mismatch")
	}

	// Stream end → io.EOF.
	if _, _, err := r.Frame(work); !errors.Is(err, io.EOF) {
		t.Errorf("expected EOF at end, got %v", err)
	}
}

func TestAudioRoundTrip(t *testing.T) {
	init := AudioInit{SampleRate: 48000, Channels: 2}
	samples := []byte{0x00, 0x01, 0x02, 0x03, 0xff, 0xfe, 0xfd, 0xfc}

	var buf bytes.Buffer
	buf.Write(EncodeAudioInit(init))
	buf.Write(EncodeFrame(0, samples))

	r := NewAudioReader(&buf)
	gotInit, err := r.Init()
	if err != nil {
		t.Fatalf("Init: %v", err)
	}
	if gotInit != init {
		t.Errorf("Init mismatch: %+v", gotInit)
	}

	work := make([]byte, 0)
	f, _, err := r.Frame(work)
	if err != nil {
		t.Fatalf("Frame: %v", err)
	}
	if !bytes.Equal(f.Body, samples) {
		t.Errorf("samples mismatch")
	}
}

func TestBadMagic(t *testing.T) {
	// Audio header on a video reader → ErrBadMagic.
	var buf bytes.Buffer
	buf.Write(EncodeAudioInit(AudioInit{SampleRate: 48000, Channels: 2}))

	r := NewVideoReader(&buf)
	if _, err := r.Init(); !errors.Is(err, ErrBadMagic) {
		t.Errorf("expected ErrBadMagic, got %v", err)
	}
}

func TestFrameTooBig(t *testing.T) {
	// Hand-craft an oversize frame header — the parser MUST reject
	// before allocating gigabytes.
	var buf bytes.Buffer
	buf.Write(EncodeVideoInit(VideoInit{Width: 1, Height: 1, FPS: 1}))
	// 64 MiB declared; > MaxFrameSize.
	hdr := []byte{0x04, 0x00, 0x00, 0x00, 0, 0, 0, 0}
	buf.Write(hdr)
	r := NewVideoReader(&buf)
	if _, err := r.Init(); err != nil {
		t.Fatalf("Init: %v", err)
	}
	work := make([]byte, 0)
	if _, _, err := r.Frame(work); !errors.Is(err, ErrFrameTooBig) {
		t.Errorf("expected ErrFrameTooBig, got %v", err)
	}
}

func TestFrameBufferReuse(t *testing.T) {
	// Reuses dst across frames; the second call should grow if dst
	// is too small.
	init := VideoInit{Width: 64, Height: 48, FPS: 30}
	small := make([]byte, 10)
	big := make([]byte, 1024)
	for i := range small {
		small[i] = byte(i)
	}
	for i := range big {
		big[i] = byte(i & 0xff)
	}

	var buf bytes.Buffer
	buf.Write(EncodeVideoInit(init))
	buf.Write(EncodeFrame(0, small))
	buf.Write(EncodeFrame(0, big))

	r := NewVideoReader(&buf)
	if _, err := r.Init(); err != nil {
		t.Fatal(err)
	}
	work := make([]byte, 0, 4)
	f1, work, err := r.Frame(work)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(f1.Body, small) {
		t.Error("small frame mismatch")
	}
	f2, work, err := r.Frame(work)
	_ = work
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(f2.Body, big) {
		t.Error("big frame mismatch")
	}
}

func TestTruncatedStream(t *testing.T) {
	// Init header but no frame → first Frame returns io.EOF.
	// Truncated init → io.ErrUnexpectedEOF.
	var buf bytes.Buffer
	buf.Write(EncodeVideoInit(VideoInit{Width: 1, Height: 1, FPS: 1}))
	r := NewVideoReader(&buf)
	if _, err := r.Init(); err != nil {
		t.Fatal(err)
	}
	if _, _, err := r.Frame(make([]byte, 0)); !errors.Is(err, io.EOF) {
		t.Errorf("expected EOF on empty stream, got %v", err)
	}

	short := bytes.NewReader([]byte("CB")) // 2 of 16 init bytes
	rs := NewVideoReader(short)
	if _, err := rs.Init(); !errors.Is(err, io.ErrUnexpectedEOF) {
		t.Errorf("expected ErrUnexpectedEOF, got %v", err)
	}
}

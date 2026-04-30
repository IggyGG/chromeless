// Command v4l2-writer is the native helper that closes the
// webcam/mic passthrough loop from T81 by consuming the
// streamer-page → Unix socket frame stream and writing into:
//
//   --mode=video → a v4l2loopback device (e.g. /dev/video10) as
//                  I420 frames
//   --mode=audio → a PulseAudio sink (the null sink loaded by
//                  pulse-default.pa) as S16LE samples via pacat
//
// The two modes share the framing parser (internal/wire) but use
// different sinks; we run two instances per session pod (one
// each) as separate sidecar containers in the K8s manifest. The
// binary is small enough that the duplication is fine and keeps
// the failure domains independent — a wedged pulse daemon doesn't
// take down the video path.
//
// Connection model: listen on a Unix socket; on each accepted
// connection, parse the init header, open the appropriate sink,
// stream frames until EOF or error, close the sink, accept the
// next connection. This handles the "streamer page reload →
// fresh connection" path the T81 design calls out.
//
// See docs/protocols/webcam-mic-passthrough.md for the wire
// format and capture/v4l2-writer/README.md for run instructions.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/iggy/cloud-browser-webrtc/capture/v4l2-writer/internal/pulse"
	"github.com/iggy/cloud-browser-webrtc/capture/v4l2-writer/internal/v4l2"
	"github.com/iggy/cloud-browser-webrtc/capture/v4l2-writer/internal/wire"
)

type config struct {
	mode        string // "video" | "audio"
	socketPath  string // Unix socket to accept connections on
	device      string // /dev/videoN (video mode)
	pulseSink   string // PulseAudio sink name (audio mode)
}

func parseFlags(args []string) (config, error) {
	fs := flag.NewFlagSet("v4l2-writer", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	cfg := config{}
	fs.StringVar(&cfg.mode, "mode", "", "Operating mode: video | audio")
	fs.StringVar(&cfg.socketPath, "socket", "",
		"Unix socket path the streamer connects to "+
			"(e.g. /run/cb-passthrough/video.sock)")
	fs.StringVar(&cfg.device, "device", "/dev/video10",
		"v4l2loopback device path (--mode=video only)")
	fs.StringVar(&cfg.pulseSink, "pulse-sink", "cb_passthrough",
		"PulseAudio sink name (--mode=audio only)")
	if err := fs.Parse(args); err != nil {
		return cfg, err
	}
	if cfg.mode != "video" && cfg.mode != "audio" {
		return cfg, fmt.Errorf("invalid --mode %q (want video|audio)", cfg.mode)
	}
	if cfg.socketPath == "" {
		return cfg, fmt.Errorf("--socket is required")
	}
	return cfg, nil
}

// listener is the small surface the run loop uses to accept connections.
// Tests substitute a fake.
type listener interface {
	Accept() (net.Conn, error)
	Close() error
}

// listen creates a Unix socket at socketPath. We unlink first so a
// stale socket from a previous crashed process doesn't block bind.
func listen(socketPath string) (listener, error) {
	_ = os.Remove(socketPath)
	l, err := net.Listen("unix", socketPath)
	if err != nil {
		return nil, fmt.Errorf("listen %s: %w", socketPath, err)
	}
	// World-rw so the streamer (running as a different user inside
	// the chromium container) can connect. Per-pod isolation is the
	// security boundary here.
	if err := os.Chmod(socketPath, 0o666); err != nil {
		_ = l.Close()
		return nil, fmt.Errorf("chmod %s: %w", socketPath, err)
	}
	return l, nil
}

// run is the lifecycle entrypoint: open the socket, accept
// connections sequentially, dispatch to the per-mode handler.
// Returns when ctx is cancelled.
func run(ctx context.Context, cfg config, lst listener, log *slog.Logger) error {
	defer lst.Close()

	connCh := make(chan net.Conn)
	errCh := make(chan error, 1)
	go func() {
		for {
			conn, err := lst.Accept()
			if err != nil {
				errCh <- err
				return
			}
			connCh <- conn
		}
	}()

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case err := <-errCh:
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			return fmt.Errorf("accept: %w", err)
		case conn := <-connCh:
			log.Info("peer connected",
				slog.String("mode", cfg.mode),
				slog.String("remote", conn.RemoteAddr().String()))
			startTime := time.Now()
			frames, err := handleConnection(ctx, cfg, conn, log)
			conn.Close()
			log.Info("peer disconnected",
				slog.String("mode", cfg.mode),
				slog.Int("frames", frames),
				slog.Duration("duration", time.Since(startTime)),
				slog.Any("err", err))
		}
	}
}

// handleConnection serves one accepted connection — parse init,
// open sink, copy frames until EOF.
func handleConnection(ctx context.Context, cfg config, r io.Reader, log *slog.Logger) (int, error) {
	switch cfg.mode {
	case "video":
		return handleVideo(ctx, cfg, r, log)
	case "audio":
		return handleAudio(ctx, cfg, r, log)
	}
	return 0, fmt.Errorf("unreachable mode %q", cfg.mode)
}

// videoSink is the dispatch surface tests can substitute.
type videoOpener func(path string, fmt v4l2.VideoFormat) (v4l2.Writer, error)

var openVideo videoOpener = v4l2.Open

func handleVideo(_ context.Context, cfg config, r io.Reader, log *slog.Logger) (int, error) {
	vr := wire.NewVideoReader(r)
	init, err := vr.Init()
	if err != nil {
		return 0, fmt.Errorf("video init: %w", err)
	}
	log.Info("video init",
		slog.Int("width", init.Width),
		slog.Int("height", init.Height),
		slog.Int("fps", init.FPS))

	w, err := openVideo(cfg.device, v4l2.VideoFormat{
		Width: init.Width, Height: init.Height, FPS: init.FPS,
	})
	if err != nil {
		return 0, fmt.Errorf("open v4l2 %s: %w", cfg.device, err)
	}
	defer w.Close()

	frameCount := 0
	work := make([]byte, 0, init.Width*init.Height*3/2)
	for {
		frame, next, err := vr.Frame(work)
		work = next
		if err != nil {
			if errors.Is(err, io.EOF) {
				return frameCount, nil
			}
			return frameCount, fmt.Errorf("read frame %d: %w", frameCount, err)
		}
		if err := w.WriteFrame(frame.Body); err != nil {
			return frameCount, fmt.Errorf("write frame %d: %w", frameCount, err)
		}
		frameCount++
	}
}

// audioOpener is the dispatch surface tests can substitute.
type audioOpener func(sink string, fmt pulse.AudioFormat) (pulse.Writer, error)

var openAudio audioOpener = pulse.Open

func handleAudio(_ context.Context, cfg config, r io.Reader, log *slog.Logger) (int, error) {
	ar := wire.NewAudioReader(r)
	init, err := ar.Init()
	if err != nil {
		return 0, fmt.Errorf("audio init: %w", err)
	}
	log.Info("audio init",
		slog.Int("sample_rate", init.SampleRate),
		slog.Int("channels", init.Channels))

	w, err := openAudio(cfg.pulseSink, pulse.AudioFormat{
		SampleRate: init.SampleRate, Channels: init.Channels,
	})
	if err != nil {
		return 0, fmt.Errorf("open pulse %q: %w", cfg.pulseSink, err)
	}
	defer w.Close()

	frameCount := 0
	work := make([]byte, 0, 4096)
	for {
		frame, next, err := ar.Frame(work)
		work = next
		if err != nil {
			if errors.Is(err, io.EOF) {
				return frameCount, nil
			}
			return frameCount, fmt.Errorf("read frame %d: %w", frameCount, err)
		}
		if err := w.WriteSamples(frame.Body); err != nil {
			return frameCount, fmt.Errorf("write frame %d: %w", frameCount, err)
		}
		frameCount++
	}
}

func main() {
	cfg, err := parseFlags(os.Args[1:])
	if err != nil {
		fmt.Fprintln(os.Stderr, "flag error:", err)
		os.Exit(2)
	}
	log := slog.New(slog.NewJSONHandler(os.Stderr, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(log)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	lst, err := listen(cfg.socketPath)
	if err != nil {
		log.Error("listen failed", slog.Any("err", err))
		os.Exit(1)
	}
	log.Info("v4l2-writer listening",
		slog.String("mode", cfg.mode),
		slog.String("socket", cfg.socketPath))

	if err := run(ctx, cfg, lst, log); err != nil &&
		!errors.Is(err, context.Canceled) {
		log.Error("run failed", slog.Any("err", err))
		os.Exit(1)
	}
}

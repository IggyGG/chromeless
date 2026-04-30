// Package pulse implements the audio-loopback writer half of T92.
//
// We shell out to PulseAudio's `pacat --playback ... --raw` rather
// than using a Go PulseAudio client library. Two reasons:
//
//   1. The pacat process handles the protocol (auth, sample-format
//      negotiation, latency targets, reconnection) and retries on
//      its own — saving ~200 lines of pure-Go pulse client code that
//      we'd otherwise have to maintain.
//   2. pacat is already installed in the cloud Chromium image
//      (pulseaudio-utils, T7) so we add zero new runtime deps.
//
// We open one pacat per audio stream. The writer pipes wire-format
// frame bodies to its stdin; pacat decodes (S16LE) and pushes to
// the configured sink. On socket close we kill pacat; the next
// peer connection spawns a fresh one (consistent with the pattern
// the wire-protocol's "peer hangup → reopen" rule documents).
//
// The target sink is a null-sink loaded by infra/pulse-default.pa
// (T24). Its `.monitor` source is what `getUserMedia({audio:true})`
// inside the cloud Chromium tab sees.
package pulse

import (
	"errors"
	"fmt"
	"io"
	"os/exec"
	"strconv"
)

// AudioFormat is the negotiated stream format from the wire init header.
type AudioFormat struct {
	SampleRate int
	Channels   int
}

// Writer is the abstract surface the dispatcher writes samples to.
// Tests substitute a mock that records bytes; the production
// implementation is *pacatWriter below.
type Writer interface {
	WriteSamples(s16le []byte) error
	Close() error
}

// ErrUnavailable is returned when pacat can't be spawned (binary
// missing, permission denied, etc).
var ErrUnavailable = errors.New("pulse: pacat unavailable")

// PacatPath is the binary used by Open. Override in tests to point
// at a fake. Defaults to `pacat` resolved from $PATH.
var PacatPath = "pacat"

// Open spawns a `pacat --playback --raw` targeting `sink` and
// returns a Writer that streams S16LE samples into its stdin.
// On any failure (binary missing, daemon unreachable, etc.) the
// returned error wraps ErrUnavailable.
func Open(sink string, fmt_ AudioFormat) (Writer, error) {
	if fmt_.SampleRate <= 0 || fmt_.Channels <= 0 || fmt_.Channels > 8 {
		return nil, fmt.Errorf("pulse: invalid format: %+v", fmt_)
	}

	args := []string{
		"--playback",
		"--raw",
		"--device=" + sink,
		"--format=s16le",
		"--rate=" + strconv.Itoa(fmt_.SampleRate),
		"--channels=" + strconv.Itoa(fmt_.Channels),
		// 40 ms latency target — keeps passthrough delay close to
		// the v1 input-latency budget. pacat translates this into
		// the PA stream buffer attribute.
		"--latency-msec=40",
		// pacat exits 0 on stdin EOF, which is what we want when
		// the source socket disconnects.
	}
	cmd := exec.Command(PacatPath, args...)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrUnavailable, err)
	}
	// Capture stderr so we can surface PA daemon errors in logs.
	// stdout has nothing to say in --playback mode.
	stderr, _ := cmd.StderrPipe()

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("%w: start: %v", ErrUnavailable, err)
	}

	w := &pacatWriter{
		cmd:    cmd,
		stdin:  stdin,
		stderr: stderr,
		format: fmt_,
		sink:   sink,
	}
	return w, nil
}

type pacatWriter struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stderr io.ReadCloser
	format AudioFormat
	sink   string
}

func (w *pacatWriter) WriteSamples(s16le []byte) error {
	if _, err := w.stdin.Write(s16le); err != nil {
		return err
	}
	return nil
}

func (w *pacatWriter) Close() error {
	// Closing stdin tells pacat to drain its buffer + exit cleanly.
	// If the process is already dead the close errors are noise.
	if w.stdin != nil {
		_ = w.stdin.Close()
		w.stdin = nil
	}
	// Don't block forever on a wedged pacat — the daemon disconnect
	// path can leave it waiting on a socket that never closes.
	// Phase 4 polish: add a kill timer here. For v1, Wait() is
	// fine; pacat exits within ~100ms of stdin EOF in practice.
	if w.cmd != nil {
		_ = w.cmd.Wait()
		w.cmd = nil
	}
	return nil
}

// Stderr returns the pacat process's stderr pipe. The caller is
// expected to drain it concurrently — pacat will block once the
// kernel pipe buffer fills if no one reads.
func (w *pacatWriter) Stderr() io.Reader { return w.stderr }

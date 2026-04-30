// T72: handler for client-emitted StatsSample envelopes.
//
// The streamer page (capture/streamer-page) opens a relay between the
// WebRTC `"stats"` data channel and this sidecar. Each frame on the
// data channel is a StatsSample envelope (see docs/protocols/stats-channel.md).
// The streamer wraps it as `{v, t, sample, event?}` and POSTs to
// /stats-update; we update the cb_client_* metrics from it.
//
// What we don't do:
//   - Per-session labelling. v1 sees one streamer per container, so a
//     single set of gauges is fine. Phase 3 multi-session-per-container
//     would extend this with session_id labels (and would have to stay
//     under the same cardinality discipline as signaling/metrics.go's
//     tenant cap).
//   - Buffering. The relay is best-effort: if the sidecar is down,
//     the streamer drops the sample and logs.

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"sync"
	"time"
)

// statsEnvelope mirrors the wire format from docs/protocols/stats-channel.md.
type statsEnvelope struct {
	V      int             `json:"v"`
	T      int64           `json:"t"`
	Sample *clientSample   `json:"sample,omitempty"`
	Event  string          `json:"event,omitempty"` // T54 codec_fallback rides this same channel
	Data   json.RawMessage `json:"data,omitempty"`
}

type clientInboundStats struct {
	TrackID         string  `json:"trackId"`
	Kind            string  `json:"kind"`
	BytesReceived   uint64  `json:"bytesReceived"`
	PacketsReceived uint64  `json:"packetsReceived"`
	PacketsLost     uint64  `json:"packetsLost"`
	Jitter          float64 `json:"jitter"`
	FramesPerSecond *float64 `json:"framesPerSecond"`
	FramesDropped   *uint64  `json:"framesDropped"`
	FramesReceived  *uint64  `json:"framesReceived"`
	TotalDecodeTime *float64 `json:"totalDecodeTime"`
}

type clientRemoteInboundStats struct {
	TrackID       string   `json:"trackId"`
	Kind          string   `json:"kind"`
	RoundTripTime *float64 `json:"roundTripTime"`
	PacketsLost   *float64 `json:"packetsLost"`
	FractionLost  *float64 `json:"fractionLost"`
}

type clientCandidatePair struct {
	CurrentRoundTripTime *float64 `json:"currentRoundTripTime"`
	AvailableOutgoing    *float64 `json:"availableOutgoingBitrate"`
	AvailableIncoming    *float64 `json:"availableIncomingBitrate"`
	LocalCandidateType   *string  `json:"localCandidateType"`
	RemoteCandidateType  *string  `json:"remoteCandidateType"`
}

type clientSample struct {
	V             int                       `json:"v"`
	T             int64                     `json:"t"`
	Inbound       []clientInboundStats      `json:"inbound"`
	Outbound      []json.RawMessage         `json:"outbound"`
	RemoteInbound []clientRemoteInboundStats `json:"remoteInbound"`
	CandidatePair *clientCandidatePair      `json:"candidatePair"`
}

// clientStatsState is analogous to statsState but for inbound-from-server
// metrics seen by the client. We keep prevBytesReceived to compute the
// bitrate gauge from byte deltas, and prevFramesDropped for counter Add.
type clientStatsState struct {
	mu                 sync.Mutex
	prevBytesReceived  uint64
	prevFramesDropped  uint64
	prevSampleAt       time.Time
	haveBytes          bool
	haveFrames         bool
}

func newClientStatsState() *clientStatsState {
	return &clientStatsState{}
}

// reset clears accumulators when the client's session goes away — same
// reasoning as statsState.reset on the streamer side. Public so the
// poll loop or admin handler could call it later.
func (s *clientStatsState) reset() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.prevBytesReceived = 0
	s.prevFramesDropped = 0
	s.prevSampleAt = time.Time{}
	s.haveBytes = false
	s.haveFrames = false
}

// applySample updates the cb_client_* metrics from a single envelope.
// Returns the sample timestamp on success (echoed in the response for
// the streamer's debug log) or an error if the envelope is malformed.
func (s *clientStatsState) applySample(env *statsEnvelope, log *slog.Logger) error {
	if env == nil {
		return errors.New("nil envelope")
	}
	if env.V != 1 {
		return fmt.Errorf("unsupported envelope version %d", env.V)
	}
	if env.Sample == nil {
		// Could be a non-sample event (e.g., codec_fallback). Accept
		// silently — we don't translate those into metrics yet.
		if env.Event != "" {
			log.Info("client event (no metric update)", slog.String("event", env.Event), slog.Any("data", env.Data))
			return nil
		}
		return errors.New("envelope has neither sample nor event")
	}
	sample := env.Sample
	now := time.Unix(0, env.T*int64(time.Millisecond))

	// Pick the first video inbound stat; v1 streams a single video.
	var video *clientInboundStats
	for i := range sample.Inbound {
		if sample.Inbound[i].Kind == "video" {
			video = &sample.Inbound[i]
			break
		}
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	if video != nil {
		// Bitrate from byte delta.
		if s.haveBytes && !s.prevSampleAt.IsZero() {
			dt := now.Sub(s.prevSampleAt).Seconds()
			if dt > 0 && video.BytesReceived >= s.prevBytesReceived {
				bps := float64(video.BytesReceived-s.prevBytesReceived) * 8 / dt
				mClientInboundBitrate.Set(bps)
			}
		}
		s.prevBytesReceived = video.BytesReceived
		s.haveBytes = true

		// FPS — set directly.
		if video.FramesPerSecond != nil {
			mClientInboundFPS.Set(*video.FramesPerSecond)
		}

		// Frames dropped — counter delta.
		if video.FramesDropped != nil {
			cur := *video.FramesDropped
			if s.haveFrames && cur >= s.prevFramesDropped {
				if delta := cur - s.prevFramesDropped; delta > 0 {
					mClientInboundFramesDropped.Add(float64(delta))
				}
			}
			s.prevFramesDropped = cur
			s.haveFrames = true
		}
	}

	// RTT from selected candidate-pair (seconds → ms).
	if sample.CandidatePair != nil && sample.CandidatePair.CurrentRoundTripTime != nil {
		mClientPairRTT.Set(*sample.CandidatePair.CurrentRoundTripTime * 1000)
	}

	// Loss fraction from the first remote-inbound video stat.
	for _, ri := range sample.RemoteInbound {
		if ri.Kind == "video" && ri.FractionLost != nil {
			mClientRemoteInboundLossFraction.Set(*ri.FractionLost)
			break
		}
	}

	s.prevSampleAt = now
	return nil
}

// statsUpdateHandler returns the http.HandlerFunc to register at
// /stats-update. The closure owns the per-process clientStatsState so
// counter deltas survive across requests.
func statsUpdateHandler(log *slog.Logger) (http.HandlerFunc, *clientStatsState) {
	state := newClientStatsState()
	const maxBody = 256 * 1024 // 256 KiB; one StatsSample is ~3 KiB.
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", http.MethodPost)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		body, err := io.ReadAll(io.LimitReader(r.Body, maxBody+1))
		if err != nil {
			http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
			return
		}
		if len(body) > maxBody {
			http.Error(w, "body too large", http.StatusRequestEntityTooLarge)
			return
		}
		var env statsEnvelope
		if err := json.Unmarshal(body, &env); err != nil {
			http.Error(w, "decode: "+err.Error(), http.StatusBadRequest)
			return
		}
		if err := state.applySample(&env, log); err != nil {
			http.Error(w, "apply: "+err.Error(), http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}, state
}

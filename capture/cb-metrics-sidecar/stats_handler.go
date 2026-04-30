// T72: handler for client-emitted StatsSample envelopes.
// T82: extended to label cb_client_* metrics with (tenant_id, session_id)
// extracted from the v1.1 envelope, with the same 100-distinct-tenants +
// 100-distinct-sessions cardinality cap pattern as signaling/metrics.go
// (T67). Anonymous + "_other" buckets are exempt.
//
// The streamer page (capture/streamer-page) opens a relay between the
// WebRTC `"stats"` data channel and this sidecar. Each frame on the
// data channel is a v1.1 envelope (see docs/protocols/stats-channel.md).
// The streamer wraps the client's StatsSample as
// `{v, t, session_id, tenant_id, sample, event?}` and POSTs to
// /stats-update; we update the cb_client_* metrics from it.
//
// Per-session state: bitrate computation needs prevBytesReceived
// across consecutive samples *for the same session*. We keep a
// `stateBySession` map keyed by the bucketed (tenant, session)
// pair, so two parallel sessions don't poison each other's deltas.

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
	V         int             `json:"v"`
	T         int64           `json:"t"`
	SessionID string          `json:"session_id,omitempty"` // T82 v1.1
	TenantID  string          `json:"tenant_id,omitempty"`  // T82 v1.1
	Sample    *clientSample   `json:"sample,omitempty"`
	Event     string          `json:"event,omitempty"` // T54 codec_fallback rides this same channel
	Data      json.RawMessage `json:"data,omitempty"`
}

type clientInboundStats struct {
	TrackID         string   `json:"trackId"`
	Kind            string   `json:"kind"`
	BytesReceived   uint64   `json:"bytesReceived"`
	PacketsReceived uint64   `json:"packetsReceived"`
	PacketsLost     uint64   `json:"packetsLost"`
	Jitter          float64  `json:"jitter"`
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
	V             int                        `json:"v"`
	T             int64                      `json:"t"`
	Inbound       []clientInboundStats       `json:"inbound"`
	Outbound      []json.RawMessage          `json:"outbound"`
	RemoteInbound []clientRemoteInboundStats `json:"remoteInbound"`
	CandidatePair *clientCandidatePair       `json:"candidatePair"`
}

// ---------------------------------------------------------------------------
// T82: cardinality-bounded label helpers.
// ---------------------------------------------------------------------------

const (
	clientAnonymousLabel = "_anonymous"
	clientOverflowLabel  = "_other"
	clientLabelCap       = 100
)

type labelBucket struct {
	mu   sync.RWMutex
	seen map[string]struct{}
}

func newLabelBucket() *labelBucket {
	return &labelBucket{seen: make(map[string]struct{}, clientLabelCap)}
}

// label returns the canonical label value for raw `v`:
//   - `clientAnonymousLabel` for empty / explicitly-anonymous values.
//   - the raw value if it's already in the seen set, or the cap has room.
//   - `clientOverflowLabel` if the cap is full and the value is unseen.
func (b *labelBucket) label(v string) string {
	if v == "" || v == clientAnonymousLabel {
		return clientAnonymousLabel
	}
	b.mu.RLock()
	if _, ok := b.seen[v]; ok {
		b.mu.RUnlock()
		return v
	}
	b.mu.RUnlock()
	b.mu.Lock()
	defer b.mu.Unlock()
	if _, ok := b.seen[v]; ok {
		return v
	}
	if len(b.seen) >= clientLabelCap {
		return clientOverflowLabel
	}
	b.seen[v] = struct{}{}
	return v
}

// resetForTest is used by tests to reset bucket state between cases.
func (b *labelBucket) resetForTest() {
	b.mu.Lock()
	b.seen = make(map[string]struct{}, clientLabelCap)
	b.mu.Unlock()
}

// ---------------------------------------------------------------------------
// per-(tenant, session) state for delta computation.
// ---------------------------------------------------------------------------

type stateKey struct {
	tenant  string
	session string
}

// clientStatsState carries the previous-iteration values needed to
// compute monotonic counter deltas (bytesReceived, framesDropped) for
// a single (tenant, session) pair.
type clientStatsState struct {
	prevBytesReceived uint64
	prevFramesDropped uint64
	prevSampleAt      time.Time
	haveBytes         bool
	haveFrames        bool
}

// statsRouter owns the per-session state map + the cardinality buckets.
// One per sidecar process.
type statsRouter struct {
	mu             sync.Mutex
	stateBySession map[stateKey]*clientStatsState

	tenantBucket  *labelBucket
	sessionBucket *labelBucket
}

func newStatsRouter() *statsRouter {
	return &statsRouter{
		stateBySession: make(map[stateKey]*clientStatsState),
		tenantBucket:   newLabelBucket(),
		sessionBucket:  newLabelBucket(),
	}
}

// labelsFor returns (tenant, session) labels with cardinality bounding
// applied. Empty inputs collapse to "_anonymous" and never consume cap.
func (r *statsRouter) labelsFor(env *statsEnvelope) (tenant, session string) {
	tenant = r.tenantBucket.label(env.TenantID)
	session = r.sessionBucket.label(env.SessionID)
	return tenant, session
}

// stateFor returns the per-session state, creating it if absent.
func (r *statsRouter) stateFor(tenant, session string) *clientStatsState {
	k := stateKey{tenant: tenant, session: session}
	r.mu.Lock()
	defer r.mu.Unlock()
	s, ok := r.stateBySession[k]
	if !ok {
		s = &clientStatsState{}
		r.stateBySession[k] = s
	}
	return s
}

// applySample updates the cb_client_* metrics from a single envelope.
// Returns nil on success or an error if the envelope is malformed.
func (r *statsRouter) applySample(env *statsEnvelope, log *slog.Logger) error {
	if env == nil {
		return errors.New("nil envelope")
	}
	if env.V != 1 {
		return fmt.Errorf("unsupported envelope version %d", env.V)
	}

	tenant, session := r.labelsFor(env)

	if env.Sample == nil {
		// Could be a non-sample event (e.g., codec_fallback). Accept
		// silently — we don't translate those into metrics yet.
		if env.Event != "" {
			log.Info("client event (no metric update)",
				slog.String("event", env.Event),
				slog.String("tenant", tenant),
				slog.String("session", session),
				slog.Any("data", env.Data),
			)
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

	st := r.stateFor(tenant, session)
	r.mu.Lock()
	defer r.mu.Unlock()

	if video != nil {
		// Bitrate from byte delta.
		if st.haveBytes && !st.prevSampleAt.IsZero() {
			dt := now.Sub(st.prevSampleAt).Seconds()
			if dt > 0 && video.BytesReceived >= st.prevBytesReceived {
				bps := float64(video.BytesReceived-st.prevBytesReceived) * 8 / dt
				mClientInboundBitrate.WithLabelValues(tenant, session).Set(bps)
			}
		}
		st.prevBytesReceived = video.BytesReceived
		st.haveBytes = true

		// FPS — set directly.
		if video.FramesPerSecond != nil {
			mClientInboundFPS.WithLabelValues(tenant, session).Set(*video.FramesPerSecond)
		}

		// Frames dropped — counter delta.
		if video.FramesDropped != nil {
			cur := *video.FramesDropped
			if st.haveFrames && cur >= st.prevFramesDropped {
				if delta := cur - st.prevFramesDropped; delta > 0 {
					mClientInboundFramesDropped.WithLabelValues(tenant, session).Add(float64(delta))
				}
			}
			st.prevFramesDropped = cur
			st.haveFrames = true
		}
	}

	// RTT from selected candidate-pair (seconds → ms).
	if sample.CandidatePair != nil && sample.CandidatePair.CurrentRoundTripTime != nil {
		mClientPairRTT.WithLabelValues(tenant, session).
			Set(*sample.CandidatePair.CurrentRoundTripTime * 1000)
	}

	// Loss fraction from the first remote-inbound video stat.
	for _, ri := range sample.RemoteInbound {
		if ri.Kind == "video" && ri.FractionLost != nil {
			mClientRemoteInboundLossFraction.WithLabelValues(tenant, session).
				Set(*ri.FractionLost)
			break
		}
	}

	st.prevSampleAt = now
	return nil
}

// statsUpdateHandler returns the http.HandlerFunc to register at
// /stats-update. The closure owns the per-process router so per-session
// counter deltas + cardinality buckets survive across requests.
func statsUpdateHandler(log *slog.Logger) (http.HandlerFunc, *statsRouter) {
	router := newStatsRouter()
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
		if err := router.applySample(&env, log); err != nil {
			http.Error(w, "apply: "+err.Error(), http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}, router
}

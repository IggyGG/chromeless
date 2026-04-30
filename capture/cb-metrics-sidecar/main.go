// Package main is the cb-metrics-sidecar — a small Go service that
// runs alongside Chromium inside the cloud-browser-webrtc container,
// polls Chromium for performance and WebRTC stats, and exposes them on
// /metrics in Prometheus exposition format.
//
// T38: this is the chromium-side half of the per-container metrics
// scaffolding. The signaling-side half lives in signaling/metrics.go.
//
// Why a separate process instead of patching Chromium itself?
//   - We never want to ship a custom Chromium just for metrics; the
//     container baseline must work against the stable Debian package.
//   - The streamer page (T23) already exposes window.pc, so we can
//     reach getStats() data via Runtime.evaluate over DevTools without
//     a single custom Chromium API.
//   - A separate process means metrics survive a Chromium restart by
//     supervisord; we just see gauges drop to zero until DevTools is
//     reachable again.
//
// Metrics surfaced (see infra/observability.md for the rationale):
//
//	cb_chromium_cpu_pct                            gauge
//	cb_chromium_rss_bytes                          gauge
//	cb_webrtc_outbound_bitrate_bps{kind}           gauge
//	cb_webrtc_outbound_frames_per_second           gauge
//	cb_webrtc_outbound_dropped_frames_total        counter
//	cb_webrtc_outbound_qp                          gauge
//	cb_webrtc_remote_inbound_packets_lost_total    counter
//	cb_webrtc_round_trip_time_ms                   gauge
//
// Configuration is via env (see flag definitions in main()).
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// ---------------------------------------------------------------------------
// metrics
// ---------------------------------------------------------------------------

var (
	mCPU = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "cb_chromium_cpu_pct",
		Help: "Aggregate Chromium CPU usage as a percent of one core (so 100 == one fully busy core; 200 == two cores etc.).",
	})
	mRSS = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "cb_chromium_rss_bytes",
		Help: "Aggregate resident set size of all Chromium processes, in bytes.",
	})

	mOutboundBitrate = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_webrtc_outbound_bitrate_bps",
		Help: "Outbound RTP bitrate in bits per second, computed as a delta over the polling interval.",
	}, []string{"kind"})
	mOutboundFPS = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "cb_webrtc_outbound_frames_per_second",
		Help: "Outbound video frames per second, as reported by the encoder.",
	})
	mOutboundDropped = promauto.NewCounter(prometheus.CounterOpts{
		Name: "cb_webrtc_outbound_dropped_frames_total",
		Help: "Total frames dropped by the outbound video pipeline.",
	})
	mOutboundQP = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "cb_webrtc_outbound_qp",
		Help: "Average video encoder quantization parameter (lower = higher quality).",
	})
	mInboundLost = promauto.NewCounter(prometheus.CounterOpts{
		Name: "cb_webrtc_remote_inbound_packets_lost_total",
		Help: "Packets the remote peer reported as lost on the inbound side.",
	})
	mRTT = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "cb_webrtc_round_trip_time_ms",
		Help: "Selected ICE candidate-pair round-trip time in milliseconds.",
	})

	// ---- T72: client-side stats forwarded over the WebRTC "stats" data
	// channel (T42 protocol) and relayed by streamer.js to /stats-update.
	// These are what the user's browser actually receives, and they sit
	// next to the streamer-side metrics above so dashboards (T66) can
	// graph "what we sent" vs. "what they got."
	// T82 added (tenant_id, session_id) labels so per-session
	// drilldowns in T66's cb-session-detail dashboard actually
	// populate. Cardinality is bounded by labelTenant/labelSession
	// (top-100 each, rest bucketed as "_other"; "_anonymous" exempt).
	mClientInboundBitrate = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_client_inbound_video_bitrate_bps",
		Help: "Inbound video bitrate as observed at the client, in bits per second.",
	}, []string{"tenant_id", "session_id"})
	mClientInboundFPS = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_client_inbound_video_fps",
		Help: "Inbound video frames per second as observed at the client.",
	}, []string{"tenant_id", "session_id"})
	mClientInboundFramesDropped = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_client_inbound_video_frames_dropped_total",
		Help: "Total inbound video frames dropped, observed at the client.",
	}, []string{"tenant_id", "session_id"})
	mClientPairRTT = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_client_pair_rtt_ms",
		Help: "Selected ICE candidate-pair RTT as observed at the client, in milliseconds.",
	}, []string{"tenant_id", "session_id"})
	mClientRemoteInboundLossFraction = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_client_remote_inbound_packet_loss_fraction",
		Help: "Fraction of packets reported lost on the client's remote-inbound report (0..1).",
	}, []string{"tenant_id", "session_id"})
)

// ---------------------------------------------------------------------------
// /proc — chromium CPU + RSS aggregation
// ---------------------------------------------------------------------------

// procReader aggregates CPU% and RSS across all currently-running
// processes whose /proc/PID/comm matches one of the chromium binary
// names. Linux clock-tick semantics: utime+stime are in CLK_TCK units;
// CLK_TCK is 100 on every Linux kernel built in the last decade. We
// hardcode it to keep the sidecar dep-free.
type procReader struct {
	procRoot string

	// previous total CPU ticks across matched processes, for delta.
	prevTicks uint64
	prevTime  time.Time
}

const clkTck = 100.0

func (pr *procReader) sample() (cpuPct float64, rssBytes uint64, err error) {
	entries, err := os.ReadDir(pr.procRoot)
	if err != nil {
		return 0, 0, err
	}

	var totalTicks uint64
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		pid, perr := strconv.Atoi(e.Name())
		if perr != nil {
			continue
		}

		commPath := filepath.Join(pr.procRoot, strconv.Itoa(pid), "comm")
		commBytes, rerr := os.ReadFile(commPath)
		if rerr != nil {
			continue // process may have exited; skip silently
		}
		comm := strings.TrimSpace(string(commBytes))
		if !isChromiumComm(comm) {
			continue
		}

		ticks, rss, perr := readStat(filepath.Join(pr.procRoot, strconv.Itoa(pid), "stat"))
		if perr != nil {
			continue
		}
		totalTicks += ticks
		rssBytes += rss
	}

	now := time.Now()
	if pr.prevTime.IsZero() {
		pr.prevTicks = totalTicks
		pr.prevTime = now
		return 0, rssBytes, nil // first sample: no delta yet
	}

	deltaTicks := float64(totalTicks - pr.prevTicks)
	deltaSecs := now.Sub(pr.prevTime).Seconds()
	if deltaSecs <= 0 {
		return 0, rssBytes, nil
	}
	cpuPct = (deltaTicks / clkTck) / deltaSecs * 100.0

	pr.prevTicks = totalTicks
	pr.prevTime = now
	return cpuPct, rssBytes, nil
}

// isChromiumComm returns true for /proc/PID/comm values that look like
// part of the Chromium process tree as Debian packages it.
func isChromiumComm(comm string) bool {
	switch comm {
	case "chromium", "chrome", "chromium-browse":
		return true
	}
	// Helper / utility / zygote / etc. processes: chromium spawns these
	// with the same comm in v1 (they share the binary name); some
	// versions reuse a "chrome" comm for all helpers. Match defensively.
	return strings.HasPrefix(comm, "chrome")
}

// readStat reads /proc/PID/stat and returns (utime+stime in CLK_TCK
// ticks, rss in bytes). The /proc(5) format puts (comm) in parens which
// can itself contain spaces, so we slice from the last ')' rather than
// fields[1].
func readStat(path string) (ticks uint64, rss uint64, err error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, 0, err
	}
	end := strings.LastIndexByte(string(data), ')')
	if end < 0 || end+2 >= len(data) {
		return 0, 0, errors.New("malformed stat")
	}
	fields := strings.Fields(string(data[end+2:]))
	// fields are 0-indexed against post-comm columns:
	//   13 utime        (proc(5) col 14)
	//   14 stime        (proc(5) col 15)
	//   21 rss in pages (proc(5) col 24)
	if len(fields) < 22 {
		return 0, 0, fmt.Errorf("stat has only %d fields", len(fields))
	}
	utime, err := strconv.ParseUint(fields[11], 10, 64)
	if err != nil {
		return 0, 0, err
	}
	stime, err := strconv.ParseUint(fields[12], 10, 64)
	if err != nil {
		return 0, 0, err
	}
	rssPages, err := strconv.ParseUint(fields[21], 10, 64)
	if err != nil {
		return 0, 0, err
	}
	pageSize := uint64(os.Getpagesize())
	return utime + stime, rssPages * pageSize, nil
}

// ---------------------------------------------------------------------------
// DevTools client — find streamer page, evaluate getStats() on it
// ---------------------------------------------------------------------------

type devtoolsClient struct {
	baseURL string
	log     *slog.Logger
}

type cdpTarget struct {
	Type                 string `json:"type"`
	Title                string `json:"title"`
	URL                  string `json:"url"`
	WebSocketDebuggerURL string `json:"webSocketDebuggerUrl"`
}

// findStreamerWS returns the websocketDebuggerUrl of the streamer page,
// or an empty string if no such page is currently loaded.
func (d *devtoolsClient) findStreamerWS(ctx context.Context) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, d.baseURL+"/json", nil)
	if err != nil {
		return "", err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", err
	}
	var targets []cdpTarget
	if err := json.Unmarshal(body, &targets); err != nil {
		return "", err
	}
	for _, t := range targets {
		if t.Type != "page" {
			continue
		}
		if !strings.Contains(strings.ToLower(t.Title), "streamer") {
			continue
		}
		return t.WebSocketDebuggerURL, nil
	}
	return "", nil
}

// evaluateGetStats opens a fresh DevTools websocket to the streamer
// page, invokes window.pc.getStats(), and returns the deserialized
// stats array (RTCStatsReport flattened to a JSON array).
func (d *devtoolsClient) evaluateGetStats(ctx context.Context, wsURL string) ([]map[string]any, error) {
	dialer := websocket.Dialer{HandshakeTimeout: 5 * time.Second}
	c, _, err := dialer.DialContext(ctx, wsURL, nil)
	if err != nil {
		return nil, fmt.Errorf("cdp dial: %w", err)
	}
	defer c.Close()

	const expr = `(async () => {
		if (typeof window.pc === 'undefined' || window.pc === null) return null;
		const stats = await window.pc.getStats();
		const out = [];
		stats.forEach(s => out.push(s));
		return out;
	})()`

	req := map[string]any{
		"id":     1,
		"method": "Runtime.evaluate",
		"params": map[string]any{
			"expression":    expr,
			"awaitPromise":  true,
			"returnByValue": true,
		},
	}
	if err := c.WriteJSON(req); err != nil {
		return nil, fmt.Errorf("cdp write: %w", err)
	}

	deadline, ok := ctx.Deadline()
	if ok {
		_ = c.SetReadDeadline(deadline)
	} else {
		_ = c.SetReadDeadline(time.Now().Add(5 * time.Second))
	}

	for {
		var resp struct {
			ID     int             `json:"id"`
			Result json.RawMessage `json:"result,omitempty"`
			Error  json.RawMessage `json:"error,omitempty"`
		}
		if err := c.ReadJSON(&resp); err != nil {
			return nil, fmt.Errorf("cdp read: %w", err)
		}
		if resp.ID != 1 {
			continue // event/notification, skip
		}
		if len(resp.Error) > 0 {
			return nil, fmt.Errorf("cdp error: %s", string(resp.Error))
		}
		var result struct {
			Result struct {
				Value []map[string]any `json:"value"`
			} `json:"result"`
		}
		if err := json.Unmarshal(resp.Result, &result); err != nil {
			return nil, fmt.Errorf("cdp decode result: %w", err)
		}
		return result.Result.Value, nil
	}
}

// ---------------------------------------------------------------------------
// stats parsing — turn an RTCStatsReport array into metric updates
// ---------------------------------------------------------------------------

// statsState carries the previous-iteration values needed to compute
// monotonic counter deltas (bytesSent, framesDropped, packetsLost) and
// to expose those deltas as Prometheus metrics.
type statsState struct {
	prevBytesSent      map[string]uint64
	prevFramesDropped  uint64
	prevPacketsLost    uint64
	haveBytesPrev      bool
	haveFramesPrev     bool
	havePacketsPrev    bool
	prevSampleAt       time.Time
}

func newStatsState() *statsState {
	return &statsState{prevBytesSent: map[string]uint64{}}
}

// reset clears all accumulators. Called when we lose contact with the
// streamer page so we don't compute a wild delta when it comes back.
func (st *statsState) reset() {
	st.prevBytesSent = map[string]uint64{}
	st.haveBytesPrev = false
	st.haveFramesPrev = false
	st.havePacketsPrev = false
	st.prevSampleAt = time.Time{}
}

func (st *statsState) update(stats []map[string]any, log *slog.Logger) {
	now := time.Now()

	// Aggregate by kind (video/audio) for outbound-rtp; pick the last
	// remote-inbound-rtp and selected candidate-pair seen.
	curBytesSent := map[string]uint64{}
	var fps, qp float64
	var qpHave bool
	var framesDropped uint64
	var packetsLost uint64
	var rttSeconds float64
	var rttHave bool

	for _, s := range stats {
		typ, _ := s["type"].(string)
		switch typ {
		case "outbound-rtp":
			kind, _ := s["kind"].(string)
			if kind == "" {
				kind = "video" // legacy schemas; default to video
			}
			if v, ok := numU64(s["bytesSent"]); ok {
				curBytesSent[kind] += v
			}
			if kind == "video" {
				if v, ok := numF64(s["framesPerSecond"]); ok {
					fps = v
				}
				if v, ok := numU64(s["framesDropped"]); ok {
					framesDropped = v
				}
				if v, ok := numF64(s["qpSum"]); ok {
					if fe, ok := numF64(s["framesEncoded"]); ok && fe > 0 {
						qp = v / fe
						qpHave = true
					}
				}
			}

		case "remote-inbound-rtp":
			if v, ok := numU64(s["packetsLost"]); ok {
				packetsLost += v
			}

		case "candidate-pair":
			// Selected pair has nominated=true and state=succeeded.
			nominated, _ := s["nominated"].(bool)
			state, _ := s["state"].(string)
			if !nominated || state != "succeeded" {
				continue
			}
			if v, ok := numF64(s["currentRoundTripTime"]); ok {
				rttSeconds = v
				rttHave = true
			}
		}
	}

	// --- bitrate (delta) ---
	if st.haveBytesPrev && !st.prevSampleAt.IsZero() {
		dt := now.Sub(st.prevSampleAt).Seconds()
		if dt > 0 {
			for kind, cur := range curBytesSent {
				prev := st.prevBytesSent[kind]
				if cur >= prev {
					bps := float64(cur-prev) * 8 / dt
					mOutboundBitrate.WithLabelValues(kind).Set(bps)
				}
			}
			// kinds that disappeared: zero them so we don't lie
			for kind := range st.prevBytesSent {
				if _, ok := curBytesSent[kind]; !ok {
					mOutboundBitrate.WithLabelValues(kind).Set(0)
				}
			}
		}
	}
	st.prevBytesSent = curBytesSent
	st.haveBytesPrev = true
	st.prevSampleAt = now

	// --- per-iteration gauges ---
	mOutboundFPS.Set(fps)
	if qpHave {
		mOutboundQP.Set(qp)
	}
	if rttHave {
		mRTT.Set(rttSeconds * 1000)
	}

	// --- counters: only Add the delta; getStats counters are monotonic ---
	if st.haveFramesPrev {
		if framesDropped >= st.prevFramesDropped {
			delta := framesDropped - st.prevFramesDropped
			if delta > 0 {
				mOutboundDropped.Add(float64(delta))
			}
		}
	}
	st.prevFramesDropped = framesDropped
	st.haveFramesPrev = true

	if st.havePacketsPrev {
		if packetsLost >= st.prevPacketsLost {
			delta := packetsLost - st.prevPacketsLost
			if delta > 0 {
				mInboundLost.Add(float64(delta))
			}
		}
	}
	st.prevPacketsLost = packetsLost
	st.havePacketsPrev = true

	log.Debug("stats updated",
		slog.Any("bytes_sent", curBytesSent),
		slog.Float64("fps", fps),
		slog.Float64("qp", qp),
		slog.Bool("qp_have", qpHave),
		slog.Uint64("frames_dropped", framesDropped),
		slog.Uint64("packets_lost", packetsLost),
		slog.Float64("rtt_seconds", rttSeconds),
	)
}

func numU64(v any) (uint64, bool) {
	switch n := v.(type) {
	case float64:
		if n < 0 {
			return 0, false
		}
		return uint64(n), true
	case json.Number:
		if u, err := strconv.ParseUint(n.String(), 10, 64); err == nil {
			return u, true
		}
	}
	return 0, false
}

func numF64(v any) (float64, bool) {
	switch n := v.(type) {
	case float64:
		return n, true
	case json.Number:
		if f, err := n.Float64(); err == nil {
			return f, true
		}
	}
	return 0, false
}

// ---------------------------------------------------------------------------
// poll loop
// ---------------------------------------------------------------------------

func runPoller(ctx context.Context, dt *devtoolsClient, pr *procReader, interval time.Duration, log *slog.Logger) {
	tick := time.NewTicker(interval)
	defer tick.Stop()

	st := newStatsState()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
		}

		// 1. CPU + RSS — cheap; do unconditionally.
		cpu, rss, err := pr.sample()
		if err != nil {
			log.Warn("proc sample failed", slog.Any("err", err))
		} else {
			mCPU.Set(cpu)
			mRSS.Set(float64(rss))
		}

		// 2. WebRTC stats — DevTools roundtrip; tolerate failures.
		stCtx, cancel := context.WithTimeout(ctx, interval/2)
		wsURL, err := dt.findStreamerWS(stCtx)
		if err != nil || wsURL == "" {
			cancel()
			if err != nil {
				log.Debug("devtools unavailable", slog.Any("err", err))
			}
			st.reset()
			zeroWebRTCGauges()
			continue
		}
		stats, err := dt.evaluateGetStats(stCtx, wsURL)
		cancel()
		if err != nil {
			log.Warn("getStats failed", slog.Any("err", err))
			st.reset()
			zeroWebRTCGauges()
			continue
		}
		if stats == nil {
			// streamer page loaded but window.pc isn't there yet
			zeroWebRTCGauges()
			continue
		}
		st.update(stats, log)
	}
}

func zeroWebRTCGauges() {
	mOutboundFPS.Set(0)
	mOutboundQP.Set(0)
	mRTT.Set(0)
	// Bitrate gauges for video/audio: leave as last-set; gauges are
	// noisy across "session went away" boundaries but the counters
	// (dropped frames, packets lost) keep their own pace so the
	// dashboards aren't misleading.
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	var (
		listen      string
		devtoolsURL string
		intervalStr string
		procRoot    string
		logLevel    string
	)
	flag.StringVar(&listen, "listen", envOr("METRICS_LISTEN", ":9100"), "address to expose /metrics on")
	flag.StringVar(&devtoolsURL, "devtools-url", envOr("DEVTOOLS_URL", "http://127.0.0.1:9222"), "Chromium DevTools base URL")
	flag.StringVar(&intervalStr, "poll-interval", envOr("POLL_INTERVAL_S", "10s"), "polling interval (Go duration string or seconds)")
	flag.StringVar(&procRoot, "proc-root", envOr("PROC_ROOT", "/proc"), "procfs root (override for tests)")
	flag.StringVar(&logLevel, "log-level", envOr("LOG_LEVEL", "info"), "log level: debug, info, warn, error")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: parseLevel(logLevel)}))
	slog.SetDefault(logger)

	interval, err := parseInterval(intervalStr)
	if err != nil {
		logger.Error("invalid poll interval", slog.String("value", intervalStr), slog.Any("err", err))
		os.Exit(2)
	}

	if _, err := url.Parse(devtoolsURL); err != nil {
		logger.Error("invalid devtools URL", slog.String("value", devtoolsURL), slog.Any("err", err))
		os.Exit(2)
	}

	logger.Info("cb-metrics-sidecar starting",
		slog.String("listen", listen),
		slog.String("devtools_url", devtoolsURL),
		slog.Duration("interval", interval),
		slog.String("proc_root", procRoot),
	)

	// Pre-register known label combinations so the metric series exist
	// in /metrics output even before the first session runs. Without
	// this, GaugeVec series are absent until WithLabelValues is first
	// called, which would make the metrics-presence smoke flaky on a
	// just-booted idle container.
	mOutboundBitrate.WithLabelValues("video").Set(0)
	mOutboundBitrate.WithLabelValues("audio").Set(0)

	pr := &procReader{procRoot: procRoot}
	dt := &devtoolsClient{baseURL: devtoolsURL, log: logger}

	mux := http.NewServeMux()
	mux.Handle("/metrics", promhttp.Handler())
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"status":"ok"}`))
	})
	// T72: client-side stats forwarded by the streamer page over the
	// "stats" data channel. See docs/protocols/stats-channel.md.
	statsHandler, _ := statsUpdateHandler(logger)
	mux.HandleFunc("/stats-update", statsHandler)
	srv := &http.Server{
		Addr:              listen,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		runPoller(ctx, dt, pr, interval, logger)
	}()

	errCh := make(chan error, 1)
	go func() {
		errCh <- srv.ListenAndServe()
	}()

	select {
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("listen failed", slog.Any("err", err))
			os.Exit(1)
		}
	case <-ctx.Done():
		logger.Info("shutdown signal received")
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutdownCtx)
	wg.Wait()
	logger.Info("bye")
}

func envOr(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return fallback
}

func parseLevel(s string) slog.Level {
	switch strings.ToLower(s) {
	case "debug":
		return slog.LevelDebug
	case "warn", "warning":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

// parseInterval accepts either a Go duration ("10s", "30s", "1m") or a
// plain integer treated as seconds. The supervisord environment block
// finds it more natural to set "POLL_INTERVAL_S=10" than "10s".
func parseInterval(s string) (time.Duration, error) {
	if d, err := time.ParseDuration(s); err == nil {
		return d, nil
	}
	if n, err := strconv.Atoi(s); err == nil {
		return time.Duration(n) * time.Second, nil
	}
	return 0, fmt.Errorf("not a duration or integer: %q", s)
}

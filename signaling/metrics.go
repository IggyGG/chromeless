// Package main — metrics.go
//
// T38: Prometheus metrics for the signaling server.
//
// We expose:
//
//	cb_signaling_sessions_total            counter
//	cb_signaling_messages_total{type}      counter (vec)
//	cb_signaling_close_total{code}         counter (vec)
//	cb_signaling_active_sessions           gauge
//	cb_signaling_active_connections{role}  gauge (vec)
//
// All metrics register against prometheus.DefaultRegisterer, so
// promhttp.Handler() picks them up automatically. server.go calls the
// recordXxx helpers below at the relevant hook points; keeping the
// instrumentation behind those helpers means we can later swap in a
// non-default registry, OpenTelemetry, or no-op stubs without
// disturbing server.go.
//
// See infra/observability.md for the rationale on each metric and how
// it's used downstream (Phase 2 latency tuning, Phase 3 capacity
// planning).

package main

import (
	"net/http"
	"strconv"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

var (
	mSessionsTotal = promauto.NewCounter(prometheus.CounterOpts{
		Name: "cb_signaling_sessions_total",
		Help: "Total number of signaling sessions ever created.",
	})

	mMessagesTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_signaling_messages_total",
		Help: "Total signaling messages forwarded, by envelope type.",
	}, []string{"type"})

	mCloseTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_signaling_close_total",
		Help: "Total websocket close events, by close code.",
	}, []string{"code"})

	mActiveSessions = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "cb_signaling_active_sessions",
		Help: "Number of currently-live signaling sessions.",
	})

	mActiveConnections = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_signaling_active_connections",
		Help: "Number of currently-live websocket connections, by peer role.",
	}, []string{"role"})
)

// metricsHandler is the http.Handler exposing /metrics in Prometheus
// exposition format.
func metricsHandler() http.Handler {
	return promhttp.Handler()
}

// recordSessionCreated is called when a new session is added to the hub.
func recordSessionCreated() {
	mSessionsTotal.Inc()
	mActiveSessions.Inc()
}

// recordSessionDropped is called when a session is removed from the hub.
func recordSessionDropped() {
	mActiveSessions.Dec()
}

// recordPeerRegistered is called after a peer joins a session.
func recordPeerRegistered(role peerRole) {
	mActiveConnections.WithLabelValues(string(role)).Inc()
}

// recordPeerUnregistered is called when a peer leaves a session.
func recordPeerUnregistered(role peerRole) {
	mActiveConnections.WithLabelValues(string(role)).Dec()
}

// recordMessageForwarded is called when an envelope is relayed to its
// counterpart peer (or attempted — count includes drops where there's
// no counterpart yet, since those still represent client-side intent).
func recordMessageForwarded(envelopeType string) {
	mMessagesTotal.WithLabelValues(envelopeType).Inc()
}

// recordClose is called when a websocket connection closes. code is
// the websocket close code if known, else 0 (== "no close code").
func recordClose(code int) {
	mCloseTotal.WithLabelValues(strconv.Itoa(code)).Inc()
}

// init pre-registers the label combinations we know we'll see, so the
// metric series exist in /metrics output even on a freshly-booted
// server with no sessions yet. Without this, the GaugeVec / CounterVec
// series are absent until the first event, which would make the
// metrics-presence smoke (T38) flaky.
func init() {
	for _, t := range []string{"offer", "answer", "ice", "bye"} {
		mMessagesTotal.WithLabelValues(t)
	}
	mCloseTotal.WithLabelValues("0")
	mActiveConnections.WithLabelValues(string(roleClient))
	mActiveConnections.WithLabelValues(string(roleBrowser))
}

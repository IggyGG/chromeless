// Package main — metrics.go
//
// T38: Prometheus metrics for the signaling server.
// T67: per-tenant cardinality with a 100-distinct-tenant cap.
//
// We expose:
//
//	cb_signaling_sessions_total{tenant}            counter
//	cb_signaling_messages_total{type}              counter (vec)
//	cb_signaling_close_total{code}                 counter (vec)
//	cb_signaling_active_sessions{tenant}           gauge
//	cb_signaling_active_connections{role,tenant}   gauge (vec)
//
// Tenant cardinality control:
//   We track up to `tenantLabelCap` distinct tenant ids; everything
//   beyond that is bucketed as "_other". This keeps Prometheus from
//   exploding into N-thousand series when an attacker (or a buggy
//   client) starts cycling tenant ids. The `_anonymous` tenant is
//   exempt from the cap — it always gets its own bucket so
//   auth-disabled deployments stay legible.
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
	"sync"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
)

// tenantLabelCap is the max distinct (non-anonymous) tenant labels
// we'll emit. Beyond it we bucket as "_other".
const tenantLabelCap = 100

const (
	tenantOverflow = "_other"
)

var (
	// tenantBucketMu guards seenTenants below. We lookup-then-insert
	// per metric record, so contention is per-record on a sync.Map-style
	// structure; a plain Mutex is fine for the v1 traffic pattern.
	tenantBucketMu sync.RWMutex
	seenTenants    = make(map[string]struct{}, tenantLabelCap)
)

// labelTenant returns the tenant id we should attach to a metric for
// `t`. Returns:
//   - t itself when t is anonymousTenant or already in the seen set,
//     or when the seen set has room.
//   - tenantOverflow when we've hit the cap and t is unseen.
//
// Anonymous never counts toward the cap.
func labelTenant(t string) string {
	if t == anonymousTenant {
		return t
	}
	tenantBucketMu.RLock()
	if _, ok := seenTenants[t]; ok {
		tenantBucketMu.RUnlock()
		return t
	}
	tenantBucketMu.RUnlock()

	tenantBucketMu.Lock()
	defer tenantBucketMu.Unlock()
	if _, ok := seenTenants[t]; ok {
		return t
	}
	if len(seenTenants) >= tenantLabelCap {
		return tenantOverflow
	}
	seenTenants[t] = struct{}{}
	return t
}

var (
	mSessionsTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_signaling_sessions_total",
		Help: "Total number of signaling sessions ever created, by tenant.",
	}, []string{"tenant"})

	mMessagesTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_signaling_messages_total",
		Help: "Total signaling messages forwarded, by envelope type.",
	}, []string{"type"})

	mCloseTotal = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_signaling_close_total",
		Help: "Total websocket close events, by close code.",
	}, []string{"code"})

	mActiveSessions = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_signaling_active_sessions",
		Help: "Number of currently-live signaling sessions, by tenant.",
	}, []string{"tenant"})

	mActiveConnections = promauto.NewGaugeVec(prometheus.GaugeOpts{
		Name: "cb_signaling_active_connections",
		Help: "Number of currently-live websocket connections, by peer role and tenant.",
	}, []string{"role", "tenant"})
)

// metricsHandler is the http.Handler exposing /metrics in Prometheus
// exposition format.
func metricsHandler() http.Handler {
	return promhttp.Handler()
}

// recordSessionCreated is called when a new session is added to the hub.
func recordSessionCreated(tenant string) {
	t := labelTenant(tenant)
	mSessionsTotal.WithLabelValues(t).Inc()
	mActiveSessions.WithLabelValues(t).Inc()
}

// recordSessionDropped is called when a session is removed from the hub.
func recordSessionDropped(tenant string) {
	mActiveSessions.WithLabelValues(labelTenant(tenant)).Dec()
}

// recordPeerRegistered is called after a peer joins a session.
func recordPeerRegistered(role peerRole, tenant string) {
	mActiveConnections.WithLabelValues(string(role), labelTenant(tenant)).Inc()
}

// recordPeerUnregistered is called when a peer leaves a session.
func recordPeerUnregistered(role peerRole, tenant string) {
	mActiveConnections.WithLabelValues(string(role), labelTenant(tenant)).Dec()
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
	for _, t := range []string{"offer", "answer", "ice", "bye", "request_renegotiate"} {
		mMessagesTotal.WithLabelValues(t)
	}
	mCloseTotal.WithLabelValues("0")
	// Pre-register anonymous tenant so /metrics is well-formed in
	// auth-disabled deployments (the dev/test default).
	mSessionsTotal.WithLabelValues(anonymousTenant)
	mActiveSessions.WithLabelValues(anonymousTenant)
	mActiveConnections.WithLabelValues(string(roleClient), anonymousTenant)
	mActiveConnections.WithLabelValues(string(roleBrowser), anonymousTenant)
}

// otlp_logs.go — OTLP-logs exporter for chromeless.webrtc.* dot-named events
// (FU #28).
//
// The Wave 2 A4 closeout left a gap: streamer.js POSTs structured events
// (chromeless.webrtc.session_created, ice.connected, dc.opened, …) to
// /webrtc-event, the sidecar increments Prometheus counters off them,
// and that's all. The dot-name events are SHAPED like activity-feed
// records, but never reach Triform's event bus because the sidecar has
// only an OTLP-trace exporter (tracing.go) and a Prometheus scrape
// surface — no OTLP-logs path.
//
// This file adds the OTLP-logs path. It runs in parallel to the
// existing Prometheus emit in webrtc_handler.go: every event still
// increments its counter, AND now also flows out as an OTLP log record
// to whatever collector $OTEL_EXPORTER_OTLP_ENDPOINT points at (in
// production: a SigNoz collector; in dev compose: usually unset, which
// means this exporter is a no-op stub).
//
// Defaults are deliberately quiet — same shape as tracing.go:
//   - OTEL_EXPORTER_OTLP_ENDPOINT unset → no-op OTLPLogger, no
//     exporter, no goroutines, no wire cost. Tests + dev compose
//     without a collector see this.
//   - When set (e.g. `signoz-otel-collector:4318` for HTTP, or a
//     Triform-internal collector URL), log records flow over OTLP/HTTP
//     to /v1/logs.
//
// Why HTTP not gRPC: tracing.go uses gRPC because that's what the T99
// brief picked; for logs, HTTP/protobuf is the OTel default and works
// against more collectors out of the box (SigNoz collector ships
// /v1/logs on the same listener as /v1/traces). Keeping the shapes
// independent means we can swap protocol per signal without coupling.

package main

import (
	"context"
	"fmt"
	"os"
	"sync"
	"time"

	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp"
	"go.opentelemetry.io/otel/log"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	"go.opentelemetry.io/otel/sdk/resource"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
)

const (
	otlpLogsEnvOTLPEndpoint = "OTEL_EXPORTER_OTLP_ENDPOINT"
	otlpLogsEnvLogsEndpoint = "OTEL_EXPORTER_OTLP_LOGS_ENDPOINT"
	otlpLogsServiceName     = "chromeless-metrics-sidecar"
	otlpLogsScopeName       = "chromeless-metrics-sidecar.webrtc"
)

// OTLPLogger emits dot-named events from /webrtc-event as OTLP log
// records. A nil receiver and a no-op stub (when env is unset) both
// behave as drops — callers can always Emit without nil-checking, and
// Shutdown is always safe to call.
type OTLPLogger struct {
	// provider is nil in the no-op stub. Holding the provider lets
	// Shutdown flush any in-flight batch.
	provider *sdklog.LoggerProvider

	// logger is nil in the no-op stub. The OTel SDK returns a no-op
	// Logger from a shut-down provider, so we check the provider
	// indirectly via this field at Emit time.
	logger log.Logger

	// shutdownOnce guards Shutdown so callers can defer it without
	// worrying about double-close on a graceful + signal-triggered
	// teardown path.
	shutdownOnce sync.Once
}

// newOTLPLogger initialises the exporter from env. Returns a no-op
// stub (with no error) when OTEL_EXPORTER_OTLP_ENDPOINT (or the
// signal-specific override OTEL_EXPORTER_OTLP_LOGS_ENDPOINT) is unset.
//
// The exporter is configured by the standard OTel env-var contract,
// not by hand-rolled options, so production deploys can swap endpoint
// / headers / TLS without touching this file. The same env vars the
// SDK reads natively cover:
//
//	OTEL_EXPORTER_OTLP_ENDPOINT          (e.g. https://otel.example.com)
//	OTEL_EXPORTER_OTLP_LOGS_ENDPOINT     (full /v1/logs URL override)
//	OTEL_EXPORTER_OTLP_HEADERS           (k1=v1,k2=v2 — auth tokens etc)
//	OTEL_EXPORTER_OTLP_LOGS_HEADERS      (logs-specific override)
//	OTEL_EXPORTER_OTLP_LOGS_INSECURE     (disable TLS)
//	OTEL_EXPORTER_OTLP_LOGS_COMPRESSION  (gzip)
//	OTEL_EXPORTER_OTLP_LOGS_TIMEOUT      (ms)
//
// — see https://pkg.go.dev/go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp.
//
// version is the semver string passed to the resource.ServiceVersion
// attribute, mirroring tracing.go's signature.
func newOTLPLogger(ctx context.Context, version string) (*OTLPLogger, error) {
	endpoint := os.Getenv(otlpLogsEnvOTLPEndpoint)
	if endpoint == "" {
		endpoint = os.Getenv(otlpLogsEnvLogsEndpoint)
	}
	if endpoint == "" {
		// No-op stub. Return a non-nil receiver so callers can Emit
		// unconditionally without nil checks.
		return &OTLPLogger{}, nil
	}

	exp, err := otlploghttp.New(ctx)
	if err != nil {
		return nil, fmt.Errorf("otlp logs exporter: %w", err)
	}

	res, err := resource.Merge(
		resource.Default(),
		resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName(otlpLogsServiceName),
			semconv.ServiceVersion(version),
			semconv.DeploymentEnvironment(os.Getenv("CHROMELESS_REGION")),
		),
	)
	if err != nil {
		return nil, fmt.Errorf("otlp logs resource: %w", err)
	}

	provider := sdklog.NewLoggerProvider(
		sdklog.WithResource(res),
		sdklog.WithProcessor(sdklog.NewBatchProcessor(exp)),
	)

	return &OTLPLogger{
		provider: provider,
		logger:   provider.Logger(otlpLogsScopeName),
	}, nil
}

// Emit sends a structured OTLP log record for one chromeless.webrtc.*
// event. Body is the dot-named event string (so consumers indexing
// on body get a useful key); attrs become record attributes.
//
// Safe to call on a nil receiver, a no-op stub, or after Shutdown — in
// all three cases this is a fast drop, no allocation in the hot path
// other than the closure-bound nil check. The webrtc_handler.go fast
// path can call this on every request without conditional wiring.
func (o *OTLPLogger) Emit(event string, attrs map[string]any) {
	if o == nil || o.logger == nil {
		return
	}
	if event == "" {
		return
	}

	now := time.Now()

	rec := log.Record{}
	rec.SetBody(log.StringValue(event))
	rec.SetTimestamp(now)
	rec.SetObservedTimestamp(now)
	// Severity INFO maps cleanly to "informational lifecycle event"
	// in the OTel logs data model — these aren't errors or warnings,
	// they're activity-feed beacons.
	rec.SetSeverity(log.SeverityInfo)
	rec.SetSeverityText("INFO")

	// Translate the JSON-decoded attrs map (string→any) into
	// log.KeyValue entries. We deliberately handle the four scalar
	// kinds the JSON decoder produces (string, float64, bool, and
	// json.Number which we already saw in webrtc_handler.go); other
	// kinds get string-formatted via fmt.Sprint to stay safe.
	for k, v := range attrs {
		rec.AddAttributes(toKeyValue(k, v))
	}

	// The existing emitters (streamer.js side) don't propagate a W3C
	// traceparent into the /webrtc-event POSTs today, so we don't try
	// to extract one here. If/when streamer.js learns to inject one,
	// the SDK's W3C propagator (already installed by tracing.go) plus
	// otelhttp middleware on /webrtc-event would carry it across — at
	// which point we'd swap context.Background() out for the request
	// context. Tracked as a follow-up; not in scope for FU #28.
	o.logger.Emit(context.Background(), rec)
}

// Shutdown flushes any in-flight batch and tears down the exporter
// goroutines. Idempotent — safe to defer alongside a signal-triggered
// shutdown path. No-op stubs return nil immediately.
func (o *OTLPLogger) Shutdown(ctx context.Context) error {
	if o == nil || o.provider == nil {
		return nil
	}
	var err error
	o.shutdownOnce.Do(func() {
		err = o.provider.Shutdown(ctx)
	})
	return err
}

// toKeyValue is the small adapter from JSON-decoded attrs to OTel log
// KeyValue. Mirrors the type-handling in webrtc_handler.go's
// attrFloat64 / attrString helpers — we keep the conversion table here
// in one place rather than building a whole reflection harness.
func toKeyValue(key string, v any) log.KeyValue {
	switch n := v.(type) {
	case string:
		return log.String(key, n)
	case bool:
		return log.Bool(key, n)
	case float64:
		// JSON numbers always decode to float64 in Go's default
		// decoder. We could try to detect "looks like an integer" and
		// emit Int64, but the OTel data model accepts float64 cleanly
		// and ClickHouse-side dashboards (SigNoz) cast on the way in.
		// Simpler is better.
		return log.Float64(key, n)
	case int:
		return log.Int64(key, int64(n))
	case int64:
		return log.Int64(key, n)
	case nil:
		// Render nil attrs as empty strings — OTel doesn't have a
		// first-class "null" attribute kind, and dropping the key
		// silently would obscure what was actually sent.
		return log.String(key, "")
	default:
		// Fallback: any other shape (json.Number, slices, maps)
		// becomes a string. Keeps the wire valid even if streamer.js
		// adds a richer attribute kind we haven't taught the sidecar
		// to handle yet.
		return log.String(key, fmt.Sprint(n))
	}
}

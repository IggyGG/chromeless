// tracing.go — OpenTelemetry SDK init for cb-metrics-sidecar (T99).
//
// This is the third copy of the same shape (signaling, controller,
// sidecar). We deliberately duplicate rather than share an internal
// module: each Go service is a leaf module today, the file is short,
// and the alternative — a /pkg/cbtracing internal — would force every
// service to vendor a common module path through the multi-stage
// builder, which T84's Helm chart and infra/Dockerfile don't.
//
// Defaults:
//   - OTEL_EXPORTER_OTLP_ENDPOINT unset → no-op TracerProvider, no
//     exporter, no goroutines, no wire cost. Tests + dev compose
//     without Jaeger see this.
//   - When set (typically `jaeger:4317` in compose, `tempo:4317` or
//     similar in production), traces flow over OTLP/gRPC.
//   - Sample rate: default 0.1 (T99 brief). Override via
//     CBWRTC_TRACE_SAMPLE_RATIO (0.0..1.0).
//   - Region resource attribute: CBWRTC_REGION (matches the metric
//     ConstLabel from T94 so trace + metric pivots agree).
//
// W3C traceparent comes in via the `cb_trace` field on the v1.1 stats
// envelope (see docs/protocols/stats-channel.md). The streamer page
// stamps it from the browser's active span context, the sidecar
// extracts it in stats_handler.go, and the per-request span we open
// here continues that trace rather than starting a fresh root.

package main

import (
	"context"
	"fmt"
	"os"
	"strconv"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"go.opentelemetry.io/otel/trace"
	"google.golang.org/grpc/credentials/insecure"
)

const (
	tracingEnvOTLPEndpoint = "OTEL_EXPORTER_OTLP_ENDPOINT"
	tracingEnvSampleRatio  = "CBWRTC_TRACE_SAMPLE_RATIO"
	tracingEnvRegion       = "CBWRTC_REGION"
	tracingDefaultSampleR  = 0.1
	tracingServiceName     = "cb-metrics-sidecar"
)

// tracingShutdown returns nil on success; callers should defer it on
// graceful exit so batched spans flush before the process dies.
type tracingShutdown func(context.Context) error

// initTracing wires the global TracerProvider + propagator. Returns a
// no-op shutdown when OTLP endpoint is unset so dev compose without
// Jaeger doesn't pay the wire cost.
func initTracing(ctx context.Context, version string) (tracingShutdown, error) {
	endpoint := os.Getenv(tracingEnvOTLPEndpoint)

	// Always install the W3C+Baggage propagator: even with a no-op
	// provider, downstream code calling
	// `otel.GetTextMapPropagator().Extract(...)` should be able to
	// pick a traceparent off an incoming envelope and let
	// `trace.SpanContextFromContext` carry it forward, so
	// span-less correlation IDs (trace_id stamped in logs) still work.
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))

	if endpoint == "" {
		return func(context.Context) error { return nil }, nil
	}

	exp, err := otlptracegrpc.New(ctx,
		otlptracegrpc.WithEndpoint(endpoint),
		otlptracegrpc.WithTLSCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		return nil, fmt.Errorf("otlp exporter: %w", err)
	}

	res, err := resource.Merge(
		resource.Default(),
		resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName(tracingServiceName),
			semconv.ServiceVersion(version),
			semconv.DeploymentEnvironment(os.Getenv(tracingEnvRegion)),
		),
	)
	if err != nil {
		return nil, fmt.Errorf("resource: %w", err)
	}

	sampleR := tracingDefaultSampleR
	if r := os.Getenv(tracingEnvSampleRatio); r != "" {
		if f, perr := strconv.ParseFloat(r, 64); perr == nil && f >= 0 && f <= 1 {
			sampleR = f
		}
	}

	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exp),
		sdktrace.WithResource(res),
		// ParentBased: an incoming traceparent that says "sample me"
		// keeps us in the trace. New roots use head-based ratio.
		sdktrace.WithSampler(sdktrace.ParentBased(
			sdktrace.TraceIDRatioBased(sampleR),
		)),
	)
	otel.SetTracerProvider(tp)

	return tp.Shutdown, nil
}

// tracingTracer returns a tracer scoped to a subsystem within the
// service so spans group naturally in Jaeger's UI.
func tracingTracer(subsystem string) trace.Tracer {
	return otel.Tracer(tracingServiceName + "." + subsystem)
}

// keep otlptrace imported even when only the otlptracegrpc constructor
// is used directly — go vet otherwise gripes.
var _ = otlptrace.Exporter{}

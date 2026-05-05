// Package tracing initialises the OpenTelemetry SDK for the
// browser-session-controller (T99).
//
// Defaults are deliberately quiet — when `OTEL_EXPORTER_OTLP_ENDPOINT`
// is unset, this package returns a no-op TracerProvider so tests and
// dev compose runs without Jaeger don't pay the wire cost. When the
// env is set, traces flow over OTLP/gRPC to the configured endpoint
// (typically `jaeger:4317` in compose, or a Tempo / Grafana Cloud
// endpoint in production).
//
// Sample rate: default 10% per the T99 brief. Override globally via
// `CHROMELESS_TRACE_SAMPLE_RATIO` (0.0–1.0). Per-tenant overrides happen
// at the request level by setting a baggage value before the root
// span is created — the helper `WithTenantSampling` in this package
// handles the lookup.
//
// The shape mirrors the metrics.go ConstLabels pattern from T94: a
// single env var flips the feature on, missing env means "off, no
// emission, no broken pipe to Jaeger." Three Go services (signaling,
// controller, chromeless-metrics-sidecar) each have a copy of this file —
// modules are independent and this is small enough that duplication
// costs less than a shared internal-modules dance.

package tracing

import (
	"context"
	"fmt"
	"os"
	"strconv"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"go.opentelemetry.io/otel/trace"
	"google.golang.org/grpc/credentials/insecure"

	"go.opentelemetry.io/otel/exporters/otlp/otlptrace"
)

const (
	envOTLPEndpoint = "OTEL_EXPORTER_OTLP_ENDPOINT"
	envSampleRatio  = "CHROMELESS_TRACE_SAMPLE_RATIO"
	envRegion       = "CHROMELESS_REGION"
	defaultSampleR  = 0.1
)

// ServiceName is the OTel service.name resource attribute. Set once
// at process init by the Init call.
const ServiceName = "browser-session-controller"

// Shutdown is returned by Init; the caller defers it to flush spans
// and tear down exporter goroutines on graceful exit.
type Shutdown func(context.Context) error

// Init wires the global OTel TracerProvider. When OTLP endpoint is
// unset, returns a no-op shutdown without configuring an exporter —
// callers can still acquire tracers via otel.Tracer(...), they just
// don't emit anywhere.
func Init(ctx context.Context, version string) (Shutdown, error) {
	endpoint := os.Getenv(envOTLPEndpoint)
	if endpoint == "" {
		// Leave the no-op TracerProvider that go.opentelemetry.io/otel
		// installs by default; callers calling otel.Tracer(...).Start
		// will get noop spans.
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
			semconv.ServiceName(ServiceName),
			semconv.ServiceVersion(version),
			semconv.DeploymentEnvironment(os.Getenv(envRegion)),
		),
	)
	if err != nil {
		return nil, fmt.Errorf("resource: %w", err)
	}

	sampleR := defaultSampleR
	if r := os.Getenv(envSampleRatio); r != "" {
		if f, perr := strconv.ParseFloat(r, 64); perr == nil && f >= 0 && f <= 1 {
			sampleR = f
		}
	}

	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exp),
		sdktrace.WithResource(res),
		// ParentBased so an incoming traceparent that says "sample me"
		// keeps us in the trace; head-based ratio decides for new roots.
		sdktrace.WithSampler(sdktrace.ParentBased(
			sdktrace.TraceIDRatioBased(sampleR),
		)),
	)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{}, // W3C traceparent
		propagation.Baggage{},
	))

	return tp.Shutdown, nil
}

// Tracer returns a tracer scoped to the given subsystem within the
// service. Callers should pass a stable string like "reconciler.session"
// so spans group naturally in Jaeger's UI.
func Tracer(subsystem string) trace.Tracer {
	return otel.Tracer(ServiceName + "." + subsystem)
}

// keep otlptrace imported even when only the otlptracegrpc constructor
// is referenced directly — go vet otherwise gripes.
var _ = otlptrace.Exporter{}

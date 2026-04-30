// Package reconciler — tracing_helpers.go
//
// Thin shims so the reconciler files can call `tracingTracer(...)`
// and `tracingAttr*(...)` without each file importing the full OTel
// API. T99 keeps the reconciler files focused on behaviour; the
// tracing call sites stay readable.

package reconciler

import (
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/trace"

	"github.com/iggy/cloud-browser-webrtc/infra/controllers/browser-session-controller/pkg/tracing"
)

func tracingTracer(subsystem string) trace.Tracer {
	return tracing.Tracer(subsystem)
}

func tracingAttrString(k, v string) attribute.KeyValue {
	return attribute.String(k, v)
}

func tracingAttrInt(k string, v int) attribute.KeyValue {
	return attribute.Int(k, v)
}

func traceWithAttrs(kv ...attribute.KeyValue) trace.SpanStartOption {
	return trace.WithAttributes(kv...)
}

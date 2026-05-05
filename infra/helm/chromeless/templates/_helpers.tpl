{{/*
infra/helm/chromeless/templates/_helpers.tpl

Common helper templates. Standard Helm pattern; nothing unusual.
*/}}

{{/* Resolve the namespace name (chart-managed or pre-existing). */}}
{{- define "chromeless.namespace" -}}
{{- default "chromeless" .Values.namespace.name -}}
{{- end -}}

{{/* Image reference for a given component. */}}
{{- define "chromeless.image" -}}
{{- $component := .component -}}
{{- $values := .values -}}
{{- $registry := $values.images.registry -}}
{{- $img := index $values.images $component -}}
{{- $tag := default $.appVersion $img.tag -}}
{{- printf "%s/%s:%s" $registry $img.repository $tag -}}
{{- end -}}

{{/* Standard labels applied to every resource. */}}
{{- define "chromeless.labels" -}}
app.kubernetes.io/name: chromeless
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end -}}

{{/*
Component selector labels — used as Service / Deployment selectors.
Must NOT include the chart/version labels (they change on upgrade).
*/}}
{{- define "chromeless.selectorLabels" -}}
app.kubernetes.io/name: chromeless
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/component: {{ .component }}
{{- end -}}

{{/*
Fail with a helpful message when a required value is missing.
Usage: {{ include "chromeless.requireValue" (dict "value" .Values.x.y "name" "x.y" "context" "turnIssuer enabled") }}
*/}}
{{- define "chromeless.requireValue" -}}
{{- if not .value -}}
{{- fail (printf "values.%s is required (%s)" .name .context) -}}
{{- end -}}
{{- end -}}

{{/*
T94: resolve the region this deployment serves. Top-level
.Values.region is the canonical knob; per-component overrides
(`.Values.signaling.region`, etc.) are still honoured for
backwards compat with T93's signaling-only addition.

Usage: {{ include "chromeless.region" (dict "context" . "component" "signaling") }}
- `component` is optional; when set, that component's per-component
  region overrides the top-level value.
- Returns "" when nothing is set; templates SHOULD wrap their env
  block with `{{ if $region }}` so they don't emit an empty
  CHROMELESS_REGION (which would mean "no region label" downstream
  rather than "the empty-string region").
*/}}
{{- define "chromeless.region" -}}
{{- $ctx := .context -}}
{{- $component := .component -}}
{{- $top := default "" $ctx.Values.region -}}
{{- $override := "" -}}
{{- if $component -}}
  {{- if hasKey $ctx.Values $component -}}
    {{- $cv := index $ctx.Values $component -}}
    {{- if and $cv (kindIs "map" $cv) (hasKey $cv "region") -}}
      {{- $override = default "" $cv.region -}}
    {{- end -}}
  {{- end -}}
{{- end -}}
{{- if $override -}}{{- $override -}}{{- else -}}{{- $top -}}{{- end -}}
{{- end -}}

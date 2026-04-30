{{/*
infra/helm/cloud-browser-webrtc/templates/_helpers.tpl

Common helper templates. Standard Helm pattern; nothing unusual.
*/}}

{{/* Resolve the namespace name (chart-managed or pre-existing). */}}
{{- define "cb.namespace" -}}
{{- default "cloud-browser-webrtc" .Values.namespace.name -}}
{{- end -}}

{{/* Image reference for a given component. */}}
{{- define "cb.image" -}}
{{- $component := .component -}}
{{- $values := .values -}}
{{- $registry := $values.images.registry -}}
{{- $img := index $values.images $component -}}
{{- $tag := default $.appVersion $img.tag -}}
{{- printf "%s/%s:%s" $registry $img.repository $tag -}}
{{- end -}}

{{/* Standard labels applied to every resource. */}}
{{- define "cb.labels" -}}
app.kubernetes.io/name: cloud-browser-webrtc
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
{{- end -}}

{{/*
Component selector labels — used as Service / Deployment selectors.
Must NOT include the chart/version labels (they change on upgrade).
*/}}
{{- define "cb.selectorLabels" -}}
app.kubernetes.io/name: cloud-browser-webrtc
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/component: {{ .component }}
{{- end -}}

{{/*
Fail with a helpful message when a required value is missing.
Usage: {{ include "cb.requireValue" (dict "value" .Values.x.y "name" "x.y" "context" "turnIssuer enabled") }}
*/}}
{{- define "cb.requireValue" -}}
{{- if not .value -}}
{{- fail (printf "values.%s is required (%s)" .name .context) -}}
{{- end -}}
{{- end -}}

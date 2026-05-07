# syntax=docker/dockerfile:1.7
#
# chromeless-metrics-sidecar — pod-level Prometheus and OTLP event bridge.

FROM golang:1.22-alpine AS build
WORKDIR /src

COPY go.mod go.sum ./
RUN go mod download

COPY . .

ENV CGO_ENABLED=0 GOOS=linux
RUN go test ./... && \
    go build -trimpath -ldflags="-s -w" -o /out/chromeless-metrics-sidecar .

FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /

COPY --from=build /out/chromeless-metrics-sidecar /chromeless-metrics-sidecar

EXPOSE 9100

USER nonroot:nonroot

ENTRYPOINT ["/chromeless-metrics-sidecar"]

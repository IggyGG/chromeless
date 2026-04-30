module github.com/iggy/cloud-browser-webrtc/capture/cb-metrics-sidecar

// Match signaling/go.mod to keep both modules buildable on the same
// golang:1.22-alpine builder stage in infra/Dockerfile. The deps in
// use today (gorilla/websocket v1.5.3, prometheus/client_golang
// v1.20.5) all build on 1.22; bump this when we actually need a
// 1.25+ language feature.
go 1.22

require (
	github.com/gorilla/websocket v1.5.3
	github.com/prometheus/client_golang v1.20.5
)

require (
	github.com/beorn7/perks v1.0.1 // indirect
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/klauspost/compress v1.17.9 // indirect
	github.com/munnerz/goautoneg v0.0.0-20191010083416-a7dc8b61c822 // indirect
	github.com/prometheus/client_model v0.6.1 // indirect
	github.com/prometheus/common v0.55.0 // indirect
	github.com/prometheus/procfs v0.15.1 // indirect
	golang.org/x/sys v0.22.0 // indirect
	google.golang.org/protobuf v1.34.2 // indirect
)

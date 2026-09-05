// Package main — viewport.go
//
// POST /api/viewport {"width": <int>, "height": <int>}  → Cb.setViewport
//
// The stream was a fixed 1280x720 letterbox no matter how large the viewer's
// window was — the most visible "this is not a real browser" tell the
// standalone client had. The embedder grew Cb.setViewport (see
// docs/protocols/cb-cdp-methods.md) to fix exactly that, and then nothing in
// client/ or infra/gateway/ ever called it. This is the caller.
//
// Why the gateway and not the input DataChannel: the input channel is an
// untrusted client→guest path. A guest that resized itself straight off it
// would let a hostile client request 8192x8192 and OOM the worker. Routing the
// resize through an authenticated HTTP verb puts a policy point in the path —
// the guest clamps (200-2560, even-aligned) and this endpoint clamps first, so
// the guest never even sees an absurd request. The structured ack is the other
// half: the client applies the size the guest ACTUALLY set, which may differ
// from what it asked for.

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
)

// Mirrors the guest's own clamp (cb_viewport_controller.cc). Kept identical
// so a request the guest would refuse is refused here with a readable reason
// instead of a CDP error string — and so a future guest change to the range
// shows up as a disagreement in tests rather than a silent divergence.
const (
	viewportMinDIP = 200
	viewportMaxDIP = 2560
)

type viewportRequest struct {
	Width  int `json:"width"`
	Height int `json:"height"`
}

// clampViewport applies the same rules the guest applies, in the same order:
// clamp to [min, max], then round DOWN to even (I420 subsamples chroma 2x2;
// an odd dimension leaves a half-sampled edge). Down, never up, so alignment
// cannot push a value back over the ceiling it was just clamped to.
func clampViewport(w, h int) (int, int) {
	c := func(v int) int {
		if v < viewportMinDIP {
			v = viewportMinDIP
		}
		if v > viewportMaxDIP {
			v = viewportMaxDIP
		}
		return v &^ 1
	}
	return c(w), c(h)
}

func (g *gateway) handleViewport(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var body viewportRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<10)).Decode(&body); err != nil {
		writeJSONError(w, http.StatusBadRequest, "malformed JSON body")
		return
	}
	if body.Width <= 0 || body.Height <= 0 {
		writeJSONError(w, http.StatusBadRequest, "width and height must be positive integers")
		return
	}
	width, height := clampViewport(body.Width, body.Height)

	ctx, cancel := contextWithNavTimeout(r)
	defer cancel()

	applied, err := g.cdp.setViewport(ctx, width, height)
	if err != nil {
		// Includes the guest predating Cb.setViewport (-32601 method not
		// found). The client treats any error here as "resize unsupported"
		// and stops asking, so an old guest costs one failed request, not a
		// storm of them.
		g.log.Warn("viewport request failed",
			slog.Int("width", width), slog.Int("height", height), slog.Any("err", err))
		writeJSONError(w, http.StatusBadGateway, err.Error())
		return
	}

	g.log.Info("viewport applied",
		slog.Int("requested_w", body.Width), slog.Int("requested_h", body.Height),
		slog.Int("width", applied.Width), slog.Int("height", applied.Height),
		slog.Float64("dsf", applied.DeviceScaleFactor))
	writeJSON(w, applied)
}

// viewportApplied is the guest's ack: the size it ACTUALLY set. Callers must
// treat this as authoritative rather than assuming their request landed
// verbatim — a silently-clamped resize the caller believes succeeded is how
// geometry disagreements start.
type viewportApplied struct {
	Width             int     `json:"width"`
	Height            int     `json:"height"`
	DeviceScaleFactor float64 `json:"deviceScaleFactor"`
}

// setViewport issues Cb.setViewport and decodes the applied geometry.
//
// deviceScaleFactor is deliberately NOT sent: omitted means "keep the current
// scale", and the guest clamps it to 1.0 today regardless (HiDPI is blocked
// on DIP-normalising input coordinates — cb-cdp-methods.md). When that lands,
// the client sends devicePixelRatio and this gains a third field.
func (c *cdpClient) setViewport(ctx context.Context, width, height int) (viewportApplied, error) {
	raw, err := c.send(ctx, "Cb.setViewport", map[string]any{
		"width":  width,
		"height": height,
	})
	if err != nil {
		return viewportApplied{}, err
	}
	var applied viewportApplied
	if err := json.Unmarshal(raw, &applied); err != nil {
		return viewportApplied{}, fmt.Errorf("parse Cb.setViewport result: %w", err)
	}
	if applied.Width == 0 || applied.Height == 0 {
		return viewportApplied{}, fmt.Errorf("Cb.setViewport returned no geometry: %s", string(raw))
	}
	return applied, nil
}

// Package main — cdp.go
//
// A deliberately tiny DevTools client: find the page target, open a socket,
// send one command, read the reply, close. Everything the navigation endpoints
// need and nothing else.
//
// Why the gateway drives CDP at all. The embedder hardcodes its start page to
// about:blank (cloud_browser_browser_main_parts.cc) and reads exactly two
// command-line switches, both --remote-debugging-*. There is no start-URL flag
// — infra/launch-chromeless.sh passes --app="$CHROMIUM_START_URL", which the
// embedder ignores entirely — and no navigation verb on any data channel. So
// without something here, a standalone user gets a live video stream of a
// blank page and no way to leave it.
//
// This matches how the triform portal does it: navigation there is NOT carried
// over WebRTC either. The peer connection is pixels-and-input; the portal
// posts to physics, which drives CDP Page.navigate on the guest
// (physics/src/infra/compute/browser.rs). The gateway plays physics' part.
//
// SCOPE, and it matters: this exposes URL-shaped verbs, never raw CDP.
// Proxying DevTools through an authenticated port would hand any logged-in
// caller Runtime.evaluate — arbitrary code in the browser, plus file reads via
// Page.navigate to file://. The whole point of unpublishing 9222 is that
// nothing gets to speak CDP except this file, and this file only says
// Page.navigate/goBack/goForward/reload/stopLoading.

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/gorilla/websocket"
)

// cdpClient talks to one worker's DevTools endpoint.
type cdpClient struct {
	baseURL string // http://host:9222
}

// cdpTarget is the subset of a /json entry we use.
type cdpTarget struct {
	Type                 string `json:"type"`
	URL                  string `json:"url"`
	Title                string `json:"title"`
	WebSocketDebuggerURL string `json:"webSocketDebuggerUrl"`
}

// pageTarget returns the debugger URL of the worker's page.
//
// Two traps live here, both documented in tests/cdp/conftest.py and each a
// silent failure otherwise:
//
//  1. The embedder enables chromium's DNS-rebinding protection, so a GET from
//     a container that dials http://chromium:9222 is answered with 403 unless
//     the Host header says localhost. The response body does not explain why.
//  2. webSocketDebuggerUrl comes back pointing at localhost (sometimes with no
//     port at all), because that is what chromium bound. Dialing it verbatim
//     from another container connects to nothing, or worse to something else
//     listening locally. The host portion has to be rewritten to the address
//     we actually reached.
//
// Only /json is used. /json/protocol CHECK-FATALs the worker — see CLAUDE.md
// and the nine test files carrying `local: true` for the same reason.
func (c *cdpClient) pageTarget(ctx context.Context) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/json", nil)
	if err != nil {
		return "", err
	}
	// Trap 1. Setting req.Host (not the header map) is what actually changes
	// the request line's Host for net/http.
	req.Host = "localhost"

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", fmt.Errorf("devtools unreachable at %s: %w", c.baseURL, err)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return "", err
	}
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("devtools /json returned %d", resp.StatusCode)
	}

	var targets []cdpTarget
	if err := json.Unmarshal(body, &targets); err != nil {
		return "", fmt.Errorf("devtools /json: %w", err)
	}
	for _, t := range targets {
		if t.Type != "page" || t.WebSocketDebuggerURL == "" {
			continue
		}
		return rewriteWSHost(t.WebSocketDebuggerURL, c.baseURL) // trap 2
	}
	return "", fmt.Errorf("no page target at %s (worker still booting?)", c.baseURL)
}

// rewriteWSHost swaps the host:port of a ws:// URL for the one in base.
func rewriteWSHost(wsURL, base string) (string, error) {
	w, err := url.Parse(wsURL)
	if err != nil {
		return "", fmt.Errorf("parse webSocketDebuggerUrl %q: %w", wsURL, err)
	}
	b, err := url.Parse(base)
	if err != nil {
		return "", fmt.Errorf("parse devtools base %q: %w", base, err)
	}
	// b.Host carries the port when one was given. When it was not, default to
	// 9222 rather than inheriting chromium's advertised (possibly absent) one.
	host := b.Host
	if _, _, err := net.SplitHostPort(host); err != nil {
		host = net.JoinHostPort(host, "9222")
	}
	w.Host = host
	return w.String(), nil
}

// send issues one CDP command and waits for its reply.
//
// A fresh socket per command. The alternative — a long-lived connection with
// an id-to-caller map — buys nothing here: navigation is user-paced, and a
// pooled socket would need reconnect handling for a worker that restarts.
func (c *cdpClient) send(ctx context.Context, method string, params map[string]any) (json.RawMessage, error) {
	wsURL, err := c.pageTarget(ctx)
	if err != nil {
		return nil, err
	}

	dialer := websocket.Dialer{HandshakeTimeout: 5 * time.Second}
	conn, _, err := dialer.DialContext(ctx, wsURL, nil)
	if err != nil {
		return nil, fmt.Errorf("cdp dial: %w", err)
	}
	defer conn.Close()

	const reqID = 1
	req := map[string]any{"id": reqID, "method": method}
	if params != nil {
		req["params"] = params
	}
	if err := conn.WriteJSON(req); err != nil {
		return nil, fmt.Errorf("cdp write: %w", err)
	}

	deadline, ok := ctx.Deadline()
	if !ok {
		deadline = time.Now().Add(10 * time.Second)
	}
	_ = conn.SetReadDeadline(deadline)

	for {
		var resp struct {
			ID     int             `json:"id"`
			Result json.RawMessage `json:"result,omitempty"`
			Error  *struct {
				Code    int    `json:"code"`
				Message string `json:"message"`
			} `json:"error,omitempty"`
		}
		if err := conn.ReadJSON(&resp); err != nil {
			return nil, fmt.Errorf("cdp read: %w", err)
		}
		// The connection also carries events (Page.frameNavigated and friends),
		// which have no id. Skip anything that is not our reply.
		if resp.ID != reqID {
			continue
		}
		if resp.Error != nil {
			return nil, fmt.Errorf("%s: %s", method, resp.Error.Message)
		}
		return resp.Result, nil
	}
}

// navigate points the worker's page at url.
func (c *cdpClient) navigate(ctx context.Context, url string) error {
	// Page.enable first so the navigation is driven with the Page domain
	// active; the embedder's own capture re-arm hangs off those events.
	if _, err := c.send(ctx, "Page.enable", nil); err != nil {
		return err
	}
	_, err := c.send(ctx, "Page.navigate", map[string]any{"url": url})
	return err
}

// simpleCommand runs a parameterless Page command (goBack/goForward/reload/…).
func (c *cdpClient) simpleCommand(ctx context.Context, method string) error {
	if _, err := c.send(ctx, "Page.enable", nil); err != nil {
		return err
	}
	_, err := c.send(ctx, method, nil)
	return err
}

// currentURL reports what the page is showing.
func (c *cdpClient) currentURL(ctx context.Context) (string, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, c.baseURL+"/json", nil)
	if err != nil {
		return "", err
	}
	req.Host = "localhost"
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	if err != nil {
		return "", err
	}
	var targets []cdpTarget
	if err := json.Unmarshal(body, &targets); err != nil {
		return "", err
	}
	for _, t := range targets {
		if t.Type == "page" {
			return t.URL, nil
		}
	}
	return "", nil
}

// validateNavigationURL is the allowlist between a logged-in caller and the
// browser process.
//
// http and https only. The rejected schemes are not hypothetical:
//
//	file:       reads the worker's filesystem and renders it into the video
//	            stream — /etc/passwd, mounted secrets, the TLS key.
//	javascript: executes in whatever page is loaded.
//	chrome:, devtools:  chrome://net-internals and friends are privileged
//	            surfaces well beyond "browse the web".
//	data:       renders attacker-controlled HTML in a same-origin-ish context.
//
// An allowlist rather than a denylist, because the interesting schemes are the
// ones nobody thought to ban.
func validateNavigationURL(raw string) (string, error) {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "", fmt.Errorf("url required")
	}
	if len(raw) > 4096 {
		return "", fmt.Errorf("url too long")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return "", fmt.Errorf("not a valid url")
	}
	switch strings.ToLower(u.Scheme) {
	case "http", "https":
	default:
		return "", fmt.Errorf("only http:// and https:// urls are allowed")
	}
	if u.Host == "" {
		return "", fmt.Errorf("url has no host")
	}
	return u.String(), nil
}

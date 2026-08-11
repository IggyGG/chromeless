// Package main is the chromeless standalone gateway.
//
// It is the single host-facing port for a self-hosted deployment:
//
//	https://localhost:8443/          the client bundle
//	              /login  /logout    operator credential, session cookie
//	              /ws/               → signaling broker (WebSocket)
//	              /turn-credentials  → signaling broker
//	              /probe             → signaling broker (token-authed)
//
// Everything else — the broker itself, and Chromium's DevTools port — stays on
// the internal network. That is the point: a compose deployment used to
// publish 9222 with --remote-allow-origins=*, which is an unauthenticated
// remote-code-execution surface, alongside a broker that logged "auth
// disabled: any caller can connect" on every boot.
//
// Configuration is entirely environment-driven; see config.go.

package main

import (
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// healthcheck mode. The runtime image is distroless — no shell, no curl — so a
// compose HEALTHCHECK cannot be an ordinary command line. Re-executing this
// same binary with a flag is the standard way out, and it keeps the probe in
// step with the server it is probing.
var healthcheckMode = flag.Bool("healthcheck", false,
	"probe the local gateway and exit 0 if healthy (used by the container HEALTHCHECK)")

func main() {
	flag.Parse()
	if *healthcheckMode {
		os.Exit(runHealthcheck())
	}

	logger := slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	cfg, err := loadConfig()
	if err != nil {
		// Exit 2 for "you configured this wrong", matching turn-issuer, so a
		// supervisor can tell it apart from a runtime crash. Restarting will
		// not help, and the message names the variable.
		logger.Error("startup config invalid", slog.Any("err", err))
		os.Exit(2)
	}

	cert, err := loadOrCreateCert(cfg, logger)
	if err != nil {
		logger.Error("TLS setup failed", slog.Any("err", err))
		os.Exit(2)
	}

	g, err := newGateway(cfg, logger)
	if err != nil {
		logger.Error("gateway init failed", slog.Any("err", err))
		os.Exit(2)
	}

	// The broker verifies what we mint, so it needs this value as
	// CHROMELESS_AUTH_PUBKEY. Logged unconditionally: if it is not set there,
	// the broker logs "auth disabled ... any caller can connect" and the login
	// protects only the HTML — a failure mode with no other visible symptom.
	logger.Info("session-token signing key ready",
		slog.String("CHROMELESS_AUTH_PUBKEY", g.issuer.pubKeyBase64()),
		slog.Bool("generated", cfg.authPrivkey == ""))
	if cfg.authPrivkey == "" {
		logger.Warn("signing key was generated at startup, so it changes on every " +
			"restart — set CHROMELESS_AUTH_PRIVKEY (and the matching " +
			"CHROMELESS_AUTH_PUBKEY on signaling) to keep the broker in step")
	}

	srv := &http.Server{
		Addr:              cfg.addr,
		Handler:           g.routes(),
		ReadHeaderTimeout: 5 * time.Second,
		TLSConfig: &tls.Config{
			Certificates: []tls.Certificate{cert},
			MinVersion:   tls.VersionTLS12,
		},
		// No WriteTimeout: the WebSocket proxy holds connections open for the
		// life of a session, and a write deadline would sever them mid-stream.
	}

	// CHROMIUM_START_URL finally does something. infra/launch-chromeless.sh has
	// always passed it as --app=<url>, but the embedder reads only
	// --remote-debugging-{port,address} and hardcodes about:blank, so the knob
	// has never had any effect. Best-effort and asynchronous: the worker is
	// usually still booting, and an unreachable start page is no reason to
	// refuse to serve the UI.
	if cfg.startURL != "" {
		g.navigateAtStartup(cfg.startURL)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	errCh := make(chan error, 1)
	go func() {
		logger.Info("gateway listening",
			slog.String("addr", cfg.addr),
			slog.String("signaling", cfg.signalingURL),
			slog.String("static", cfg.staticDir))
		// Certificates come from TLSConfig, so both paths are empty here.
		errCh <- srv.ListenAndServeTLS("", "")
	}()

	select {
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("listen failed", slog.Any("err", err))
			os.Exit(1)
		}
	case <-ctx.Done():
		logger.Info("shutdown signal received")
	}

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = srv.Shutdown(shutdownCtx)
	logger.Info("bye")
}

// runHealthcheck probes /healthz over loopback and returns a process exit code.
//
// InsecureSkipVerify is correct here and only here: the target is this same
// process over loopback, and with a self-signed certificate there is no chain
// to verify against. Verifying would fail every time and report a healthy
// server as sick. Nothing outside this function talks to a remote host.
func runHealthcheck() int {
	port := envOr(envPort, defaultPort)
	client := &http.Client{
		Timeout: 3 * time.Second,
		Transport: &http.Transport{
			TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, //nolint:gosec // loopback self-probe
		},
	}
	resp, err := client.Get("https://127.0.0.1:" + port + "/healthz")
	if err != nil {
		fmt.Fprintf(os.Stderr, "healthcheck: %v\n", err)
		return 1
	}
	defer resp.Body.Close()
	_, _ = io.Copy(io.Discard, resp.Body)
	if resp.StatusCode != http.StatusOK {
		fmt.Fprintf(os.Stderr, "healthcheck: status %d\n", resp.StatusCode)
		return 1
	}
	return 0
}

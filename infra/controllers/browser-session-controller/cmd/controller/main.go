// Package main — controller entry point.
//
// Wires the controller-runtime Manager: scheme registration, leader
// election, both reconcilers (BrowserSession + BrowserSessionPool),
// and a /metrics + /healthz HTTP listener. Configuration is via env;
// defaults are dev-friendly and documented in the README.

package main

import (
	"flag"
	"fmt"
	"os"

	"k8s.io/apimachinery/pkg/runtime"
	utilruntime "k8s.io/apimachinery/pkg/util/runtime"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/healthz"
	"sigs.k8s.io/controller-runtime/pkg/log/zap"
	metricsserver "sigs.k8s.io/controller-runtime/pkg/metrics/server"

	cbv1 "github.com/iggy/cloud-browser-webrtc/infra/controllers/browser-session-controller/pkg/apis/v1"
	"github.com/iggy/cloud-browser-webrtc/infra/controllers/browser-session-controller/pkg/reconciler"
	"github.com/iggy/cloud-browser-webrtc/infra/controllers/browser-session-controller/pkg/tracing"
)

// version is overridden at build time via -ldflags="-X main.version=$TAG".
var version = "dev"

var scheme = runtime.NewScheme()

func init() {
	utilruntime.Must(clientgoscheme.AddToScheme(scheme))
	utilruntime.Must(cbv1.AddToScheme(scheme))
}

func main() {
	var (
		metricsAddr          string
		healthAddr           string
		enableLeaderElection bool
		leaderElectionID     string
		defaultPool          string
	)
	flag.StringVar(&metricsAddr, "metrics-bind", envOr("METRICS_BIND", ":8080"), "address /metrics binds to")
	flag.StringVar(&healthAddr, "health-bind", envOr("HEALTH_BIND", ":8081"), "address /healthz binds to")
	flag.BoolVar(&enableLeaderElection, "leader-elect", envOr("LEADER_ELECT", "true") == "true",
		"acquire a coordination.k8s.io Lease before reconciling; required for >1 replica")
	flag.StringVar(&leaderElectionID, "leader-elect-id", envOr("LEADER_ELECT_ID", "browser-session-controller.cloud-browser-webrtc.example.com"),
		"Lease name used by leader election")
	flag.StringVar(&defaultPool, "default-pool", envOr("DEFAULT_POOL", "default-pool"),
		"BrowserSessionPool name used when a BrowserSession.spec.poolName is empty")

	zapOpts := zap.Options{Development: false}
	zapOpts.BindFlags(flag.CommandLine)
	flag.Parse()
	ctrl.SetLogger(zap.New(zap.UseFlagOptions(&zapOpts)))

	logger := ctrl.Log.WithName("setup")

	// T99: OpenTelemetry tracing. Init returns a no-op shutdown when
	// OTEL_EXPORTER_OTLP_ENDPOINT is unset (test / dev compose
	// without Jaeger), so this is safe to call unconditionally.
	tracingShutdown, err := tracing.Init(ctrl.SetupSignalHandler(), version)
	if err != nil {
		logger.Error(err, "tracing init failed; continuing without tracing")
	}
	defer func() {
		if tracingShutdown != nil {
			_ = tracingShutdown(ctrl.SetupSignalHandler())
		}
	}()

	mgr, err := ctrl.NewManager(ctrl.GetConfigOrDie(), ctrl.Options{
		Scheme: scheme,
		Metrics: metricsserver.Options{
			BindAddress: metricsAddr,
		},
		HealthProbeBindAddress: healthAddr,
		LeaderElection:         enableLeaderElection,
		LeaderElectionID:       leaderElectionID,
	})
	if err != nil {
		logger.Error(err, "unable to start manager")
		os.Exit(1)
	}

	// T90: ScrubAndReturn needs Pod-exec, which client-runtime's
	// abstract Client doesn't expose. Build a separate executor on
	// top of the same rest.Config the manager uses.
	scrubExec, err := reconciler.NewRealScrubExecutor(mgr.GetConfig())
	if err != nil {
		logger.Error(err, "unable to build scrub executor; ScrubAndReturn will be unavailable")
		// Non-fatal: the session reconciler degrades to RecreatePod
		// when ScrubExec is nil.
		scrubExec = nil
	}

	if err := (&reconciler.SessionReconciler{
		Client:      mgr.GetClient(),
		Scheme:      mgr.GetScheme(),
		DefaultPool: defaultPool,
		ScrubExec:   scrubExec,
	}).SetupWithManager(mgr); err != nil {
		logger.Error(err, "unable to start SessionReconciler")
		os.Exit(1)
	}

	if err := (&reconciler.PoolReconciler{
		Client: mgr.GetClient(),
		Scheme: mgr.GetScheme(),
	}).SetupWithManager(mgr); err != nil {
		logger.Error(err, "unable to start PoolReconciler")
		os.Exit(1)
	}

	if err := mgr.AddHealthzCheck("ping", healthz.Ping); err != nil {
		logger.Error(err, "unable to set up health check")
		os.Exit(1)
	}
	if err := mgr.AddReadyzCheck("ping", healthz.Ping); err != nil {
		logger.Error(err, "unable to set up readiness check")
		os.Exit(1)
	}

	logger.Info("starting manager",
		"metrics-bind", metricsAddr,
		"health-bind", healthAddr,
		"leader-elect", enableLeaderElection,
		"default-pool", defaultPool,
	)
	if err := mgr.Start(ctrl.SetupSignalHandler()); err != nil {
		logger.Error(err, "manager exited with error")
		os.Exit(1)
	}
}

func envOr(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return fallback
}

// Avoid an unused-import warning on minimal build configs.
var _ = fmt.Sprintf

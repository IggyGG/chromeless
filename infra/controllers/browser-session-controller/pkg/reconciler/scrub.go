// Package reconciler — scrub.go
//
// T90: ScrubAndReturn implementation for BrowserSessionPool.
//
// On session end (`BrowserSession.status.phase == Ended`), the pool's
// `recyclePolicy` decides whether the bound pod is destroyed
// (`RecreatePod`, the default) or scrubbed in-place and returned to
// the warm pool (`ScrubAndReturn`).
//
// ScrubAndReturn is faster — no pod creation churn, no Chromium
// cold-boot — at the cost of a stricter trust requirement: the pod
// must end the session in a state that's safe to hand to the next
// tenant. See `infra/snapshots/README.md` for the security
// trade-off; in short, ScrubAndReturn is only safe for
// **tenant-clean pools** (single-tenant deployments, OR pools where
// every reuse is preceded by a fresh snapshot restore).
//
// Wire shape: scrub-pod.sh runs inside the chromeless container
// (called by the controller via Pod-exec). Its stdout contains the
// literal marker "[scrub] OK" on success. Anything else — non-zero
// exit, missing marker, exec error — is treated as failure and the
// reconciler falls back to RecreatePod (delete + replenish).

package reconciler

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"strings"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/kubernetes/scheme"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/remotecommand"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/log"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

// scrubScriptPath is the in-pod path scrub-pod.sh is COPYed to via
// the lifecycle directory link in infra/Dockerfile.
const scrubScriptPath = "/usr/local/bin/scrub-pod.sh"

// scrubMarker is the success line scrub-pod.sh writes on stdout.
const scrubMarker = "[scrub] OK"

// chromiumContainerName is the container we exec into. Must match
// the pod template in infra/k8s/cloud-browser-session.yaml + the
// helm chart.
const chromiumContainerName = "chromeless"

// ScrubExecutor abstracts Pod-exec so the reconciler doesn't carry
// a hard dependency on the kubernetes client-go path. Tests inject
// a fake; production uses RealScrubExecutor.
type ScrubExecutor interface {
	// Exec runs cmd inside the named container of pod/namespace and
	// returns its stdout, stderr, and any error from the streaming
	// itself. A non-zero exit code from the script comes back as a
	// remotecommand.CodeExitError in `err`.
	Exec(ctx context.Context, namespace, pod, container string, cmd []string) (stdout, stderr string, err error)
}

// RealScrubExecutor uses k8s.io/client-go's SPDY exec path to run
// commands inside running pods. Construct with NewRealScrubExecutor.
type RealScrubExecutor struct {
	cfg       *rest.Config
	clientset kubernetes.Interface
}

// NewRealScrubExecutor builds an executor against the cluster cfg
// the controller-runtime Manager already owns.
func NewRealScrubExecutor(cfg *rest.Config) (*RealScrubExecutor, error) {
	cs, err := kubernetes.NewForConfig(cfg)
	if err != nil {
		return nil, fmt.Errorf("clientset: %w", err)
	}
	return &RealScrubExecutor{cfg: cfg, clientset: cs}, nil
}

// Exec runs the command and returns stdout, stderr, error.
func (e *RealScrubExecutor) Exec(ctx context.Context, namespace, pod, container string, cmd []string) (string, string, error) {
	req := e.clientset.CoreV1().RESTClient().Post().
		Resource("pods").
		Name(pod).
		Namespace(namespace).
		SubResource("exec").
		VersionedParams(&corev1.PodExecOptions{
			Container: container,
			Command:   cmd,
			Stdin:     false,
			Stdout:    true,
			Stderr:    true,
			TTY:       false,
		}, scheme.ParameterCodec)
	exec, err := remotecommand.NewSPDYExecutor(e.cfg, "POST", req.URL())
	if err != nil {
		return "", "", fmt.Errorf("spdy exec: %w", err)
	}
	var stdout, stderr bytes.Buffer
	streamErr := exec.StreamWithContext(ctx, remotecommand.StreamOptions{
		Stdout: &stdout,
		Stderr: &stderr,
		Tty:    false,
	})
	return stdout.String(), stderr.String(), streamErr
}

// ----- metrics -----

var (
	mPodRecycled = promauto.NewCounterVec(prometheus.CounterOpts{
		Name: "cb_pool_pod_recycled_total",
		Help: "Pod recycle count by recycle policy. " +
			"`scrub_and_return` = pod stayed alive and went back to warm; " +
			"`recreate_pod` = pod was deleted and replenishment will create a new one; " +
			"`scrub_failed_fallback_recreate` = ScrubAndReturn was attempted, failed, and the controller fell back to delete.",
	}, []string{"policy"})
)

func init() {
	// Pre-register so /metrics has the series at startup, before the
	// first session ends.
	for _, p := range []string{
		"scrub_and_return",
		"recreate_pod",
		"scrub_failed_fallback_recreate",
	} {
		mPodRecycled.WithLabelValues(p)
	}
}

func recordPodRecycled(policy string) {
	mPodRecycled.WithLabelValues(policy).Inc()
}

// ----- public API -----

// ErrScrubVerifyFailed signals the post-scrub verify step (or the
// stdout marker check) didn't pass. Callers fall back to RecreatePod.
var ErrScrubVerifyFailed = errors.New("scrub verification failed")

// TryScrub runs scrub-pod.sh inside `pod`, verifies the success
// marker, and on success relabels the pod as warm so the
// pool reconciler picks it up on its next tick. Returns:
//
//	(true, nil)            scrub OK; pod is now warm.
//	(false, ErrScrubVerifyFailed) scrub returned but didn't verify;
//	                       caller should RecreatePod.
//	(false, otherErr)      a more fundamental failure (exec
//	                       transport, k8s API write); caller should
//	                       still RecreatePod and surface the error.
func TryScrub(
	ctx context.Context,
	c client.Client,
	scheme *runtime.Scheme,
	exec ScrubExecutor,
	pod *corev1.Pod,
	sess *cbv1.BrowserSession,
	pool *cbv1.BrowserSessionPool,
) (bool, error) {
	if exec == nil {
		// No executor configured (tests can also pass nil deliberately
		// to force a RecreatePod fallback).
		return false, fmt.Errorf("no ScrubExecutor configured")
	}

	stdout, stderr, err := exec.Exec(ctx, pod.Namespace, pod.Name, chromiumContainerName, []string{scrubScriptPath})
	if err != nil {
		return false, fmt.Errorf("scrub exec: %w (stderr: %s)", err, strings.TrimSpace(stderr))
	}
	if !strings.Contains(stdout, scrubMarker) {
		return false, fmt.Errorf("%w: stdout did not contain %q (stderr: %s)",
			ErrScrubVerifyFailed, scrubMarker, strings.TrimSpace(stderr))
	}

	// Relabel pod: drop ownership labels + annotations; flip state to
	// warm; drop the BrowserSession ownerRef so deleting the session
	// doesn't garbage-collect the pod via cascade.
	if pod.Labels == nil {
		pod.Labels = map[string]string{}
	}
	pod.Labels[cbv1.LabelSessionState] = cbv1.LabelSessionStateWarm
	delete(pod.Labels, cbv1.LabelSessionOwner)
	delete(pod.Labels, cbv1.LabelSessionTenant)
	if pod.Annotations != nil {
		delete(pod.Annotations, cbv1.AnnotationSessionID)
	}
	// Remove the BrowserSession owner ref. We keep any other refs
	// (e.g., a future BrowserSessionPool ownerRef) intact.
	filtered := pod.OwnerReferences[:0]
	for _, ref := range pod.OwnerReferences {
		if ref.Kind == "BrowserSession" {
			continue
		}
		filtered = append(filtered, ref)
	}
	pod.OwnerReferences = filtered

	if err := c.Update(ctx, pod); err != nil {
		// The scrub itself succeeded — the pod is wiped — but we
		// failed to re-label. Worst case the pod stays in
		// `state=assigned` until something nudges it; treat as a
		// recoverable error and let the caller decide.
		return false, fmt.Errorf("relabel pod: %w", err)
	}
	return true, nil
}

// scrubOrDelete is the dispatcher used by the session reconciler in
// completeDrain. It encapsulates the "look up the pool, ask its
// recyclePolicy, pick the path, record metrics, fall back on
// failure" pattern so completeDrain stays readable.
//
// Returns the bound pod's ultimate fate as a string suitable for
// log messages: "scrub_and_return", "recreate_pod", or
// "scrub_failed_fallback_recreate".
func (r *SessionReconciler) scrubOrDelete(
	ctx context.Context,
	pod *corev1.Pod,
	sess *cbv1.BrowserSession,
) (string, error) {
	// Find the pool the pod was minted from.
	poolName := pod.Labels[cbv1.LabelSessionPool]
	if poolName == "" {
		// No pool label -> nothing for ScrubAndReturn to target.
		// RecreatePod is the only path.
		if err := r.deleteBoundPod(ctx, pod); err != nil {
			return "", err
		}
		recordPodRecycled("recreate_pod")
		return "recreate_pod", nil
	}
	var pool cbv1.BrowserSessionPool
	if err := r.Get(ctx, client.ObjectKey{Namespace: sess.Namespace, Name: poolName}, &pool); err != nil {
		// Pool missing — degrade to RecreatePod and don't fail the
		// reconcile loop over a deleted pool.
		if err := r.deleteBoundPod(ctx, pod); err != nil {
			return "", err
		}
		recordPodRecycled("recreate_pod")
		return "recreate_pod", nil
	}

	// Default policy = RecreatePod.
	policy := pool.Spec.RecyclePolicy
	if policy == "" {
		policy = cbv1.PoolRecreatePod
	}
	if policy != cbv1.PoolScrubAndReturn || r.ScrubExec == nil {
		if err := r.deleteBoundPod(ctx, pod); err != nil {
			return "", err
		}
		recordPodRecycled("recreate_pod")
		return "recreate_pod", nil
	}

	// ScrubAndReturn path.
	ok, scrubErr := TryScrub(ctx, r.Client, r.Scheme, r.ScrubExec, pod, sess, &pool)
	if ok {
		recordPodRecycled("scrub_and_return")
		return "scrub_and_return", nil
	}
	// Scrub failed: fall back. The pod's state is unknown (it might be
	// halfway through a scrub); the safest move is to delete and let
	// the pool reconciler create a fresh one. We record the
	// `scrub_failed_fallback_recreate` metric so the failure is
	// observable, log the underlying scrub error at WARN level, and
	// return nil — the fallback succeeded, there's nothing for the
	// reconciler to retry.
	if delErr := r.deleteBoundPod(ctx, pod); delErr != nil {
		// Both failed: this IS a retry-worthy error.
		return "", fmt.Errorf("scrub failed (%v); fallback delete also failed: %w", scrubErr, delErr)
	}
	recordPodRecycled("scrub_failed_fallback_recreate")
	log.FromContext(ctx).Info("scrub failed; fell back to RecreatePod",
		"pod", pod.Name, "scrub_err", scrubErr.Error())
	return "scrub_failed_fallback_recreate", nil
}

// deleteBoundPod is the RecreatePod step factored out so both the
// happy path and the scrub-fallback path can reuse it.
func (r *SessionReconciler) deleteBoundPod(ctx context.Context, pod *corev1.Pod) error {
	if err := r.Delete(ctx, pod); err != nil && !apierrors.IsNotFound(err) {
		return err
	}
	return nil
}

// Package reconciler — pool.go
//
// PoolReconciler maintains the warm Pod count for each
// BrowserSessionPool. The two responsibilities are:
//
//   1. Replenishment — keep len(chromeless.session/state=warm) >=
//      spec.warmReplicas. Materialises Pods from the pool template
//      via CreateWarmPod.
//   2. Aging — Pods older than spec.maxAgeSeconds get drained so
//      image rolls cycle through the pool naturally. Active sessions
//      are unaffected; only warm Pods cycle.
//
// We do not enforce spec.maxSessions here; the session reconciler
// consults it before assignment. Same for ScrubAndReturn — that's a
// session-side decision triggered by the recycle policy.

package reconciler

import (
	"context"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"sigs.k8s.io/controller-runtime/pkg/builder"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/controller"
	"sigs.k8s.io/controller-runtime/pkg/log"
	"sigs.k8s.io/controller-runtime/pkg/manager"
	"sigs.k8s.io/controller-runtime/pkg/reconcile"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

// PoolReconciler reconciles BrowserSessionPool objects.
type PoolReconciler struct {
	client.Client
	Scheme *runtime.Scheme
}

// SetupWithManager wires this reconciler in. We watch Pods because
// state-label changes (warm -> assigned -> draining) are what
// actually drive the count we react to.
func (r *PoolReconciler) SetupWithManager(mgr manager.Manager) error {
	return builder.ControllerManagedBy(mgr).
		For(&cbv1.BrowserSessionPool{}).
		Owns(&corev1.Pod{}).
		WithOptions(controller.Options{MaxConcurrentReconciles: 2}).
		Named("browser-session-pool").
		Complete(r)
}

// +kubebuilder:rbac:groups=cloud-browser-webrtc.example.com,resources=browsersessionpools,verbs=get;list;watch;create;update;patch
// +kubebuilder:rbac:groups=cloud-browser-webrtc.example.com,resources=browsersessionpools/status,verbs=get;update;patch
// +kubebuilder:rbac:groups="",resources=pods,verbs=get;list;watch;create;update;patch;delete

// Reconcile is straightforward: count, replenish if short, drain if
// over-aged. T99 wraps each reconcile pass in a
// `chromeless.controller.pool.replenish` span so the operator can drill into
// "why did this pool's warm count drop?" by looking at the span
// timeline rather than correlating across log lines.
func (r *PoolReconciler) Reconcile(ctx context.Context, req reconcile.Request) (reconcile.Result, error) {
	ctx, span := tracingTracer("reconciler.pool").Start(ctx, "chromeless.controller.pool.replenish",
		traceWithAttrs(
			tracingAttrString("pool.namespace", req.Namespace),
			tracingAttrString("pool.name", req.Name),
		),
	)
	defer span.End()
	logger := log.FromContext(ctx).WithValues("pool", req.NamespacedName)

	var pool cbv1.BrowserSessionPool
	if err := r.Get(ctx, req.NamespacedName, &pool); err != nil {
		if apierrors.IsNotFound(err) {
			return reconcile.Result{}, nil
		}
		return reconcile.Result{}, err
	}

	var pods corev1.PodList
	if err := r.List(ctx, &pods,
		client.InNamespace(pool.Namespace),
		client.MatchingLabels{cbv1.LabelSessionPool: pool.Name},
	); err != nil {
		return reconcile.Result{}, err
	}

	var warm, assigned, draining int32
	maxAge := time.Duration(pool.Spec.MaxAgeSeconds) * time.Second
	if maxAge <= 0 {
		maxAge = time.Hour
	}
	now := time.Now()

	for i := range pods.Items {
		pod := &pods.Items[i]
		if !pod.DeletionTimestamp.IsZero() {
			continue
		}
		switch pod.Labels[cbv1.LabelSessionState] {
		case cbv1.LabelSessionStateWarm:
			// Age check first; over-aged warm Pods are drained, not
			// counted toward warm capacity.
			if now.Sub(pod.CreationTimestamp.Time) > maxAge {
				logger.Info("draining over-aged warm pod", "pod", pod.Name)
				if err := r.drainWarmPod(ctx, pod); err != nil {
					return reconcile.Result{}, err
				}
				draining++
				continue
			}
			warm++
		case cbv1.LabelSessionStateAssign:
			assigned++
		case cbv1.LabelSessionStateDrain:
			draining++
		}
	}

	// --- replenishment ---
	target := pool.Spec.WarmReplicas
	created := int32(0)
	for warm+created < target {
		if pool.Spec.MaxSessions > 0 && warm+assigned+created >= pool.Spec.MaxSessions {
			logger.Info("replenishment skipped: maxSessions reached",
				"warm", warm, "assigned", assigned, "max", pool.Spec.MaxSessions)
			break
		}
		_, err := CreateWarmPod(ctx, r.Client, r.Scheme, &pool)
		if err != nil {
			return reconcile.Result{}, err
		}
		created++
	}
	if created > 0 {
		logger.Info("replenished warm pool", "created", created, "warm_before", warm, "target", target)
	}

	// --- status ---
	tNow := metav1.NewTime(now)
	pool.Status.Warm = warm + created
	pool.Status.Active = assigned
	pool.Status.Draining = draining
	pool.Status.TotalEverProvisioned += int64(created)
	pool.Status.LastReplenishedAt = &tNow
	if err := r.Status().Update(ctx, &pool); err != nil {
		return reconcile.Result{}, err
	}

	// Re-queue at half maxAge so we naturally catch over-aged Pods
	// without needing a Pod-status event.
	requeue := maxAge / 2
	if requeue < 30*time.Second {
		requeue = 30 * time.Second
	}
	return reconcile.Result{RequeueAfter: requeue}, nil
}

// drainWarmPod transitions a warm Pod to draining. We don't delete
// here; controller-runtime's Pod-watch will see the label change and
// the next reconcile will count it under draining. A separate K8s
// PodLifetimeController (or this same reconciler in the future) does
// the actual termination.
func (r *PoolReconciler) drainWarmPod(ctx context.Context, pod *corev1.Pod) error {
	if pod.Labels[cbv1.LabelSessionState] == cbv1.LabelSessionStateDrain {
		return nil
	}
	pod.Labels[cbv1.LabelSessionState] = cbv1.LabelSessionStateDrain
	// Phase-1 simplification: we delete the Pod here directly. Phase 3
	// adds a SIGTERM-then-await-shutdown dance.
	if err := r.Delete(ctx, pod); err != nil && !apierrors.IsNotFound(err) {
		return err
	}
	return nil
}

// Package reconciler — session.go
//
// SessionReconciler implements the assignment + drain loops for the
// BrowserSession CR per T50's design doc. The high-level flow on
// Reconcile:
//
//   1. Load the BrowserSession.
//   2. Fast paths:
//      - Already Ended -> nothing to do.
//      - DeletionTimestamp set -> finalize: drain owned Pod, then
//        remove our finalizer.
//   3. Phase progression:
//      Pending -> Warming/Ready  (assign a Pod)
//      Ready   -> Draining       (idle eviction)
//      Draining -> Ended
//
// The pool-side replenishment loop is in pool.go; this reconciler
// expects warm Pods to be there when it asks.

package reconciler

import (
	"context"
	"fmt"
	"strings"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	"sigs.k8s.io/controller-runtime/pkg/builder"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/controller"
	"sigs.k8s.io/controller-runtime/pkg/controller/controllerutil"
	"sigs.k8s.io/controller-runtime/pkg/log"
	"sigs.k8s.io/controller-runtime/pkg/manager"
	"sigs.k8s.io/controller-runtime/pkg/reconcile"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

// SessionFinalizer keeps the BrowserSession around until our drain
// loop has had a chance to clean up the owned Pod. Without this, a
// `kubectl delete browsersession ...` would orphan the Pod under our
// label selector.
const SessionFinalizer = "cloud-browser-webrtc.example.com/session"

// signalingHostFmt is the in-cluster signaling URL we hand back to
// clients via BrowserSession.status.connection. Keeping it as a fmt
// template lets a Phase 3 deploy override per session for
// region-affine routing without touching the controller.
const signalingHostFmt = "ws://signaling.%s.svc.cluster.local:8080/ws/%s"

// SessionReconciler reconciles BrowserSession objects.
type SessionReconciler struct {
	client.Client
	Scheme *runtime.Scheme

	// DefaultPool names the BrowserSessionPool a session falls back to
	// when spec.poolName is empty.
	DefaultPool string

	// ScrubExec drives pod-exec for the ScrubAndReturn recycle path
	// (T90). Nil = ScrubAndReturn is unavailable; the controller
	// silently degrades to RecreatePod even if a pool requests
	// ScrubAndReturn. Production main wires NewRealScrubExecutor;
	// tests inject fakes.
	ScrubExec ScrubExecutor
}

// SetupWithManager wires this reconciler into a controller-runtime
// manager. We watch BrowserSession for spec/status changes and Pods
// for the owner-reference back-link (a Pod's status changing should
// re-trigger the session reconcile so we can move Pending -> Ready
// when the assigned Pod becomes ready).
func (r *SessionReconciler) SetupWithManager(mgr manager.Manager) error {
	return builder.ControllerManagedBy(mgr).
		For(&cbv1.BrowserSession{}).
		Owns(&corev1.Pod{}).
		WithOptions(controller.Options{MaxConcurrentReconciles: 4}).
		Named("browser-session").
		Complete(r)
}

// +kubebuilder:rbac:groups=cloud-browser-webrtc.example.com,resources=browsersessions,verbs=get;list;watch;create;update;patch;delete
// +kubebuilder:rbac:groups=cloud-browser-webrtc.example.com,resources=browsersessions/status,verbs=get;update;patch
// +kubebuilder:rbac:groups=cloud-browser-webrtc.example.com,resources=browsersessions/finalizers,verbs=update
// +kubebuilder:rbac:groups=cloud-browser-webrtc.example.com,resources=browsersessionpools,verbs=get;list;watch
// +kubebuilder:rbac:groups="",resources=pods,verbs=get;list;watch;create;update;patch;delete

// Reconcile implements the session lifecycle described in the file
// header.
func (r *SessionReconciler) Reconcile(ctx context.Context, req reconcile.Request) (reconcile.Result, error) {
	log := log.FromContext(ctx).WithValues("session", req.NamespacedName)

	var sess cbv1.BrowserSession
	if err := r.Get(ctx, req.NamespacedName, &sess); err != nil {
		if apierrors.IsNotFound(err) {
			return reconcile.Result{}, nil
		}
		return reconcile.Result{}, err
	}

	// Deletion path — drain Pod, remove finalizer.
	if !sess.DeletionTimestamp.IsZero() {
		return r.finalize(ctx, &sess)
	}

	// Ensure finalizer is present so we get a chance to drain.
	if !controllerutil.ContainsFinalizer(&sess, SessionFinalizer) {
		controllerutil.AddFinalizer(&sess, SessionFinalizer)
		if err := r.Update(ctx, &sess); err != nil {
			return reconcile.Result{}, err
		}
		// Re-queue; we'll continue on the next reconcile with the
		// finalizer in place.
		return reconcile.Result{Requeue: true}, nil
	}

	switch sess.Status.Phase {
	case "":
		return r.transitionToPending(ctx, &sess)
	case cbv1.SessionPending, cbv1.SessionWarming:
		return r.tryAssign(ctx, &sess)
	case cbv1.SessionReady:
		return r.checkIdle(ctx, &sess)
	case cbv1.SessionDraining:
		return r.completeDrain(ctx, &sess)
	case cbv1.SessionEnded:
		// Terminal; nothing to do unless deletion arrives.
		return reconcile.Result{}, nil
	default:
		log.Info("unknown phase, treating as Pending", "phase", sess.Status.Phase)
		return r.transitionToPending(ctx, &sess)
	}
}

// transitionToPending sets the phase and returns immediately so the
// next reconcile picks up the assignment.
func (r *SessionReconciler) transitionToPending(ctx context.Context, sess *cbv1.BrowserSession) (reconcile.Result, error) {
	sess.Status.Phase = cbv1.SessionPending
	if err := r.Status().Update(ctx, sess); err != nil {
		return reconcile.Result{}, err
	}
	return reconcile.Result{Requeue: true}, nil
}

// tryAssign looks for a warm Pod in the requested pool, binds it to
// this session, and transitions to Ready when the Pod is healthy.
//
// If no warm Pod is available, we cold-start one by creating a Pod
// from the pool template and stay in Warming until it passes
// readiness.
//
// T99: emits `chromeless.controller.session.assign` span for the assignment
// attempt. The span attributes record which path (warm/cold) we
// took and the bound pod name; failures are recorded as span events.
func (r *SessionReconciler) tryAssign(ctx context.Context, sess *cbv1.BrowserSession) (reconcile.Result, error) {
	ctx, span := tracingTracer("reconciler.session").Start(ctx, "chromeless.controller.session.assign",
		traceWithAttrs(
			tracingAttrString("session.name", sess.Name),
			tracingAttrString("tenant.id", sess.Spec.TenantID),
			tracingAttrString("pool.name", sess.Spec.PoolName),
			tracingAttrString("region", sess.Spec.Region),
		),
	)
	defer span.End()
	log := log.FromContext(ctx)

	poolName := sess.Spec.PoolName
	if poolName == "" {
		poolName = r.DefaultPool
	}
	var pool cbv1.BrowserSessionPool
	if err := r.Get(ctx, client.ObjectKey{Namespace: sess.Namespace, Name: poolName}, &pool); err != nil {
		return reconcile.Result{}, fmt.Errorf("get pool %q: %w", poolName, err)
	}

	// 1. Already-bound? Pod label chromeless.session/owner = sess.Name.
	bound, err := r.findBoundPod(ctx, sess)
	if err != nil {
		return reconcile.Result{}, err
	}
	if bound != nil {
		return r.advanceFromBoundPod(ctx, sess, bound)
	}

	// 2. Pick a warm Pod.
	warm, err := r.pickWarmPod(ctx, sess, &pool)
	if err != nil {
		return reconcile.Result{}, err
	}
	if warm != nil {
		return r.bindToPod(ctx, sess, warm)
	}

	// 3. Cold-start: create a fresh Pod from the pool template.
	log.Info("warm pool empty; cold-starting", "pool", poolName)
	created, err := r.createPodFromTemplate(ctx, sess, &pool, cbv1.LabelSessionStateAssign)
	if err != nil {
		return reconcile.Result{}, err
	}
	return r.bindToPod(ctx, sess, created)
}

// findBoundPod returns the Pod owned by this session, or nil.
func (r *SessionReconciler) findBoundPod(ctx context.Context, sess *cbv1.BrowserSession) (*corev1.Pod, error) {
	var pods corev1.PodList
	if err := r.List(ctx, &pods,
		client.InNamespace(sess.Namespace),
		client.MatchingLabels{cbv1.LabelSessionOwner: sess.Name},
	); err != nil {
		return nil, err
	}
	for i := range pods.Items {
		if pods.Items[i].DeletionTimestamp.IsZero() {
			return &pods.Items[i], nil
		}
	}
	return nil, nil
}

// pickWarmPod returns the first chromeless.session/state=warm Pod in the pool
// matching the session's region (if specified). Round-robin within
// the candidate set is not strictly necessary — the controller's
// assignment loop is single-threaded per pool by leader election so
// adjacent reconciles see different first elements naturally.
func (r *SessionReconciler) pickWarmPod(ctx context.Context, sess *cbv1.BrowserSession, pool *cbv1.BrowserSessionPool) (*corev1.Pod, error) {
	needsSessionScopedStreamer := sess.Annotations[cbv1.AnnotationBrowserSignalingURL] != "" ||
		sess.Annotations[cbv1.AnnotationBrowserSignalingToken] != ""

	var pods corev1.PodList
	if err := r.List(ctx, &pods,
		client.InNamespace(sess.Namespace),
		client.MatchingLabels{
			cbv1.LabelSessionState: cbv1.LabelSessionStateWarm,
			cbv1.LabelSessionPool:  pool.Name,
		},
	); err != nil {
		return nil, err
	}
	for i := range pods.Items {
		p := &pods.Items[i]
		if !p.DeletionTimestamp.IsZero() {
			continue
		}
		if sess.Spec.Region != "" {
			if p.Labels["topology.kubernetes.io/region"] != sess.Spec.Region {
				continue
			}
		}
		// Don't grab a Pod that's not ready yet.
		if !podReady(p) {
			continue
		}
		if needsSessionScopedStreamer && !streamerAutostartDisabled(p) {
			// Session-scoped Triform signaling used to be injected via
			// immutable pod env, so autostarting warm pods cannot be safely
			// rebound. Pods with autostart disabled are launched by Triform
			// over CDP with per-session URL params, so they are safe to reuse.
			continue
		}
		return p, nil
	}
	return nil, nil
}

func streamerAutostartDisabled(pod *corev1.Pod) bool {
	if pod == nil {
		return false
	}
	for i := range pod.Spec.Containers {
		container := &pod.Spec.Containers[i]
		if container.Name != "chromeless" {
			continue
		}
		for _, item := range container.Env {
			if item.Name != "CHROMELESS_AUTOSTART_STREAMER" {
				continue
			}
			switch strings.ToLower(strings.TrimSpace(item.Value)) {
			case "0", "false", "no", "off":
				return true
			default:
				return false
			}
		}
		return false
	}
	return false
}

// bindToPod labels the Pod as assigned and updates the session status.
func (r *SessionReconciler) bindToPod(ctx context.Context, sess *cbv1.BrowserSession, pod *corev1.Pod) (reconcile.Result, error) {
	if pod.Labels == nil {
		pod.Labels = map[string]string{}
	}
	pod.Labels[cbv1.LabelSessionState] = cbv1.LabelSessionStateAssign
	pod.Labels[cbv1.LabelSessionOwner] = sess.Name
	tenant := sess.Spec.TenantID
	if tenant == "" {
		tenant = cbv1.AnonymousTenant
	}
	pod.Labels[cbv1.LabelSessionTenant] = tenant
	if pod.Annotations == nil {
		pod.Annotations = map[string]string{}
	}
	pod.Annotations[cbv1.AnnotationSessionID] = string(sess.UID)

	// Cold-start pods are session-owned. Warm pods are owned by their pool so
	// the pool reconciler sees state changes and replenishes immediately; the
	// session finalizer still drains them by chromeless.session/owner label.
	if !hasControllerOwnerRef(pod, "BrowserSessionPool") {
		if err := controllerutil.SetControllerReference(sess, pod, r.Scheme); err != nil {
			return reconcile.Result{}, err
		}
	}
	if err := r.Update(ctx, pod); err != nil {
		return reconcile.Result{}, err
	}

	return r.advanceFromBoundPod(ctx, sess, pod)
}

func hasControllerOwnerRef(pod *corev1.Pod, kind string) bool {
	if pod == nil {
		return false
	}
	for _, ref := range pod.OwnerReferences {
		if ref.Kind == kind && ref.Controller != nil && *ref.Controller {
			return true
		}
	}
	return false
}

// advanceFromBoundPod sets the session phase based on the Pod's
// current readiness. Ready when the Pod is Ready and IP-assigned;
// Warming otherwise.
func (r *SessionReconciler) advanceFromBoundPod(ctx context.Context, sess *cbv1.BrowserSession, pod *corev1.Pod) (reconcile.Result, error) {
	if !podReady(pod) || pod.Status.PodIP == "" {
		sess.Status.Phase = cbv1.SessionWarming
		if err := r.Status().Update(ctx, sess); err != nil {
			return reconcile.Result{}, err
		}
		// Re-queue; Owns(&corev1.Pod{}) means a Pod-status update will
		// also trigger us, but a periodic backstop keeps us honest.
		return reconcile.Result{RequeueAfter: 2 * time.Second}, nil
	}

	now := metav1.NewTime(time.Now())
	sess.Status.Phase = cbv1.SessionReady
	if sess.Status.StartedAt == nil {
		sess.Status.StartedAt = &now
	}
	sess.Status.LastActivityAt = &now
	sess.Status.Connection = &cbv1.SessionConnection{
		SignalingURL: signalingURLForSession(sess),
		PodName:      pod.Name,
		PodIP:        pod.Status.PodIP,
	}
	if err := r.Status().Update(ctx, sess); err != nil {
		return reconcile.Result{}, err
	}
	return reconcile.Result{}, nil
}

func signalingURLForSession(sess *cbv1.BrowserSession) string {
	if sess.Annotations[cbv1.AnnotationBrowserSignalingURL] != "" {
		return sess.Annotations[cbv1.AnnotationBrowserSignalingURL]
	}
	return fmt.Sprintf(signalingHostFmt, sess.Namespace, sess.Name)
}

func brokerSessionIDForSession(sess *cbv1.BrowserSession) string {
	if sess.Annotations[cbv1.AnnotationBrokerSessionID] != "" {
		return sess.Annotations[cbv1.AnnotationBrokerSessionID]
	}
	return sess.Name
}

func applyAssignedSessionEnv(sess *cbv1.BrowserSession, pod *corev1.Pod) {
	if pod == nil {
		return
	}
	for i := range pod.Spec.Containers {
		if pod.Spec.Containers[i].Name != "chromeless" {
			continue
		}
		upsertEnv(&pod.Spec.Containers[i], "SESSION_ID", brokerSessionIDForSession(sess))
		if v := sess.Annotations[cbv1.AnnotationBrowserSignalingURL]; v != "" {
			upsertEnv(&pod.Spec.Containers[i], "SIGNALING_URL", v)
		}
		if v := sess.Annotations[cbv1.AnnotationBrowserSignalingToken]; v != "" {
			upsertEnv(&pod.Spec.Containers[i], "SIGNALING_TOKEN", v)
		}
		return
	}
}

func upsertEnv(container *corev1.Container, name, value string) {
	for i := range container.Env {
		if container.Env[i].Name == name {
			container.Env[i].Value = value
			container.Env[i].ValueFrom = nil
			return
		}
	}
	container.Env = append(container.Env, corev1.EnvVar{Name: name, Value: value})
}

// checkIdle decides whether to drain a Ready session.
func (r *SessionReconciler) checkIdle(ctx context.Context, sess *cbv1.BrowserSession) (reconcile.Result, error) {
	timeout := time.Duration(sess.Spec.IdleTimeoutSeconds) * time.Second
	if timeout <= 0 {
		timeout = 600 * time.Second
	}
	last := sess.Status.LastActivityAt
	if last == nil {
		// Shouldn't happen; treat as start.
		now := metav1.NewTime(time.Now())
		sess.Status.LastActivityAt = &now
		_ = r.Status().Update(ctx, sess)
		return reconcile.Result{RequeueAfter: timeout}, nil
	}
	idleFor := time.Since(last.Time)
	if idleFor < timeout {
		// Re-queue when the threshold could fire.
		return reconcile.Result{RequeueAfter: timeout - idleFor}, nil
	}

	sess.Status.Phase = cbv1.SessionDraining
	if err := r.Status().Update(ctx, sess); err != nil {
		return reconcile.Result{}, err
	}
	return reconcile.Result{Requeue: true}, nil
}

// completeDrain finishes the drain. Picks between ScrubAndReturn
// (pod stays alive, returns to warm) and RecreatePod (pod is
// deleted; replenishment loop creates a fresh one) per the pool's
// `spec.recyclePolicy`. T90.
func (r *SessionReconciler) completeDrain(ctx context.Context, sess *cbv1.BrowserSession) (reconcile.Result, error) {
	bound, err := r.findBoundPod(ctx, sess)
	if err != nil {
		return reconcile.Result{}, err
	}
	if bound != nil {
		fate, err := r.scrubOrDelete(ctx, bound, sess)
		if err != nil {
			return reconcile.Result{}, err
		}
		log.FromContext(ctx).Info("session drain complete",
			"pod", bound.Name, "fate", fate)
	}

	now := metav1.NewTime(time.Now())
	sess.Status.Phase = cbv1.SessionEnded
	sess.Status.EndedAt = &now
	if sess.Status.EndReason == "" {
		sess.Status.EndReason = "IdleTimeout"
	}
	sess.Status.Connection = nil
	if err := r.Status().Update(ctx, sess); err != nil {
		return reconcile.Result{}, err
	}
	return reconcile.Result{}, nil
}

// finalize is the kubectl-delete path.
func (r *SessionReconciler) finalize(ctx context.Context, sess *cbv1.BrowserSession) (reconcile.Result, error) {
	bound, err := r.findBoundPod(ctx, sess)
	if err != nil {
		return reconcile.Result{}, err
	}
	if bound != nil && bound.DeletionTimestamp.IsZero() {
		if err := r.Delete(ctx, bound); err != nil && !apierrors.IsNotFound(err) {
			return reconcile.Result{}, err
		}
	}

	if controllerutil.RemoveFinalizer(sess, SessionFinalizer) {
		if err := r.Update(ctx, sess); err != nil {
			return reconcile.Result{}, err
		}
	}
	return reconcile.Result{}, nil
}

// createPodFromTemplate materialises a Pod from the pool's template
// labelled with the requested initial state. Used by both the
// session reconciler (cold-start fallback) and the pool reconciler
// (warm replenishment).
//
// Public so pool.go can use it; lives here because it shares all the
// label/annotation conventions with bindToPod.
func (r *SessionReconciler) createPodFromTemplate(ctx context.Context, sess *cbv1.BrowserSession, pool *cbv1.BrowserSessionPool, initialState string) (*corev1.Pod, error) {
	pod := &corev1.Pod{
		ObjectMeta: *pool.Spec.Template.ObjectMeta.DeepCopy(),
		Spec:       *pool.Spec.Template.Spec.DeepCopy(),
	}
	pod.Namespace = sess.Namespace
	// Make GenerateName-friendly: prefix from the pool, suffix random.
	pod.Name = ""
	if pod.GenerateName == "" {
		pod.GenerateName = fmt.Sprintf("%s-", strings.ToLower(pool.Name))
	}
	if pod.Labels == nil {
		pod.Labels = map[string]string{}
	}
	pod.Labels[cbv1.LabelSessionState] = initialState
	pod.Labels[cbv1.LabelSessionPool] = pool.Name
	if initialState == cbv1.LabelSessionStateAssign {
		pod.Labels[cbv1.LabelSessionOwner] = sess.Name
		applyAssignedSessionEnv(sess, pod)
	}
	if err := controllerutil.SetControllerReference(sess, pod, r.Scheme); err != nil {
		return nil, err
	}
	if err := r.Create(ctx, pod); err != nil {
		return nil, err
	}
	return pod, nil
}

// CreateWarmPod is the pool reconciler's entry point into the
// template-creation logic. The session arg is nil (warm Pods aren't
// owned by a session yet); the pool itself becomes the controller-ref.
func CreateWarmPod(ctx context.Context, c client.Client, scheme *runtime.Scheme, pool *cbv1.BrowserSessionPool) (*corev1.Pod, error) {
	pod := &corev1.Pod{
		ObjectMeta: *pool.Spec.Template.ObjectMeta.DeepCopy(),
		Spec:       *pool.Spec.Template.Spec.DeepCopy(),
	}
	pod.Namespace = pool.Namespace
	pod.Name = ""
	if pod.GenerateName == "" {
		pod.GenerateName = fmt.Sprintf("%s-warm-", strings.ToLower(pool.Name))
	}
	if pod.Labels == nil {
		pod.Labels = map[string]string{}
	}
	pod.Labels[cbv1.LabelSessionState] = cbv1.LabelSessionStateWarm
	pod.Labels[cbv1.LabelSessionPool] = pool.Name
	if err := controllerutil.SetControllerReference(pool, pod, scheme); err != nil {
		return nil, err
	}
	if err := c.Create(ctx, pod); err != nil {
		return nil, err
	}
	return pod, nil
}

// podReady is a helper: returns true if the Pod has Ready condition
// = True.
func podReady(pod *corev1.Pod) bool {
	if pod == nil {
		return false
	}
	for _, c := range pod.Status.Conditions {
		if c.Type == corev1.PodReady {
			return c.Status == corev1.ConditionTrue
		}
	}
	return false
}

// keyOf is a small helper used by some tests; lets them treat a
// BrowserSession as an objectKey without importing client.ObjectKey.
func keyOf(sess *cbv1.BrowserSession) types.NamespacedName {
	return types.NamespacedName{Namespace: sess.Namespace, Name: sess.Name}
}

var _ = keyOf // keep symbol for tests

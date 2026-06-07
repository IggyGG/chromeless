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
	"net"
	"net/url"
	"strings"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/util/retry"
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
		if err := r.updateSession(ctx, &sess, func(latest *cbv1.BrowserSession) error {
			controllerutil.AddFinalizer(latest, SessionFinalizer)
			return nil
		}); err != nil {
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
	if err := r.updateSessionStatus(ctx, sess, func(latest *cbv1.BrowserSession) {
		latest.Status.Phase = cbv1.SessionPending
	}); err != nil {
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
	needsSessionScopedSignaling := sess.Annotations[cbv1.AnnotationBrowserSignalingURL] != "" ||
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
		if needsSessionScopedSignaling {
			// Session-scoped Triform signaling is injected through immutable
			// pod env. Native WebRTC reads WEBRTC_SIGNALING_* at Chromium
			// launch, so warm pods cannot be safely rebound even when the
			// legacy streamer autostart is disabled.
			continue
		}
		return p, nil
	}
	return nil, nil
}

// bindToPod labels the Pod as assigned and updates the session status.
func (r *SessionReconciler) bindToPod(ctx context.Context, sess *cbv1.BrowserSession, pod *corev1.Pod) (reconcile.Result, error) {
	bound, err := r.assignPodToSession(ctx, sess, pod)
	if err != nil {
		return reconcile.Result{}, err
	}
	return r.advanceFromBoundPod(ctx, sess, bound)
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

func (r *SessionReconciler) assignPodToSession(ctx context.Context, sess *cbv1.BrowserSession, pod *corev1.Pod) (*corev1.Pod, error) {
	key := client.ObjectKeyFromObject(pod)
	latest := pod.DeepCopy()
	var assigned corev1.Pod
	first := true
	err := retry.RetryOnConflict(retry.DefaultRetry, func() error {
		if !first {
			if err := r.Get(ctx, key, latest); err != nil {
				return err
			}
		}
		first = false
		applyPodAssignment(sess, latest)

		// Cold-start pods are session-owned. Warm pods are owned by their pool so
		// the pool reconciler sees state changes and replenishes immediately; the
		// session finalizer still drains them by chromeless.session/owner label.
		if !hasControllerOwnerRef(latest, "BrowserSessionPool") {
			if err := controllerutil.SetControllerReference(sess, latest, r.Scheme); err != nil {
				return err
			}
		}
		if err := r.Update(ctx, latest); err != nil {
			return err
		}
		assigned = *latest
		return nil
	})
	if err != nil {
		return nil, err
	}
	return &assigned, nil
}

func applyPodAssignment(sess *cbv1.BrowserSession, pod *corev1.Pod) {
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
}

func (r *SessionReconciler) updateSessionStatus(
	ctx context.Context,
	sess *cbv1.BrowserSession,
	mutate func(*cbv1.BrowserSession),
) error {
	key := client.ObjectKeyFromObject(sess)
	return retry.RetryOnConflict(retry.DefaultRetry, func() error {
		var latest cbv1.BrowserSession
		if err := r.Get(ctx, key, &latest); err != nil {
			return err
		}
		mutate(&latest)
		if err := r.Status().Update(ctx, &latest); err != nil {
			return err
		}
		*sess = latest
		return nil
	})
}

func (r *SessionReconciler) updateSession(
	ctx context.Context,
	sess *cbv1.BrowserSession,
	mutate func(*cbv1.BrowserSession) error,
) error {
	key := client.ObjectKeyFromObject(sess)
	return retry.RetryOnConflict(retry.DefaultRetry, func() error {
		var latest cbv1.BrowserSession
		if err := r.Get(ctx, key, &latest); err != nil {
			return err
		}
		if err := mutate(&latest); err != nil {
			return err
		}
		if err := r.Update(ctx, &latest); err != nil {
			return err
		}
		*sess = latest
		return nil
	})
}

// advanceFromBoundPod sets the session phase based on the Pod's
// current readiness. Ready when the Pod is Ready and IP-assigned;
// Warming otherwise.
func (r *SessionReconciler) advanceFromBoundPod(ctx context.Context, sess *cbv1.BrowserSession, pod *corev1.Pod) (reconcile.Result, error) {
	if !podReady(pod) || pod.Status.PodIP == "" {
		if err := r.updateSessionStatus(ctx, sess, func(latest *cbv1.BrowserSession) {
			latest.Status.Phase = cbv1.SessionWarming
		}); err != nil {
			return reconcile.Result{}, err
		}
		// Re-queue; Owns(&corev1.Pod{}) means a Pod-status update will
		// also trigger us, but a periodic backstop keeps us honest.
		return reconcile.Result{RequeueAfter: 2 * time.Second}, nil
	}

	now := metav1.NewTime(time.Now())
	if err := r.updateSessionStatus(ctx, sess, func(latest *cbv1.BrowserSession) {
		latest.Status.Phase = cbv1.SessionReady
		if latest.Status.StartedAt == nil {
			latest.Status.StartedAt = &now
		}
		latest.Status.LastActivityAt = &now
		latest.Status.Connection = &cbv1.SessionConnection{
			SignalingURL: signalingURLForSession(latest),
			PodName:      pod.Name,
			PodIP:        pod.Status.PodIP,
		}
	}); err != nil {
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
		upsertEnv(&pod.Spec.Containers[i], "WEBRTC_SIGNALING_SESSION_ID", brokerSessionIDForSession(sess))
		if v := sess.Annotations[cbv1.AnnotationBrowserSignalingURL]; v != "" {
			upsertEnv(&pod.Spec.Containers[i], "SIGNALING_URL", v)
			if host, tls, ok := nativeSignalingEndpoint(v); ok {
				upsertEnv(&pod.Spec.Containers[i], "WEBRTC_SIGNALING_HOST", host)
				upsertEnv(&pod.Spec.Containers[i], "WEBRTC_SIGNALING_TLS", tls)
			}
		}
		if v := sess.Annotations[cbv1.AnnotationBrowserSignalingToken]; v != "" {
			upsertEnv(&pod.Spec.Containers[i], "SIGNALING_TOKEN", v)
			upsertEnv(&pod.Spec.Containers[i], "WEBRTC_SIGNALING_TOKEN", v)
		}
		return
	}
}

func nativeSignalingEndpoint(raw string) (host string, tls string, ok bool) {
	return nativeSignalingEndpointFromLookup(raw, net.LookupHost)
}

func nativeSignalingEndpointFromLookup(raw string, lookupHost func(string) ([]string, error)) (host string, tls string, ok bool) {
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" {
		return "", "", false
	}
	host = u.Host
	if lookupHost != nil {
		host = resolveNativeSignalingHost(host, lookupHost)
	}
	switch strings.ToLower(u.Scheme) {
	case "wss", "https":
		return host, "1", true
	case "ws", "http":
		return host, "0", true
	default:
		return "", "", false
	}
}

func resolveNativeSignalingHost(host string, lookupHost func(string) ([]string, error)) string {
	host = strings.TrimSpace(host)
	if host == "" {
		return host
	}

	name := host
	port := ""
	if parsedHost, parsedPort, err := net.SplitHostPort(host); err == nil {
		name = parsedHost
		port = parsedPort
	}

	// Native Chromium has repeatedly failed to resolve Kubernetes service DNS
	// names even when getent/curl work in the same pod. Keep the BrowserSession
	// annotation readable, but inject an IP:port into WEBRTC_SIGNALING_HOST for
	// in-cluster service URLs so the browser process bypasses that resolver path.
	if strings.HasSuffix(name, ".svc.cluster.local") {
		if ips, err := lookupHost(name); err == nil && len(ips) > 0 && ips[0] != "" {
			name = ips[0]
		}
	}

	if port == "" {
		return name
	}
	return net.JoinHostPort(name, port)
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
		_ = r.updateSessionStatus(ctx, sess, func(latest *cbv1.BrowserSession) {
			latest.Status.LastActivityAt = &now
		})
		return reconcile.Result{RequeueAfter: timeout}, nil
	}
	idleFor := time.Since(last.Time)
	if idleFor < timeout {
		// Re-queue when the threshold could fire.
		return reconcile.Result{RequeueAfter: timeout - idleFor}, nil
	}

	if err := r.updateSessionStatus(ctx, sess, func(latest *cbv1.BrowserSession) {
		latest.Status.Phase = cbv1.SessionDraining
	}); err != nil {
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
	if err := r.updateSessionStatus(ctx, sess, func(latest *cbv1.BrowserSession) {
		latest.Status.Phase = cbv1.SessionEnded
		latest.Status.EndedAt = &now
		if latest.Status.EndReason == "" {
			latest.Status.EndReason = "IdleTimeout"
		}
		latest.Status.Connection = nil
	}); err != nil {
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
		if err := r.updateSession(ctx, sess, func(latest *cbv1.BrowserSession) error {
			controllerutil.RemoveFinalizer(latest, SessionFinalizer)
			return nil
		}); err != nil {
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

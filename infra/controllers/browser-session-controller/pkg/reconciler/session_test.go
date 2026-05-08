// Package reconciler — session_test.go
//
// Table-driven tests for SessionReconciler using
// controller-runtime's fake client. Covers:
//
//   - empty pool -> cold-start path (Pod created, session Warming)
//   - warm pool -> fast assignment (existing warm Pod relabelled,
//     session goes to Warming/Ready depending on Pod readiness)
//   - drain on idle (LastActivityAt > IdleTimeoutSeconds in the past)
//   - finalizer removed on deletion (Pod GC'd via owner ref)
//
// We use fake client + a manually-constructed pool/template so the
// tests don't need an envtest binary.

package reconciler

import (
	"context"
	"testing"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/client/fake"
	"sigs.k8s.io/controller-runtime/pkg/controller/controllerutil"
	"sigs.k8s.io/controller-runtime/pkg/reconcile"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

func mustScheme(t *testing.T) *runtime.Scheme {
	t.Helper()
	s := runtime.NewScheme()
	if err := clientgoscheme.AddToScheme(s); err != nil {
		t.Fatal(err)
	}
	if err := cbv1.AddToScheme(s); err != nil {
		t.Fatal(err)
	}
	return s
}

func samplePool(name, ns string, warm int32) *cbv1.BrowserSessionPool {
	return &cbv1.BrowserSessionPool{
		ObjectMeta: metav1.ObjectMeta{Name: name, Namespace: ns, UID: types.UID(name + "-uid")},
		Spec: cbv1.BrowserSessionPoolSpec{
			WarmReplicas:  warm,
			MaxAgeSeconds: 3600,
			RecyclePolicy: cbv1.PoolRecreatePod,
			Template: corev1.PodTemplateSpec{
				ObjectMeta: metav1.ObjectMeta{
					Labels: map[string]string{"app.kubernetes.io/name": "chromeless"},
				},
				Spec: corev1.PodSpec{
					Containers: []corev1.Container{{
						Name:  "chromeless",
						Image: "ghcr.io/iggy/chromeless/chromium:test",
					}},
				},
			},
		},
	}
}

func sampleSession(name, ns, pool, tenant string) *cbv1.BrowserSession {
	return &cbv1.BrowserSession{
		ObjectMeta: metav1.ObjectMeta{Name: name, Namespace: ns, UID: types.UID(name + "-uid")},
		Spec: cbv1.BrowserSessionSpec{
			TenantID:           tenant,
			PoolName:           pool,
			IdleTimeoutSeconds: 600,
		},
	}
}

func warmReadyPod(name, ns, pool string) *corev1.Pod {
	return &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{
			Name:      name,
			Namespace: ns,
			Labels: map[string]string{
				cbv1.LabelSessionState: cbv1.LabelSessionStateWarm,
				cbv1.LabelSessionPool:  pool,
			},
		},
		Spec: corev1.PodSpec{
			Containers: []corev1.Container{{Name: "chromeless", Image: "x"}},
		},
		Status: corev1.PodStatus{
			PodIP: "10.0.0.42",
			Conditions: []corev1.PodCondition{
				{Type: corev1.PodReady, Status: corev1.ConditionTrue},
			},
		},
	}
}

func disableStreamerAutostart(pod *corev1.Pod) {
	for i := range pod.Spec.Containers {
		if pod.Spec.Containers[i].Name != "chromeless" {
			continue
		}
		pod.Spec.Containers[i].Env = append(pod.Spec.Containers[i].Env, corev1.EnvVar{
			Name:  "CHROMELESS_AUTOSTART_STREAMER",
			Value: "0",
		})
		return
	}
}

// reconcileTwice: the first reconcile installs the finalizer + sets
// Pending; the second is the assignment pass. Tests want the
// post-assignment state, so we call until phase != "" and != Pending,
// or up to N times.
func reconcileTwice(t *testing.T, r *SessionReconciler, key types.NamespacedName) {
	t.Helper()
	ctx := context.Background()
	for i := 0; i < 5; i++ {
		if _, err := r.Reconcile(ctx, reconcile.Request{NamespacedName: key}); err != nil {
			t.Fatalf("reconcile pass %d: %v", i+1, err)
		}
	}
}

func TestSession_EmptyPool_ColdStart(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 0)
	sess := sampleSession("s1", "cb", "default-pool", "")
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "default-pool"}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s1"})

	// A Pod should now exist with state=assigned + owner=s1.
	var pods corev1.PodList
	if err := c.List(context.Background(), &pods, client.InNamespace("cb")); err != nil {
		t.Fatal(err)
	}
	if len(pods.Items) != 1 {
		t.Fatalf("want 1 pod, got %d", len(pods.Items))
	}
	got := pods.Items[0].Labels[cbv1.LabelSessionState]
	if got != cbv1.LabelSessionStateAssign {
		t.Fatalf("pod state = %q, want %q", got, cbv1.LabelSessionStateAssign)
	}

	// Session should be Warming (Pod won't be ready in fake client).
	var got2 cbv1.BrowserSession
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s1"}, &got2); err != nil {
		t.Fatal(err)
	}
	if got2.Status.Phase != cbv1.SessionWarming {
		t.Fatalf("phase = %q, want Warming", got2.Status.Phase)
	}
}

func TestSession_WarmPool_FastAssignment(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 1)
	sess := sampleSession("s2", "cb", "default-pool", "tenant-x")
	pod := warmReadyPod("warm-pod-1", "cb", "default-pool")
	if err := controllerutil.SetControllerReference(pool, pod, scheme); err != nil {
		t.Fatal(err)
	}
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "default-pool"}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s2"})

	// Pod relabelled?
	var got corev1.Pod
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "warm-pod-1"}, &got); err != nil {
		t.Fatal(err)
	}
	if got.Labels[cbv1.LabelSessionState] != cbv1.LabelSessionStateAssign {
		t.Fatalf("pod state = %q", got.Labels[cbv1.LabelSessionState])
	}
	if got.Labels[cbv1.LabelSessionOwner] != "s2" {
		t.Fatalf("pod owner = %q", got.Labels[cbv1.LabelSessionOwner])
	}
	if got.Labels[cbv1.LabelSessionTenant] != "tenant-x" {
		t.Fatalf("pod tenant = %q", got.Labels[cbv1.LabelSessionTenant])
	}

	// Session should be Ready since Pod is Ready + has IP.
	var sg cbv1.BrowserSession
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s2"}, &sg); err != nil {
		t.Fatal(err)
	}
	if sg.Status.Phase != cbv1.SessionReady {
		t.Fatalf("phase = %q, want Ready", sg.Status.Phase)
	}
	if sg.Status.Connection == nil || sg.Status.Connection.PodName != "warm-pod-1" {
		t.Fatalf("connection = %+v", sg.Status.Connection)
	}
	if sg.Status.Connection.PodIP != "10.0.0.42" {
		t.Fatalf("podIP = %q", sg.Status.Connection.PodIP)
	}
}

func TestSession_TriformPatternCColdStartsWhenWarmPodAutostarts(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 1)
	sess := sampleSession("tf-11111111-2222-3333-4444-555555555555", "cb", "default-pool", "tenant-x")
	sess.Annotations = map[string]string{
		cbv1.AnnotationBrokerSessionID:       "cb:11111111-2222-3333-4444-555555555555",
		cbv1.AnnotationBrowserSignalingURL:   "ws://triform.triform-wtf.svc.cluster.local:3000/api/webrtc/signaling",
		cbv1.AnnotationBrowserSignalingToken: "jwt-token",
	}
	warm := warmReadyPod("warm-pod-pattern-c", "cb", "default-pool")
	if err := controllerutil.SetControllerReference(pool, warm, scheme); err != nil {
		t.Fatal(err)
	}
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, warm).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "default-pool"}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: sess.Name})

	var pods corev1.PodList
	if err := c.List(context.Background(), &pods, client.InNamespace("cb")); err != nil {
		t.Fatal(err)
	}
	var assigned *corev1.Pod
	for i := range pods.Items {
		if pods.Items[i].Labels[cbv1.LabelSessionOwner] == sess.Name {
			assigned = &pods.Items[i]
			break
		}
	}
	if assigned == nil {
		t.Fatalf("expected a cold-start assigned pod; pods=%v", pods.Items)
	}
	if assigned.Name == warm.Name {
		t.Fatalf("Pattern-C session reused warm pod %q; expected cold-start pod", warm.Name)
	}
	if len(assigned.Spec.Containers) == 0 {
		t.Fatalf("assigned pod has no containers")
	}
	env := map[string]string{}
	for _, item := range assigned.Spec.Containers[0].Env {
		env[item.Name] = item.Value
	}
	if env["SESSION_ID"] != sess.Annotations[cbv1.AnnotationBrokerSessionID] {
		t.Fatalf("SESSION_ID = %q", env["SESSION_ID"])
	}
	if env["SIGNALING_URL"] != sess.Annotations[cbv1.AnnotationBrowserSignalingURL] {
		t.Fatalf("SIGNALING_URL = %q", env["SIGNALING_URL"])
	}
	if env["SIGNALING_TOKEN"] != sess.Annotations[cbv1.AnnotationBrowserSignalingToken] {
		t.Fatalf("SIGNALING_TOKEN = %q", env["SIGNALING_TOKEN"])
	}
}

func TestSession_TriformPatternCUsesWarmPodWhenAutostartDisabled(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 1)
	sess := sampleSession("tf-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "cb", "default-pool", "tenant-x")
	sess.Annotations = map[string]string{
		cbv1.AnnotationBrokerSessionID:       "cb:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
		cbv1.AnnotationBrowserSignalingURL:   "ws://triform.triform-wtf.svc.cluster.local:3000/api/webrtc/signaling",
		cbv1.AnnotationBrowserSignalingToken: "jwt-token",
	}
	warm := warmReadyPod("warm-pod-pattern-c-no-autostart", "cb", "default-pool")
	disableStreamerAutostart(warm)
	if err := controllerutil.SetControllerReference(pool, warm, scheme); err != nil {
		t.Fatal(err)
	}
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, warm).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "default-pool"}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: sess.Name})

	var pods corev1.PodList
	if err := c.List(context.Background(), &pods, client.InNamespace("cb")); err != nil {
		t.Fatal(err)
	}
	if len(pods.Items) != 1 {
		t.Fatalf("want warm pod reused without cold-start, got %d pods", len(pods.Items))
	}

	var assigned corev1.Pod
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: warm.Name}, &assigned); err != nil {
		t.Fatal(err)
	}
	if assigned.Labels[cbv1.LabelSessionState] != cbv1.LabelSessionStateAssign {
		t.Fatalf("pod state = %q", assigned.Labels[cbv1.LabelSessionState])
	}
	if assigned.Labels[cbv1.LabelSessionOwner] != sess.Name {
		t.Fatalf("pod owner = %q", assigned.Labels[cbv1.LabelSessionOwner])
	}

	var sg cbv1.BrowserSession
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: sess.Name}, &sg); err != nil {
		t.Fatal(err)
	}
	if sg.Status.Phase != cbv1.SessionReady {
		t.Fatalf("phase = %q, want Ready", sg.Status.Phase)
	}
	if sg.Status.Connection == nil || sg.Status.Connection.PodName != warm.Name {
		t.Fatalf("connection = %+v", sg.Status.Connection)
	}
	if sg.Status.Connection.SignalingURL != sess.Annotations[cbv1.AnnotationBrowserSignalingURL] {
		t.Fatalf("signalingURL = %q", sg.Status.Connection.SignalingURL)
	}
}

func TestSession_IdleEviction(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 1)
	pod := warmReadyPod("warm-pod-2", "cb", "default-pool")
	pod.Labels[cbv1.LabelSessionState] = cbv1.LabelSessionStateAssign
	pod.Labels[cbv1.LabelSessionOwner] = "s3"
	stale := metav1.NewTime(time.Now().Add(-11 * time.Minute))
	now := metav1.NewTime(time.Now())
	sess := sampleSession("s3", "cb", "default-pool", "")
	sess.Status = cbv1.BrowserSessionStatus{
		Phase: cbv1.SessionReady,
		Connection: &cbv1.SessionConnection{
			PodName: "warm-pod-2", PodIP: "10.0.0.43",
			SignalingURL: "ws://...",
		},
		LastActivityAt: &stale,
		StartedAt:      &now,
	}
	sess.Finalizers = []string{SessionFinalizer}
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "default-pool"}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s3"})

	var sg cbv1.BrowserSession
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s3"}, &sg); err != nil {
		t.Fatal(err)
	}
	if sg.Status.Phase != cbv1.SessionEnded {
		t.Fatalf("phase = %q, want Ended", sg.Status.Phase)
	}
	if sg.Status.EndReason != "IdleTimeout" {
		t.Fatalf("endReason = %q", sg.Status.EndReason)
	}
	// Pod should have been deleted.
	var pods corev1.PodList
	_ = c.List(context.Background(), &pods, client.InNamespace("cb"))
	for _, p := range pods.Items {
		if p.Name == "warm-pod-2" {
			t.Fatalf("pod warm-pod-2 still present after idle eviction")
		}
	}
}

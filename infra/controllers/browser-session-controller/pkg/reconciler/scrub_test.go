// Package reconciler — scrub_test.go
//
// Table tests for T90 ScrubAndReturn. Uses a fakeScrubExecutor that
// returns scripted stdout/stderr/error so we can drive the reconciler
// through every branch without standing up envtest or a real pod.

package reconciler

import (
	"context"
	"errors"
	"testing"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	"sigs.k8s.io/controller-runtime/pkg/client/fake"
	"sigs.k8s.io/controller-runtime/pkg/reconcile"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

// fakeScrubExecutor records the call and returns scripted output.
type fakeScrubExecutor struct {
	stdout, stderr string
	err            error
	calls          int
}

func (f *fakeScrubExecutor) Exec(_ context.Context, _, _, _ string, _ []string) (string, string, error) {
	f.calls++
	return f.stdout, f.stderr, f.err
}

// activePool builds a pool with the requested recycle policy.
func activePool(t *testing.T, name, ns string, policy cbv1.PoolRecyclePolicy) *cbv1.BrowserSessionPool {
	t.Helper()
	p := samplePool(name, ns, 1)
	p.Spec.RecyclePolicy = policy
	return p
}

// boundReadyPod is a pod already in the assigned state pointing at sess.
func boundReadyPod(name, ns, pool, sess string) *corev1.Pod {
	p := warmReadyPod(name, ns, pool)
	p.Labels[cbv1.LabelSessionState] = cbv1.LabelSessionStateAssign
	p.Labels[cbv1.LabelSessionOwner] = sess
	p.Labels[cbv1.LabelSessionTenant] = "tenant-x"
	if p.Annotations == nil {
		p.Annotations = map[string]string{}
	}
	p.Annotations[cbv1.AnnotationSessionID] = sess + "-uid"
	return p
}

// drainTarget builds an idle BrowserSession that's about to drain.
// We pre-set Phase=Draining so the reconciler picks completeDrain
// immediately. LastActivityAt is well past the idle timeout.
func drainTarget(name, ns, pool string) *cbv1.BrowserSession {
	stale := metav1.NewTime(time.Now().Add(-1 * time.Hour))
	now := metav1.NewTime(time.Now())
	s := sampleSession(name, ns, pool, "tenant-x")
	s.Finalizers = []string{SessionFinalizer}
	s.Status = cbv1.BrowserSessionStatus{
		Phase: cbv1.SessionDraining,
		Connection: &cbv1.SessionConnection{
			PodName: name + "-pod", PodIP: "10.0.0.1",
			SignalingURL: "ws://...",
		},
		LastActivityAt: &stale,
		StartedAt:      &now,
	}
	return s
}

func TestScrub_HappyPath_PodReturnsToWarm(t *testing.T) {
	scheme := mustScheme(t)
	pool := activePool(t, "shared-pool", "cb", cbv1.PoolScrubAndReturn)
	sess := drainTarget("s-scrub", "cb", "shared-pool")
	pod := boundReadyPod("s-scrub-pod", "cb", "shared-pool", "s-scrub")

	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()

	exec := &fakeScrubExecutor{stdout: "[scrub] OK\n", stderr: "starting\nuser-data-dir verified empty\n"}
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "shared-pool", ScrubExec: exec}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s-scrub"})

	// Pod should still exist, relabelled as warm, no owner.
	var got corev1.Pod
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s-scrub-pod"}, &got); err != nil {
		t.Fatalf("pod missing post-scrub: %v", err)
	}
	if got.Labels[cbv1.LabelSessionState] != cbv1.LabelSessionStateWarm {
		t.Errorf("pod state = %q, want warm", got.Labels[cbv1.LabelSessionState])
	}
	if _, has := got.Labels[cbv1.LabelSessionOwner]; has {
		t.Errorf("expected owner label cleared; still: %q", got.Labels[cbv1.LabelSessionOwner])
	}
	if _, has := got.Labels[cbv1.LabelSessionTenant]; has {
		t.Errorf("expected tenant label cleared; still: %q", got.Labels[cbv1.LabelSessionTenant])
	}
	if got.Annotations != nil {
		if _, has := got.Annotations[cbv1.AnnotationSessionID]; has {
			t.Errorf("expected session-id annotation cleared")
		}
	}
	if exec.calls != 1 {
		t.Errorf("exec called %d times, want 1", exec.calls)
	}

	// Session should be Ended.
	var sg cbv1.BrowserSession
	_ = c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s-scrub"}, &sg)
	if sg.Status.Phase != cbv1.SessionEnded {
		t.Errorf("phase = %q, want Ended", sg.Status.Phase)
	}
}

func TestScrub_VerifyFails_FallsBackToRecreate(t *testing.T) {
	scheme := mustScheme(t)
	pool := activePool(t, "shared-pool", "cb", cbv1.PoolScrubAndReturn)
	sess := drainTarget("s-fail", "cb", "shared-pool")
	pod := boundReadyPod("s-fail-pod", "cb", "shared-pool", "s-fail")

	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()

	// Scrub script ran but didn't emit the OK marker.
	exec := &fakeScrubExecutor{stdout: "[scrub] starting\n", stderr: "FAIL: $USER_DATA_DIR still has 3 entries"}
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "shared-pool", ScrubExec: exec}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s-fail"})

	// Pod should be deleted (RecreatePod fallback).
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s-fail-pod"}, &corev1.Pod{}); err == nil {
		t.Fatalf("expected pod deleted after scrub-fallback; pod still present")
	}
	if exec.calls != 1 {
		t.Errorf("exec called %d times, want 1", exec.calls)
	}
}

func TestScrub_ExecError_FallsBackToRecreate(t *testing.T) {
	scheme := mustScheme(t)
	pool := activePool(t, "shared-pool", "cb", cbv1.PoolScrubAndReturn)
	sess := drainTarget("s-exec-err", "cb", "shared-pool")
	pod := boundReadyPod("s-exec-err-pod", "cb", "shared-pool", "s-exec-err")

	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()

	exec := &fakeScrubExecutor{err: errors.New("connection refused")}
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "shared-pool", ScrubExec: exec}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s-exec-err"})

	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s-exec-err-pod"}, &corev1.Pod{}); err == nil {
		t.Fatalf("expected pod deleted on exec-failure fallback; pod still present")
	}
}

func TestRecyclePolicy_RecreatePod_NotRegressed(t *testing.T) {
	// With RecreatePod, the bound pod must always be deleted regardless
	// of whether ScrubExec is wired.
	scheme := mustScheme(t)
	pool := activePool(t, "strict-pool", "cb", cbv1.PoolRecreatePod)
	sess := drainTarget("s-recreate", "cb", "strict-pool")
	pod := boundReadyPod("s-recreate-pod", "cb", "strict-pool", "s-recreate")

	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()

	// Even with a working ScrubExec, RecreatePod skips it entirely.
	exec := &fakeScrubExecutor{stdout: "[scrub] OK\n"}
	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "strict-pool", ScrubExec: exec}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s-recreate"})

	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s-recreate-pod"}, &corev1.Pod{}); err == nil {
		t.Fatalf("expected pod deleted on RecreatePod policy")
	}
	if exec.calls != 0 {
		t.Errorf("exec called %d times, want 0 (RecreatePod must not invoke scrub)", exec.calls)
	}
}

func TestScrub_NoExecutor_DegradesToRecreate(t *testing.T) {
	// ScrubExec=nil + recyclePolicy=ScrubAndReturn: degrade silently
	// to RecreatePod. Same expected outcome as the failure cases.
	scheme := mustScheme(t)
	pool := activePool(t, "shared-pool", "cb", cbv1.PoolScrubAndReturn)
	sess := drainTarget("s-noexec", "cb", "shared-pool")
	pod := boundReadyPod("s-noexec-pod", "cb", "shared-pool", "s-noexec")

	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, sess, pod).
		WithStatusSubresource(&cbv1.BrowserSession{}, &cbv1.BrowserSessionPool{}).
		Build()

	r := &SessionReconciler{Client: c, Scheme: scheme, DefaultPool: "shared-pool", ScrubExec: nil}

	reconcileTwice(t, r, types.NamespacedName{Namespace: "cb", Name: "s-noexec"})

	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "s-noexec-pod"}, &corev1.Pod{}); err == nil {
		t.Fatalf("expected pod deleted when ScrubExec is nil")
	}
}

// reconcileEnded forces the scrubOrDelete path off the assignment loop:
// we pre-set Phase=Draining (via drainTarget) so completeDrain runs in
// the first reconcile pass.
var _ = reconcile.Result{} // keep the import live across edits

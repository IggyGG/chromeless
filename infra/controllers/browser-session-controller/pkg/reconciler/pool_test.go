// Package reconciler — pool_test.go
//
// Tests for the warm-pool replenishment loop.

package reconciler

import (
	"context"
	"testing"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/client/fake"
	"sigs.k8s.io/controller-runtime/pkg/reconcile"

	cbv1 "github.com/iggy/chromeless/infra/controllers/browser-session-controller/pkg/apis/v1"
)

func TestPool_Replenishes(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 3)
	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool).
		WithStatusSubresource(&cbv1.BrowserSessionPool{}).
		Build()
	r := &PoolReconciler{Client: c, Scheme: scheme}

	if _, err := r.Reconcile(context.Background(),
		reconcile.Request{NamespacedName: types.NamespacedName{Namespace: "cb", Name: "default-pool"}},
	); err != nil {
		t.Fatal(err)
	}

	var pods corev1.PodList
	if err := c.List(context.Background(), &pods, client.InNamespace("cb")); err != nil {
		t.Fatal(err)
	}
	if len(pods.Items) != 3 {
		t.Fatalf("want 3 pods, got %d", len(pods.Items))
	}
	for i := range pods.Items {
		if pods.Items[i].Labels[cbv1.LabelSessionState] != cbv1.LabelSessionStateWarm {
			t.Fatalf("pod[%d].state = %q", i, pods.Items[i].Labels[cbv1.LabelSessionState])
		}
		if pods.Items[i].Labels[cbv1.LabelSessionPool] != "default-pool" {
			t.Fatalf("pod[%d].pool = %q", i, pods.Items[i].Labels[cbv1.LabelSessionPool])
		}
	}

	var pg cbv1.BrowserSessionPool
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "default-pool"}, &pg); err != nil {
		t.Fatal(err)
	}
	if pg.Status.Warm != 3 {
		t.Fatalf("status.warm = %d, want 3", pg.Status.Warm)
	}
	if pg.Status.TotalEverProvisioned != 3 {
		t.Fatalf("status.totalEverProvisioned = %d, want 3", pg.Status.TotalEverProvisioned)
	}
}

func TestPool_DrainsOverAged(t *testing.T) {
	scheme := mustScheme(t)
	pool := samplePool("default-pool", "cb", 1)
	pool.Spec.MaxAgeSeconds = 60 // 1 minute

	old := warmReadyPod("warm-old", "cb", "default-pool")
	old.CreationTimestamp = metav1.NewTime(time.Now().Add(-10 * time.Minute))
	young := warmReadyPod("warm-young", "cb", "default-pool")
	young.CreationTimestamp = metav1.NewTime(time.Now())

	c := fake.NewClientBuilder().
		WithScheme(scheme).
		WithObjects(pool, old, young).
		WithStatusSubresource(&cbv1.BrowserSessionPool{}).
		Build()
	r := &PoolReconciler{Client: c, Scheme: scheme}

	if _, err := r.Reconcile(context.Background(),
		reconcile.Request{NamespacedName: types.NamespacedName{Namespace: "cb", Name: "default-pool"}},
	); err != nil {
		t.Fatal(err)
	}

	// warm-old should be deleted (drainWarmPod calls Delete directly).
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "warm-old"}, &corev1.Pod{}); err == nil {
		t.Fatalf("expected warm-old to be deleted")
	}
	// warm-young should still be present.
	if err := c.Get(context.Background(), types.NamespacedName{Namespace: "cb", Name: "warm-young"}, &corev1.Pod{}); err != nil {
		t.Fatalf("warm-young should still exist: %v", err)
	}
}

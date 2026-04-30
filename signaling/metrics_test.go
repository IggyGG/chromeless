package main

import (
	"fmt"
	"testing"
)

// resetTenantBucketForTest replaces the seenTenants map with an empty
// one and restores the original on cleanup. Lets each test see a fresh
// cap state without races against parallel tests in this package.
func resetTenantBucketForTest(t *testing.T) {
	t.Helper()
	tenantBucketMu.Lock()
	prev := seenTenants
	seenTenants = make(map[string]struct{}, tenantLabelCap)
	tenantBucketMu.Unlock()
	t.Cleanup(func() {
		tenantBucketMu.Lock()
		seenTenants = prev
		tenantBucketMu.Unlock()
	})
}

func TestLabelTenant_Anonymous(t *testing.T) {
	resetTenantBucketForTest(t)
	if got := labelTenant(anonymousTenant); got != anonymousTenant {
		t.Fatalf("anonymous: got %q, want %q", got, anonymousTenant)
	}
	// anonymous must not consume a slot in seenTenants.
	tenantBucketMu.RLock()
	defer tenantBucketMu.RUnlock()
	if _, ok := seenTenants[anonymousTenant]; ok {
		t.Fatal("anonymous should not occupy a tracked slot")
	}
}

func TestLabelTenant_BelowCap(t *testing.T) {
	resetTenantBucketForTest(t)
	for i := 0; i < tenantLabelCap-1; i++ {
		got := labelTenant(fmt.Sprintf("tenant-%d", i))
		want := fmt.Sprintf("tenant-%d", i)
		if got != want {
			t.Fatalf("i=%d: got %q, want %q", i, got, want)
		}
	}
	// Same tenant repeats are still labeled themselves (cache hit).
	if got := labelTenant("tenant-0"); got != "tenant-0" {
		t.Fatalf("repeat: got %q, want tenant-0", got)
	}
}

func TestLabelTenant_OverCap(t *testing.T) {
	resetTenantBucketForTest(t)
	// Fill to exactly the cap.
	for i := 0; i < tenantLabelCap; i++ {
		labelTenant(fmt.Sprintf("t-%d", i))
	}
	// One previously-seen tenant still maps to itself.
	if got := labelTenant("t-0"); got != "t-0" {
		t.Fatalf("known tenant remapped to overflow: %q", got)
	}
	// A new tenant maps to overflow.
	if got := labelTenant("brand-new"); got != tenantOverflow {
		t.Fatalf("unseen-over-cap: got %q, want %q", got, tenantOverflow)
	}
	// Repeated overflow lookups stay overflow without polluting the map.
	for i := 0; i < 10; i++ {
		if got := labelTenant(fmt.Sprintf("over-%d", i)); got != tenantOverflow {
			t.Fatalf("over-%d: got %q", i, got)
		}
	}
	tenantBucketMu.RLock()
	size := len(seenTenants)
	tenantBucketMu.RUnlock()
	if size != tenantLabelCap {
		t.Fatalf("seen size: got %d, want %d (cap)", size, tenantLabelCap)
	}
}

func TestLabelTenant_AnonymousNeverConsumesSlot(t *testing.T) {
	resetTenantBucketForTest(t)
	for i := 0; i < tenantLabelCap; i++ {
		labelTenant(anonymousTenant)
	}
	// Should still have full cap available.
	for i := 0; i < tenantLabelCap; i++ {
		got := labelTenant(fmt.Sprintf("t-%d", i))
		if got == tenantOverflow {
			t.Fatalf("anonymous illegally consumed a slot at i=%d", i)
		}
	}
}

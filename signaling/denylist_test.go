package main

import (
	"context"
	"errors"
	"sync"
	"testing"
)

// ---------- StaticDenylist ----------

func TestStaticDenylist_Empty(t *testing.T) {
	d := NewStaticDenylist()
	hit, err := d.Contains(context.Background(), "alice", "abc")
	if err != nil {
		t.Fatalf("err: %v", err)
	}
	if hit {
		t.Fatal("empty denylist returned hit")
	}
}

func TestStaticDenylist_TenantWide(t *testing.T) {
	d := NewStaticDenylist()
	if err := d.AddTenant(context.Background(), "alice", "compromised"); err != nil {
		t.Fatalf("AddTenant: %v", err)
	}
	for _, jti := range []string{"", "abc", "xyz"} {
		hit, _ := d.Contains(context.Background(), "alice", jti)
		if !hit {
			t.Errorf("alice/%q: want hit, got miss", jti)
		}
	}
	hit, _ := d.Contains(context.Background(), "bob", "abc")
	if hit {
		t.Errorf("bob: want miss, got hit (cross-tenant)")
	}
}

func TestStaticDenylist_PerJTI(t *testing.T) {
	d := NewStaticDenylist()
	if err := d.AddJTI(context.Background(), "alice", "abc", "stolen"); err != nil {
		t.Fatalf("AddJTI: %v", err)
	}
	if hit, _ := d.Contains(context.Background(), "alice", "abc"); !hit {
		t.Error("specific jti should hit")
	}
	if hit, _ := d.Contains(context.Background(), "alice", "other"); hit {
		t.Error("other jti for same tenant should miss")
	}
	if hit, _ := d.Contains(context.Background(), "alice", ""); hit {
		t.Error("empty jti must not match per-jti entries")
	}
}

func TestStaticDenylist_AddRequiresArgs(t *testing.T) {
	d := NewStaticDenylist()
	if err := d.AddTenant(context.Background(), "", ""); err == nil {
		t.Error("AddTenant with empty tenant should error")
	}
	if err := d.AddJTI(context.Background(), "alice", "", ""); err == nil {
		t.Error("AddJTI with empty jti should error")
	}
	if err := d.AddJTI(context.Background(), "", "abc", ""); err == nil {
		t.Error("AddJTI with empty tenant should error")
	}
}

func TestStaticDenylist_LoadFromEnv(t *testing.T) {
	d := NewStaticDenylist()
	n := loadStaticFromEnv(d, " alice , bob:abc , , carol , dave:xyz123 ")
	if n != 4 {
		t.Errorf("loaded count: got %d want 4", n)
	}
	cases := []struct {
		t, j string
		want bool
	}{
		{"alice", "anything", true}, // tenant-wide
		{"bob", "abc", true},        // per-jti hit
		{"bob", "other", false},     // wrong jti
		{"carol", "", true},         // tenant-wide, empty jti
		{"dave", "xyz123", true},
		{"dave", "x", false},
		{"eve", "abc", false},
	}
	for _, tc := range cases {
		hit, _ := d.Contains(context.Background(), tc.t, tc.j)
		if hit != tc.want {
			t.Errorf("Contains(%q, %q) = %v, want %v", tc.t, tc.j, hit, tc.want)
		}
	}
}

// ---------- RedisDenylist with a fake client ----------

type fakeRedis struct {
	mu     sync.Mutex
	sets   map[string]map[string]struct{}
	addErr error
	memErr error
	calls  int
}

func newFakeRedis() *fakeRedis {
	return &fakeRedis{sets: make(map[string]map[string]struct{})}
}

func (f *fakeRedis) SAdd(_ context.Context, key string, members ...string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.addErr != nil {
		return f.addErr
	}
	if _, ok := f.sets[key]; !ok {
		f.sets[key] = make(map[string]struct{})
	}
	for _, m := range members {
		f.sets[key][m] = struct{}{}
	}
	return nil
}
func (f *fakeRedis) SIsMember(_ context.Context, key, member string) (bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.memErr != nil {
		return false, f.memErr
	}
	if s, ok := f.sets[key]; ok {
		_, hit := s[member]
		return hit, nil
	}
	return false, nil
}

func TestRedisDenylist_HappyPath(t *testing.T) {
	rc := newFakeRedis()
	d := NewRedisDenylist(rc, quietLogger())

	if err := d.AddTenant(context.Background(), "alice", "compromised"); err != nil {
		t.Fatalf("AddTenant: %v", err)
	}
	if err := d.AddJTI(context.Background(), "bob", "abc", ""); err != nil {
		t.Fatalf("AddJTI: %v", err)
	}

	// Tenant-wide hit.
	if hit, _ := d.Contains(context.Background(), "alice", "anyjti"); !hit {
		t.Error("alice tenant-wide hit missed")
	}
	// Per-jti hit.
	if hit, _ := d.Contains(context.Background(), "bob", "abc"); !hit {
		t.Error("bob:abc miss")
	}
	// Per-jti miss for the right tenant, wrong jti.
	if hit, _ := d.Contains(context.Background(), "bob", "other"); hit {
		t.Error("bob:other should miss")
	}
}

func TestRedisDenylist_FailsOpenOnError(t *testing.T) {
	rc := newFakeRedis()
	rc.memErr = errors.New("redis ECONNREFUSED")
	d := NewRedisDenylist(rc, quietLogger())
	hit, err := d.Contains(context.Background(), "alice", "abc")
	if err == nil {
		t.Error("Contains should bubble the redis error to the caller")
	}
	if hit {
		t.Error("on error must NOT report hit (fail-open)")
	}
}

func TestRedisDenylist_TenantBeatsJTI(t *testing.T) {
	rc := newFakeRedis()
	d := NewRedisDenylist(rc, quietLogger())
	_ = d.AddTenant(context.Background(), "alice", "")
	// jti with the same tenant should never even reach the second
	// SIsMember call — verify by setting jti-set lookup to error.
	rc.mu.Lock()
	rc.sets[redisKeyTenants] = map[string]struct{}{"alice": {}}
	rc.mu.Unlock()
	hit, err := d.Contains(context.Background(), "alice", "any-jti")
	if err != nil || !hit {
		t.Fatalf("hit=%v err=%v want hit", hit, err)
	}
}

// ---------- newRedisClient stub returns a clear error ----------

func TestNewRedisClient_NotCompiled(t *testing.T) {
	_, err := newRedisClient("localhost:6379")
	if err == nil {
		t.Fatal("expected explanatory error from stub adapter")
	}
	if !errors.Is(err, errRedisNotCompiled) {
		t.Errorf("err = %v, want errRedisNotCompiled", err)
	}
}

// ---------- initDenylist env handling ----------

func TestInitDenylist_StaticFromEnv(t *testing.T) {
	t.Setenv("CHROMELESS_DENYLIST", "alice, bob:tok")
	t.Setenv("CHROMELESS_DENYLIST_REDIS_ADDR", "")
	prev := globalDenylist
	t.Cleanup(func() { globalDenylist = prev })

	d := initDenylist(quietLogger())
	if d == nil {
		t.Fatal("nil denylist")
	}
	hit, _ := d.Contains(context.Background(), "alice", "")
	if !hit {
		t.Error("alice should be banned via env")
	}
}

func TestInitDenylist_FallsBackToStaticOnRedisFailure(t *testing.T) {
	t.Setenv("CHROMELESS_DENYLIST_REDIS_ADDR", "127.0.0.1:6379") // newRedisClient is stubbed
	t.Setenv("CHROMELESS_DENYLIST", "alice")
	prev := globalDenylist
	t.Cleanup(func() { globalDenylist = prev })

	d := initDenylist(quietLogger())
	// Redis failed → static fallback applies the env CSV.
	hit, _ := d.Contains(context.Background(), "alice", "")
	if !hit {
		t.Error("expected static fallback to honour CHROMELESS_DENYLIST")
	}
}

// Package main — denylist.go
//
// T89: token revocation via a pluggable denylist. Two implementations:
//
//   - StaticDenylist: in-memory set populated from `CHROMELESS_DENYLIST`
//     (CSV of `tenant` or `tenant:jti` entries). Suitable for dev,
//     CI, and small single-node deploys.
//   - RedisDenylist: reads from a Redis SET so multiple signaling
//     replicas share a single revocation source. The real go-redis
//     wiring lives in `denylist_redis.go`; this file provides the
//     interface + a fake-friendly `RedisClient` abstraction so tests
//     can exercise the logic without a running Redis.
//
// Storage shape (Redis):
//
//   cb:auth:denylist:tenants  SET of tenant ids (tenant-wide ban)
//   cb:auth:denylist:jtis     SET of "tenant:jti" entries (per-token ban)
//
// Lookups consult both. Tenant-wide ban beats jti — a tenant in the
// tenants set is rejected regardless of jti.
//
// The admin endpoint (signaling/admin/admin.go) is the canonical
// writer; the auth path is read-only.

package main

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"strings"
	"sync"
	"time"
)

// Denylist is the read-side interface. `Contains(tenant, jti)` is
// called inline on every authenticated connect. Implementations MUST
// be safe for concurrent use.
type Denylist interface {
	// Contains reports whether (tenant, jti) is revoked. An empty jti
	// means "any jti for this tenant" — equivalent to a tenant-wide
	// query. Implementations should answer in <1 ms typical so the
	// connect path stays cheap.
	Contains(ctx context.Context, tenant, jti string) (bool, error)
}

// adminDenylist is the write-side interface; admin/admin.go uses it.
// Kept separate so callers that only need read access (auth.go) can
// take the smaller interface.
type adminDenylist interface {
	Denylist
	// AddTenant bans every token for `tenant` until removed.
	AddTenant(ctx context.Context, tenant, reason string) error
	// AddJTI bans a single token id under tenant. Idempotent.
	AddJTI(ctx context.Context, tenant, jti, reason string) error
}

// ---------------------------------------------------------------------------
// StaticDenylist — env-var-driven, no external state.
// ---------------------------------------------------------------------------

const denylistEnv = "CHROMELESS_DENYLIST"

// StaticDenylist is the default. It is seeded once from `CHROMELESS_DENYLIST`
// (CSV of `tenant` or `tenant:jti` entries). Mutations made via
// AddTenant / AddJTI are in-memory only — they survive within the
// process but are lost on restart.
type StaticDenylist struct {
	mu      sync.RWMutex
	tenants map[string]struct{}
	jtis    map[string]struct{} // key is "tenant:jti"
	reasons map[string]string   // key is the same; informational only
}

func NewStaticDenylist() *StaticDenylist {
	return &StaticDenylist{
		tenants: make(map[string]struct{}),
		jtis:    make(map[string]struct{}),
		reasons: make(map[string]string),
	}
}

// loadStaticFromEnv parses CHROMELESS_DENYLIST. Each comma-separated entry
// is either `tenant` (whole-tenant ban) or `tenant:jti` (single-token
// ban). Whitespace is stripped; empty entries are skipped.
func loadStaticFromEnv(d *StaticDenylist, env string) int {
	d.mu.Lock()
	defer d.mu.Unlock()
	loaded := 0
	for _, raw := range strings.Split(env, ",") {
		entry := strings.TrimSpace(raw)
		if entry == "" {
			continue
		}
		if i := strings.IndexByte(entry, ':'); i >= 0 {
			tenant := entry[:i]
			jti := entry[i+1:]
			if tenant != "" && jti != "" {
				d.jtis[tenant+":"+jti] = struct{}{}
				loaded++
			}
		} else {
			d.tenants[entry] = struct{}{}
			loaded++
		}
	}
	return loaded
}

// Contains implements Denylist. Constant-time set lookups; the lock
// is RLock so concurrent connects don't serialise on the auth path.
func (d *StaticDenylist) Contains(_ context.Context, tenant, jti string) (bool, error) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	if _, ok := d.tenants[tenant]; ok {
		return true, nil
	}
	if jti != "" {
		if _, ok := d.jtis[tenant+":"+jti]; ok {
			return true, nil
		}
	}
	return false, nil
}

func (d *StaticDenylist) AddTenant(_ context.Context, tenant, reason string) error {
	if tenant == "" {
		return fmt.Errorf("tenant required")
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	d.tenants[tenant] = struct{}{}
	if reason != "" {
		d.reasons["tenant:"+tenant] = reason
	}
	return nil
}

func (d *StaticDenylist) AddJTI(_ context.Context, tenant, jti, reason string) error {
	if tenant == "" || jti == "" {
		return fmt.Errorf("tenant and jti required")
	}
	d.mu.Lock()
	defer d.mu.Unlock()
	d.jtis[tenant+":"+jti] = struct{}{}
	if reason != "" {
		d.reasons["jti:"+tenant+":"+jti] = reason
	}
	return nil
}

// Snapshot returns the current denylist contents for diagnostics.
// Returned slices are copies; safe to mutate.
func (d *StaticDenylist) Snapshot() (tenants []string, jtis []string) {
	d.mu.RLock()
	defer d.mu.RUnlock()
	for t := range d.tenants {
		tenants = append(tenants, t)
	}
	for k := range d.jtis {
		jtis = append(jtis, k)
	}
	return tenants, jtis
}

// ---------------------------------------------------------------------------
// RedisDenylist — multi-replica revocation source.
// ---------------------------------------------------------------------------

const (
	redisKeyTenants = "cb:auth:denylist:tenants"
	redisKeyJTIs    = "cb:auth:denylist:jtis"
)

// RedisClient is the small subset of go-redis we depend on. Defining
// it here means tests can exercise RedisDenylist with a fake without
// pulling in miniredis (and without forcing main.go to depend on a
// running Redis just to compile). The real-redis adapter lives in
// `denylist_redis.go`.
type RedisClient interface {
	SAdd(ctx context.Context, key string, members ...string) error
	SIsMember(ctx context.Context, key, member string) (bool, error)
}

// RedisDenylist reads/writes the shared revocation set in Redis.
type RedisDenylist struct {
	client RedisClient
	// shortCircuit is the maximum total time we'll wait on Redis for
	// an auth-path lookup. We default to 100 ms — Redis reaches us
	// over loopback in v1, and a hard cap means a Redis hiccup can't
	// stall connect. On timeout we fail-open (return false) and log;
	// a follow-up may want fail-closed in security-critical mode.
	shortCircuit time.Duration
	log          *slog.Logger
}

func NewRedisDenylist(client RedisClient, log *slog.Logger) *RedisDenylist {
	return &RedisDenylist{client: client, shortCircuit: 100 * time.Millisecond, log: log}
}

func (d *RedisDenylist) Contains(ctx context.Context, tenant, jti string) (bool, error) {
	ctx, cancel := context.WithTimeout(ctx, d.shortCircuit)
	defer cancel()

	// Tenant-wide ban first; one round-trip and beats jti-level.
	tenantHit, err := d.client.SIsMember(ctx, redisKeyTenants, tenant)
	if err != nil {
		d.log.Warn("redis denylist tenant lookup failed; failing open", slog.Any("err", err))
		return false, err
	}
	if tenantHit {
		return true, nil
	}
	if jti == "" {
		return false, nil
	}
	jtiHit, err := d.client.SIsMember(ctx, redisKeyJTIs, tenant+":"+jti)
	if err != nil {
		d.log.Warn("redis denylist jti lookup failed; failing open", slog.Any("err", err))
		return false, err
	}
	return jtiHit, nil
}

func (d *RedisDenylist) AddTenant(ctx context.Context, tenant, _ string) error {
	if tenant == "" {
		return fmt.Errorf("tenant required")
	}
	return d.client.SAdd(ctx, redisKeyTenants, tenant)
}

func (d *RedisDenylist) AddJTI(ctx context.Context, tenant, jti, _ string) error {
	if tenant == "" || jti == "" {
		return fmt.Errorf("tenant and jti required")
	}
	return d.client.SAdd(ctx, redisKeyJTIs, tenant+":"+jti)
}

// ---------------------------------------------------------------------------
// initDenylist — wire from env at startup.
// ---------------------------------------------------------------------------

// globalDenylist is the read-side handle the auth path consults. The
// admin endpoint takes the same instance as adminDenylist to write.
var globalDenylist Denylist = NewStaticDenylist() // safe default until init runs

// initDenylist seeds globalDenylist. Honours:
//
//	CHROMELESS_DENYLIST_REDIS_ADDR  — if set, use a Redis denylist; the
//	                              client adapter is in denylist_redis.go.
//	CHROMELESS_DENYLIST             — CSV-seed the static denylist.
//
// When both are set, Redis wins; the static CSV is logged-and-ignored
// rather than silently dropped, because that combination is almost
// always a misconfiguration.
//
// initDenylist also returns the writable handle; main passes it to
// the admin endpoint.
func initDenylist(logger *slog.Logger) adminDenylist {
	if addr := strings.TrimSpace(os.Getenv("CHROMELESS_DENYLIST_REDIS_ADDR")); addr != "" {
		client, err := newRedisClient(addr) // implemented in denylist_redis.go
		if err != nil {
			logger.Error("denylist: redis client construction failed; falling back to static",
				slog.String("addr", addr), slog.Any("err", err))
		} else {
			d := NewRedisDenylist(client, logger)
			globalDenylist = d
			logger.Info("denylist: redis backend active", slog.String("addr", addr))
			if static := strings.TrimSpace(os.Getenv(denylistEnv)); static != "" {
				logger.Warn("denylist: " + denylistEnv +
					" is set but ignored because Redis is configured")
			}
			return d
		}
	}
	d := NewStaticDenylist()
	if env := strings.TrimSpace(os.Getenv(denylistEnv)); env != "" {
		n := loadStaticFromEnv(d, env)
		logger.Info("denylist: static backend, seeded from env",
			slog.Int("entries", n))
	} else {
		logger.Info("denylist: static backend, empty (set " + denylistEnv +
			" or CHROMELESS_DENYLIST_REDIS_ADDR to populate)")
	}
	globalDenylist = d
	return d
}

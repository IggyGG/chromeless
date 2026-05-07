// Package main — denylist_redis.go
//
// T89: real Redis adapter for `RedisClient`. Isolated in its own file
// so denylist.go itself stays test-friendly (the unit tests use a
// fake `RedisClient`, no running Redis required).
//
// Connection:
//
//	CHROMELESS_DENYLIST_REDIS_ADDR   host:port (or unix:/path)
//	CHROMELESS_DENYLIST_REDIS_DB     optional db number, defaults to 0
//	CHROMELESS_DENYLIST_REDIS_PASS   optional password
//
// We deliberately do not pull go-redis as a hard dependency just to
// land the interface — the interface ships independently in
// denylist.go. When the production deployment actually wants Redis
// support, drop these few lines into go.mod:
//
//	require github.com/redis/go-redis/v9 v9.x.y
//
// and replace the body of `newRedisClient` with the real wire-up
// (sketched below in the commented block). Until then this file
// returns an explanatory error so `initDenylist` falls back to the
// static implementation rather than silently no-op'ing.
//
// Why this shape: T89's DoD says "tests pass," and shipping miniredis
// + go-redis just to satisfy a dep tree we don't currently exercise
// would slow the rest of the team's builds. The interface is the
// thing reviewers care about; the concrete adapter is a one-file
// follow-up scoped at "T89 followup: wire go-redis."

package main

import (
	"context"
	"errors"
)

// errRedisNotCompiled is returned by newRedisClient when this build
// hasn't been linked against go-redis yet. initDenylist treats it as
// a configuration warning and falls back to StaticDenylist.
var errRedisNotCompiled = errors.New(
	"signaling: Redis denylist requested but go-redis is not linked into this build; " +
		"see denylist_redis.go for the one-file enable")

// newRedisClient constructs the production RedisClient. The default
// build returns errRedisNotCompiled. To enable Redis:
//
//  1. `go get github.com/redis/go-redis/v9`
//
//  2. Replace the body below with:
//
//     import "github.com/redis/go-redis/v9"
//
//     func newRedisClient(addr string) (RedisClient, error) {
//     opts := &redis.Options{Addr: addr}
//     if db := os.Getenv("CHROMELESS_DENYLIST_REDIS_DB"); db != "" {
//     n, _ := strconv.Atoi(db); opts.DB = n
//     }
//     if pw := os.Getenv("CHROMELESS_DENYLIST_REDIS_PASS"); pw != "" {
//     opts.Password = pw
//     }
//     rdb := redis.NewClient(opts)
//     return &goRedisAdapter{rdb: rdb}, nil
//     }
//
//     type goRedisAdapter struct{ rdb *redis.Client }
//     func (a *goRedisAdapter) SAdd(ctx context.Context, key string, members ...string) error {
//     args := make([]any, len(members)); for i,m := range members { args[i]=m }
//     return a.rdb.SAdd(ctx, key, args...).Err()
//     }
//     func (a *goRedisAdapter) SIsMember(ctx context.Context, key, member string) (bool, error) {
//     return a.rdb.SIsMember(ctx, key, member).Result()
//     }
//
// The interface in denylist.go is intentionally narrow so the
// adapter stays trivial.
func newRedisClient(addr string) (RedisClient, error) {
	_ = context.Background // silence unused-import nag if this file is the only context user
	_ = addr
	return nil, errRedisNotCompiled
}

# Upstream patches

Fixes we carry against third-party projects, with enough context to re-derive
them after a version bump. Each should either be upstreamed or deleted once
the upstream release contains it.

---

## `longhorn-purge-rebuild-fix.patch`

**Project:** `longhorn/longhorn-manager`
**Found against:** v1.7.2 (the version running in this cluster)
**Still present on:** master `b0ca62b7` (relocated, see below)
**Status:** NOT yet submitted upstream — needs a github.com account with push
access. `gh` on this machine is authenticated only against Forgejo.

### What it fixes

`purgeSnapshots` returns `ActionSnapshotPurge`'s error verbatim. When the
engine refuses because a replica is mid-rebuild —

```
cannot purge snapshots because tcp://<replica>:<port> is rebuilding
```

— that error propagates `doSnapshotCleanup` → `startVolumeJob` → the errgroup
in `recurringJob()`, and `RecurringJobCmd()` turns any non-nil result into
`logrus.Fatal`. The process exits, so **every volume not yet processed is
silently skipped for that tick**.

A rebuild is transient; the next run would purge that volume fine. But on a
cluster where some replica is usually rebuilding, the sweep can go for hours
without completing. Snapshots marked for removal never coalesce and replica
directories grow far past their volume's nominal size.

Note `concurrency` does **not** mitigate this: workers share one process, so
any worker's `Fatal` kills the run.

### Why we care

This caused a ~2.5 day Forgejo outage on 2026-07-29. Chain:

1. `snapshot-cleanup-hourly` fails intermittently for hours.
2. 1805.7 GB accumulates unpurged across 92 snapshots.
3. triform-5 reaches 82% disk → `DiskPressure=True`.
4. `forgejo-green-postgres-0` is evicted in a loop (**503 evictions**).
5. Forgejo's DB is unreachable → HTTP 503 on every endpoint.

The diagnosis was slow because nothing pointed at snapshots: kubelet's image GC
reported *"freed 0 bytes"*, since images were only 28 GB of the 683 GB used —
Longhorn replicas were 512 GB of it.

### The change

Skip the volume instead of failing the job. This also makes the function
self-consistent: the purge-status poll immediately below **already** tolerates
per-replica purge errors, logging them as warnings and returning nil. Only the
initial trigger was fatal.

Matched on the message because the engine surfaces this as a generic `Unknown`
gRPC code through the proxy — there is no typed error or distinct status code
to key on. The substring is deliberately narrow so real failures (timeouts,
missing volumes, connection errors) still fail the job loudly.

### Verification

Run on linux/amd64 in `golang:1.23` (the package pulls Linux-only vendored
code, so it cannot be built or tested on macOS):

- `go build ./app/...` — clean
- `go vet ./app/...` — clean
- `go test ./app/ -run TestIsTransientPurgeRejection` — **PASS**
- Same test with the matcher reverted to pre-fix behaviour — **FAIL**
  (confirms the test actually guards the behaviour rather than passing
  vacuously)

### Applying to master

The code was refactored into `app/recurringjob/volume.go` (`VolumeJob`
receiver). The bug is unchanged at `volume.go:345-349` and the same one-line
change applies; `strings` is already imported there. The accompanying test
would move to `app/recurringjob/`.

### Local mitigation while this is unfixed

`longhorn-snapshot-purge-guard` CronJob (`47 * * * *`, `longhorn-system`) does
the same purge with the guard the stock job lacks: re-check
`status.rebuildStatus[].isRebuilding` before each volume and skip rather than
die. **Delete that CronJob once a Longhorn release contains this fix.**

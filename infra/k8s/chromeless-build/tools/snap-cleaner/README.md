# snap-cleaner — containerd orphan-in-DB snapshot repair

Surgical fix for the containerd snapshotter metadata corruption class
where `metadata.db` references on-disk snapshot directories that no
longer exist. Symptom: `Init:CreateContainerError: failed to stat parent`.

## When to use

When pods on a node refuse to start with the error pattern:

```
failed to create containerd container: failed to stat parent:
stat /var/lib/containerd/io.containerd.snapshotter.v1.overlayfs/snapshots/<N>/fs:
no such file or directory
```

A few of these = surgical (`ctr -n k8s.io snapshots rm <key>` cascades).
Many of these (>10) = bulk via this tool.

## Caveat (must read)

This tool addresses **one** of three corruption directions:

1. **orphan-on-disk** — dirs without DB entries. Existing
   `containerd-snapshot-gc` CronJob handles. Symptom: DiskPressure
   evictions.
2. **orphan-in-DB** — DB entries without dirs. **This tool addresses.**
   Symptom: `failed to stat parent`.
3. **dangling-parent** — DB entries whose `parent` field points at a
   non-existent bucket. **This tool can INTRODUCE this class** if a
   deleted orphan was a parent of healthy buckets. Symptom:
   `failed to create snapshot: missing parent ... bucket: not found`.

Track IMPROVE-218 for the cascade-aware v3 design that addresses all
three directions. Track BUGS-512 for the existing
`snapshot-gc-bidirec` CronJob that's wedged at init (alpine + go install
issue) and would have prevented the orphan accumulation if it had
been working.

## Build

```
cd infra/k8s/chromeless-build/tools/snap-cleaner
CGO_ENABLED=0 go build -o /tmp/snap-cleaner .
```

If you don't have Go locally, build on triform-6:

```
ssh root@triform-6 '
  apt-get install -y golang-go &&
  mkdir -p /tmp/snap-cleaner-src &&
  rsync -av --include "*.go" --include "*.mod" --include "*.sum" \
    -e ssh /path/to/this/dir/ /tmp/snap-cleaner-src/ &&
  cd /tmp/snap-cleaner-src &&
  CGO_ENABLED=0 go build -o /tmp/snap-cleaner .
'
scp triform-6:/tmp/snap-cleaner triform-N:/tmp/snap-cleaner
```

## Per-node procedure

Proven 2026-05-01 on triform-2 (305 → 0 orphans) and triform-6
(13 → 0 orphans), with zero workload disruption.

```
# 1. cordon (no new schedules during the window)
kubectl cordon triform-N

# 2. snapshot metadata.db for rollback
ssh root@triform-N
cp /var/lib/containerd/io.containerd.snapshotter.v1.overlayfs/metadata.db \
   /tmp/m.db.preop-$(date +%s)

# 3. dry-run first; verify the count looks right
/tmp/snap-cleaner

# 4. stop daemons (containerd 2.2.x reconnects to existing shims —
#    running pods keep their state through the restart)
systemctl stop kubelet
systemctl stop containerd

# 5. mutate
/tmp/snap-cleaner --apply

# 6. restart
systemctl start containerd
systemctl start kubelet
exit

# 7. verify control-plane static pods + critical workloads
kubectl get pods --all-namespaces --field-selector spec.nodeName=triform-N \
  | grep -v Running

# 8. uncordon
kubectl uncordon triform-N
```

If step 7 surfaces issues — particularly the dangling-parent symptom
described in the caveat above — the rollback is restoring the
preop snapshot:

```
systemctl stop kubelet containerd
cp /tmp/m.db.preop-<timestamp> \
   /var/lib/containerd/io.containerd.snapshotter.v1.overlayfs/metadata.db
systemctl start containerd kubelet
```

## Files

- `main.go` — the single-file Go source.
- `go.mod` — `go.etcd.io/bbolt v1.3.10` (the bbolt library).
- `README.md` — this file.

## Cross-references

- `feedback_containerd_orphan_in_db_class.md` in agent memory — full
  diagnosis recipe with the corrected on-disk-id field.
- IMPROVE-218 — structural follow-up: bidirec GC sweep that handles
  all three corruption directions.
- BUGS-512 — existing `snapshot-gc-bidirec` CronJob wedge.

// snap-cleaner — repair containerd's overlayfs snapshotter metadata when
// the metadata.db references on-disk snapshot directories that no longer
// exist (orphan-in-DB corruption class).
//
// Symptom this addresses: pods stuck in Init:CreateContainerError with
// "failed to stat parent: stat /var/lib/.../snapshots/<N>/fs: no such
// file or directory". The metadata DB has bucket entries pointing at
// snapshot dir number <N>, but the dir itself is gone (kernel
// overlay-cleanup race during pod eviction, or a containerd 2.2.x
// snapshotter bug, or — pre-fix — the existing snapshot-GC CronJob
// only handling the on-disk-leak direction).
//
// History
// -------
// 2026-05-01 — original tool authored on triform-6 by the bbolt-surgeon
// teammate while clearing 305 orphan-in-DB entries on triform-2. The
// memory file recipe (`feedback_containerd_orphan_in_db_class.md`) had
// suggested using `awk -F/ '{print $2}'` on bucket keys to extract the
// on-disk dir number; empirical run showed the bucket-key middle
// integer is a metastore counter unrelated to the snapshotter's on-disk
// dir name. The on-disk number lives in the inner `id` varint inside
// each snapshot's bucket value. This tool uses the correct path.
//
// Critical caveat (logged to IMPROVE-218 today, not yet addressed in
// this version): deleting orphan-in-DB buckets WITHOUT walking the
// parent graph can introduce a third corruption class: dangling-parent
// references. Other healthy buckets carry deleted bucket-keys in their
// `parent` field; once their parent disappears, NEW container creation
// against those healthy buckets fails with `missing parent ... bucket:
// not found`. This was observed on triform-6 after running this tool —
// 13 orphans cleared, but controlplane-watchdog kept CrashLooping with
// the dangling-parent error. A v3 cascade-aware cleaner is the
// follow-up.
//
// Build
// -----
//   cd infra/k8s/chromeless-build/tools/snap-cleaner
//   go build -o /tmp/snap-cleaner .
//   # OR with no Go locally available:
//   #   ssh root@triform-6 'cd /tmp/snap-cleaner-src && CGO_ENABLED=0 go build -o /tmp/snap-cleaner .'
//
// Use
// ---
//   /tmp/snap-cleaner                                            # dry-run, default paths
//   /tmp/snap-cleaner --apply                                    # repair, default paths
//   /tmp/snap-cleaner --db=/path/to/metadata.db --snapdir=...    # custom paths
//
// Always dry-run first. Always snapshot the metadata.db before --apply
// (`cp metadata.db /tmp/m.db.preop-$(date +%s)`).
//
// Per-node procedure (proven on triform-2 + triform-6, zero workload disruption)
// ------------------------------------------------------------------------------
// 1. kubectl cordon triform-N
// 2. cp metadata.db /tmp/m.db.preop-$(date +%s)
// 3. systemctl stop kubelet
// 4. systemctl stop containerd
// 5. /tmp/snap-cleaner --apply
// 6. systemctl start containerd
// 7. systemctl start kubelet
// 8. (verify: control-plane static pods come back; orphan count is now 0)
// 9. kubectl uncordon triform-N
//
// Active containers survive the daemon restart because containerd 2.2.x
// reconnects to existing shims. The kernel overlay mount is pinned by
// the existing dentry, so even if an ancestor on-disk dir is gone, the
// mount stays valid until container exit.

package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"time"

	bolt "go.etcd.io/bbolt"
)

type orphan struct {
	key string
	id  uint64
}

func decodeUvarint(b []byte) (uint64, int) {
	v, n := binary.Uvarint(b)
	return v, n
}

func main() {
	var dbPath, snapDir string
	var apply bool
	flag.StringVar(&dbPath, "db", "/var/lib/containerd/io.containerd.snapshotter.v1.overlayfs/metadata.db", "metadata.db path")
	flag.StringVar(&snapDir, "snapdir", "/var/lib/containerd/io.containerd.snapshotter.v1.overlayfs/snapshots", "snapshots dir")
	flag.BoolVar(&apply, "apply", false, "if true, mutate the DB; otherwise dry-run")
	flag.Parse()

	fmt.Printf("snap-cleaner v2: db=%s snapdir=%s apply=%v\n", dbPath, snapDir, apply)
	fmt.Println("Orphan check: on-disk dir == inner id varint field (NOT bucket-key middle integer)")

	db, err := bolt.Open(dbPath, 0600, &bolt.Options{Timeout: 10 * time.Second})
	if err != nil {
		fmt.Fprintf(os.Stderr, "open db: %v\n", err)
		os.Exit(2)
	}
	defer db.Close()

	var orphans []orphan
	var healthy int
	var noIDField int

	err = db.View(func(tx *bolt.Tx) error {
		v1 := tx.Bucket([]byte("v1"))
		if v1 == nil {
			return fmt.Errorf("no v1 bucket")
		}
		snaps := v1.Bucket([]byte("snapshots"))
		if snaps == nil {
			return fmt.Errorf("no v1/snapshots bucket")
		}
		return snaps.ForEach(func(k, v []byte) error {
			// Sub-buckets have v == nil
			if v != nil {
				return nil
			}
			snapBkt := snaps.Bucket(k)
			if snapBkt == nil {
				return nil
			}
			idBytes := snapBkt.Get([]byte("id"))
			if idBytes == nil {
				noIDField++
				return nil
			}
			id, n := decodeUvarint(idBytes)
			if n <= 0 {
				noIDField++
				return nil
			}
			dir := filepath.Join(snapDir, strconv.FormatUint(id, 10))
			if _, err := os.Stat(dir); os.IsNotExist(err) {
				orphans = append(orphans, orphan{key: string(k), id: id})
			} else {
				healthy++
			}
			return nil
		})
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "scan: %v\n", err)
		os.Exit(2)
	}

	fmt.Printf("Healthy snapshot buckets: %d\n", healthy)
	fmt.Printf("No id field (skipped):    %d\n", noIDField)
	fmt.Printf("Orphan-in-DB to remove:   %d\n", len(orphans))

	if !apply {
		fmt.Println("DRY-RUN — pass --apply to mutate.")
		n := 5
		if len(orphans) < n {
			n = len(orphans)
		}
		fmt.Println("Sample orphan (key, on-disk-id):")
		for i := 0; i < n; i++ {
			fmt.Printf("  id=%d key=%s\n", orphans[i].id, orphans[i].key)
		}
		return
	}

	deleted := 0
	failed := 0
	err = db.Update(func(tx *bolt.Tx) error {
		v1 := tx.Bucket([]byte("v1"))
		snaps := v1.Bucket([]byte("snapshots"))
		for _, o := range orphans {
			if err := snaps.DeleteBucket([]byte(o.key)); err != nil {
				fmt.Fprintf(os.Stderr, "delete %s: %v\n", o.key, err)
				failed++
			} else {
				deleted++
			}
		}
		return nil
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "tx: %v\n", err)
		os.Exit(2)
	}
	fmt.Printf("Deleted: %d, Failed: %d\n", deleted, failed)
}

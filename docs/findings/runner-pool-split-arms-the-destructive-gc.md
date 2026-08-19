# The 16Gi/32Gi runner split decides which runners destroy live containers

**Status:** investigated, not actioned. The runner StatefulSet and its
dind-gc sidecar belong to tf-multiverse; the fix is theirs to make. Measured
2026-08-19 against the live cluster.

**Symptom, as it reaches this repo:** CI fails in two ways that look
unrelated and both look intermittent —

```
no space left on device                                    (E2E, image pull)
Conflict. The container name "/chromeless-smoke-228"
  is already in use by container "45fdf65db096..."         (Container smoke test)
```

Same root. Neither is a code defect, and re-running "fixes" it often enough
to look flaky.

## The gate

The dind-gc sidecar's two destructive arms —

```sh
docker image     prune -f --filter until=...
docker container prune -f --filter "label!=${GC_PIN_LABEL}"
```

— both sit inside `if [ "$p" -ge "${GC_HIGH_WATER_PCT}" ]`, and
`GC_HIGH_WATER_PCT=60`. So they only run on a runner that is already ≥60%
full. Below that, nothing destructive happens at all.

The second arm removes every container *without* the sidecar's own label
**regardless of state** — including one being created right now. `docker run`
claims a container NAME before the daemon returns an id, so a container can
be destroyed inside that window: the name is taken, nothing owns it, and
every later job using that name fails on Conflict forever.

## Which runners are above the gate is not random

Sampled 3× across the fleet, 20 s apart:

| PVC size | samples | min | max | samples ≥60% (armed) |
| --- | --- | --- | --- | --- |
| 16Gi | 12 | 45% | 85% | **7** |
| 32Gi | 12 | 22% | 37% | **0** |

No 32Gi runner came within 23 points of the threshold. Every armed sample
was a 16Gi runner.

**So a job's exposure to the destructive path is decided by which runner it
lands on**, and the pool is split 4/4 by an inconsistency in PVC size. The
same workflow is safe on one half of the fleet and fatal on the other. That
is the whole reason these failures read as flaky.

## Why "just grow the 16Gi ones" is wrong

It is already recorded as tried and reverted, in-band, in
`tf-multiverse` `runner-statefulset.yaml`:

```
#   32Gi → froze the rollout: 32×14 runners = 448Gi blew the
#     hel pool's over-provisioning ceiling.
```

and separately, growing 16→24Gi was **rejected** at 89–91% pool usage. The
StorageClass is `numberOfReplicas: 2` with `nodeSelector: hel`, so only three
nodes are eligible and every gigabyte costs two.

This finding is written down because the same reasoning was re-derived from
scratch during this investigation, and the wrong action was taken before the
warning was read.

The manifest also pre-empts the obvious alternative — raising GC pressure:

```
# dind-gc CANNOT substitute for this. It fires correctly at the 60%
# high-water mark, but its prune reclaims 0B when the space is held by
# volumes of RUNNING containers — observed live on forgejo-runner-7 at 87%
```

Confirmed here: on runner-5 and runner-7 under load, `docker system df`
reported **0B reclaimable** across images, containers and volumes. On a busy
16Gi runner the prune is simultaneously *armed* (destroying in-flight
containers) and *useless* (freeing nothing). It bounds accumulated dead
layers; it is structurally blind to a live concurrent working set.

## What this repo did about it

Only what is in this repo's own control — `tests/smoke/container-boot.sh`:

* a container name that is actually unique (`$$` is not: inside a container
  the PID space is small and repeats, which is how two runs collided on
  `chromeless-smoke-228`);
* cleanup by NAME as well as by id, because a trap keyed on `CONTAINER_ID`
  cannot clean up a container that died before the id came back — which is
  precisely the window the prune destroys.

That makes this repo's jobs survive the behaviour. It does not fix it. The
fix is a consistent pool size, or an age filter on the container prune
(`--filter until=`), both of which live in tf-multiverse.

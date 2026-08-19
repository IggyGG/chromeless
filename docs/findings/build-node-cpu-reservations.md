# The chromeless build lane cannot schedule: t7/t8 CPU and memory reservations

**Status:** investigated, not actioned. Every pod named below belongs to
another team; the changes are theirs to make. Measured 2026-08-17 against
14 days of Prometheus history (the full retention window).

**Symptom:** `fire-build.sh` applies, the Job is verified, and every pod dies
`OutOfcpu` before its init container runs. The build never starts, so there is
no log to read and nothing that looks like a failure — just three dead pods.

## Why shaving the request is no longer the answer

The chromeless lane's own request has been cut three times for exactly this —
12 → 4 (2026-06-10) → 2 (2026-08-14). It is now the smallest reservation of any
real workload on the node, and the node still cannot fit it:

| node | allocatable | requested | free | actual use | lane asks |
| --- | --- | --- | --- | --- | --- |
| triform-7 | 48 | 47.4 (98%) | 0.59 | 55% | 2 |
| triform-8 | 48 | 44.0 (92%) | 3.97 | 67% | 6 |

A fourth cut would put the build below what it genuinely consumes during
`gclient sync`, and would still be one pod away from failing again.

## What the data actually says

Most reservations here are **correct or too small**, and cutting them would be
actively harmful. Peak is the maximum 5-minute rate over 14 days:

| pod | request | peak (14d) | verdict |
| --- | --- | --- | --- |
| `forgejo/forgejo-*` | 4.0 | **24.00** | under-requested; bursts 6× its reservation |
| `cargoless-serve-witness` | 2.1 | **15.19** | under-requested |
| `cargoless-serve-shard-2` | 2.1 | **13.47** | under-requested |
| `cargoless-serve-shard-3` | 2.1 | **8.11** | under-requested |
| `cargoless-serve-shard-0` | 2.1 | 2.07 | right-sized |
| `longhorn instance-manager` | 5.76 | 4.49 | right-sized |
| `coder-workspaces/pr-verifier` | 2.8 | 1.55 | reasonable |

Those are healthy burstable pods doing what burstable pods do. **Leave them
alone.**

Three are genuinely over-reserved — an order of magnitude above their observed
ceiling, sustained across the entire retention window:

| pod | node | request | peak (14d) | QoS |
| --- | --- | --- | --- | --- |
| `triform-builder/builder-fsn-build-v2-0` | t7 | **10.0** (dind 8 + builder 2) | **1.22** | Guaranteed |
| `triform-builder/builder-fsn-clippy-v3-0` | t8 | 3.0 | **0.06** | Burstable |
| `triform-wtf/postgres-standby-fsn-0` | t8 | 2.0 | **0.01** | Burstable |

`builder-fsn-build-v2-0` is the one that matters: 10 cores reserved, 1.22 peak,
and **Guaranteed** QoS (`requests == limits`), so the reservation is not a floor
it can burst above — it is the entire allocation, held whether or not a CI job
is running. Right-sizing its `dind` container from 8 to 3 (2.5× its observed
peak) would return **5 cores** to triform-7 and still leave it far more
headroom than it has ever used.

## Recommendation

For the owners of `triform-builder`, in priority order:

1. **`builder-fsn-build-v2` dind: 8 → 3 cores.** Returns 5 cores to t7, the
   most contended node. Keeps 2.5× headroom over a 14-day peak of 1.22.
   Guaranteed QoS means this reservation is pure loss while idle, and the pool
   was idle at every point sampled with no CI jobs pending.
2. **`builder-fsn-clippy-v3`: 3 → 1 core.** Peak 0.06 over 14 days. Returns 2
   cores to t8.
3. **`triform-wtf/postgres-standby-fsn-0`: 2 → 0.5 cores.** Peak 0.01. It is a
   standby; if it is ever promoted the request should rise with the role.

Together that is ~7 cores across the two build nodes — enough for the chromeless
lane at its original 12-core request, let alone its current 2.

**Counter-argument, stated fairly:** a burst pool's whole purpose is to be idle
until it is needed, and a Guaranteed reservation is how you guarantee a CI job
starts instantly instead of pending. If instant start matters more than the
5 cores, the right answer is a bigger node, not a smaller reservation. That is
a judgement for the pool's owners; this document only supplies the numbers.

## Reproducing this

```sh
kubectl port-forward -n monitoring svc/kube-prometheus-stack-prometheus 9090:9090 &
# peak 5-minute CPU rate over 14 days for one pod
curl -sG http://127.0.0.1:9090/api/v1/query --data-urlencode \
  'query=max_over_time(sum by(pod)(rate(container_cpu_usage_seconds_total{namespace="triform-builder",pod=~"builder-fsn-build-v2.*"}[5m]))[14d:10m])'
```

**Do not judge from `kubectl top`.** A single sample said `cargoless-serve-shard-3`
was using 4m and looked like an obvious candidate to shrink; its 14-day peak is
8.11 cores. The snapshot would have led to cutting a pod that needs 4× its
current request. Prometheus retention here is 15 days, so 14d is the longest
honest window.

---

## Addendum: memory is the SECOND way this lane fails to schedule (2026-08-19)

The CPU story above is not the whole shape. `OutOfmemory` is a distinct
rejection with an almost identical presentation, and it caught me today: I
fired the x264 fix at `build-job-x264.yaml` (the **t8** lane, `requests.memory:
96Gi`) and got three dead pods and no log — visually the same as the CPU case,
different reason.

Measured at the moment of the failure, then again 40 minutes later:

| node | allocatable | requested | free | 96Gi lane |
| --- | --- | --- | --- | --- |
| triform-2 | 251.4 GiB | 153.7 (61%) | 97.7 | fits, barely |
| triform-7 | 251.3 GiB | 136.9 (54%) | 114.3 | **fits** |
| triform-8 | 251.3 GiB | 165.6 (66%) | 85.6 | **CANNOT SCHEDULE** |

t8 is short by ~10 GiB. t2's 1.7 GiB of slack is not a margin — any pod that
lands there first takes the lane out too.

**So the lane choice is load-bearing, and the default is the right one.**
`fire-build.sh`'s `DEFAULT_MANIFEST` is the **t7** lane, which is both the node
with headroom and the manifest that builds 7 of 8 test targets rather than 2.
Overriding `--manifest` to a t8 lane, as I did, is choosing the node that
cannot fit it *and* the narrower target list — the exact pairing the "green
about a target list, not about the tree" trap warns about. A build fired that
way is worse than the default in both dimensions.

**Check before you fire, not after three OutOfmemory pods:**

```sh
kubectl describe node triform-8 | awk '/Allocated resources/,/^Events/' | grep -E '^  memory'
```

Compare that against the manifest's `requests.memory`. This costs one command;
discovering it from dead pods costs a cycle and produces no log to read.

The CPU findings above still stand — this is an additional constraint on the
same lane, not a correction to them.

---

## Addendum: the CI queue, and a wrong diagnosis worth recording

While landing this work, PR #64 sat unmergeable with 8 checks "pending". I
first reported that as the known cross-repo coalescer bug — **that was wrong**,
in two separate ways, and both mistakes are the kind that repeat.

**Wrong #1: the coalescer bug is already fixed.** The live CronJob
(`forgejo/forgejo-superseded-run-coalescer`) scopes its dead-PR arm with
`pr.base_repo_id = r.repo_id`, and its own comment documents the exact
chromeless#44 incident that motivated the fix. Prior notes describing it as
unfixed are stale. **Read the deployed object, not your notes about it.**

**Wrong #2: "no runs exist" came from querying the wrong thing.** The
`/actions/tasks` API lists runs that reached a RUNNER. Queued runs have no task
row (`task_id = 0`), so they are invisible there — which reads exactly like
"CI never scheduled anything". The database says otherwise:

```sql
SELECT r.id, r.status, r.ref, substring(r.commit_sha,1,8), r.workflow_id
  FROM action_run r JOIN repository p ON p.id = r.repo_id
 WHERE p.name = 'chromeless' AND r.commit_sha LIKE '<sha>%';
-- status 5 = waiting, 6 = running, 3 = cancelled, 1 = success, 2 = failure
```

**What was actually happening: label starvation.** The runners advertise
`["docker","rust","ubuntu-22.04","ubuntu-latest"]` — one pool serving several
labels. A 156-job `docker` backlog from tf-multiverse had every slot, while 9
`ubuntu-latest` jobs (8 of them mine) waited behind it:

| | |
| --- | --- |
| runners in the pool | 26 |
| tasks running | 43 |
| `docker` jobs waiting | 156 |
| `ubuntu-latest` jobs waiting | 9 |
| `docker` completions / 15 min | 66 |

That is ~35 minutes of backlog, not a wedge — and it drains on its own. The
tell that separates the two: **runners polling recently** (`last_online` within
seconds) with **jobs whose `task_id` is 0**. Healthy runners plus unassigned
jobs means queued-behind-something. Dead runners plus unassigned jobs means
wedged.

```sql
-- is the queue moving, or stuck?
SELECT (SELECT count(*) FROM action_task WHERE status=6)                AS running,
       (SELECT count(*) FROM action_run_job WHERE status=5)             AS waiting,
       (SELECT count(*) FROM action_run_job
         WHERE status IN (1,2)
           AND stopped > extract(epoch from now())::bigint - 900)       AS done_15m;
```

If `done_15m` is healthy, wait. If it is zero with runners online, escalate.

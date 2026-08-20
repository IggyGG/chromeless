# Working in this repo

Orientation for anyone — human or agent — making changes here. It records the
things that cost time to rediscover, not things you can read off the tree.

## What this is

A Chromium **embedder** (`capture/`, C++) that runs a libwebrtc peer inside the
browser process and streams a real browser over WebRTC. Around it: a Go
signaling broker (`signaling/`), a TypeScript client (`client/`), a Go k8s
controller and Helm chart (`infra/`).

It is used in production by a separate system ("triform": portal → physics →
isolator), and is also meant to work for people with none of that.

## First: run this

```bash
make verify        # ~15 s. Everything checkable without Chromium or Docker.
```

`make help` lists the rest. If you change C++, read the next section before
you believe anything.

## The single most important constraint

**You cannot compile `capture/` here.** It is an out-of-tree Chromium
embedder; building it needs a full `gclient sync` plus a 4–8 hour build
(`build/chromeless-build.sh`, ~1 h warm with sccache). There is no local
Chromium tree and no shortcut.

Consequences:

- The compiler is not available as a feedback signal for most edits.
- A missing `#include` or a wrong `base::` API surfaces hours later in the
  build lane — historically, in a lane nobody was watching.
- **Verify C++ against in-tree precedent.** Before using a Chromium API,
  `grep` for existing uses of it in `capture/`. This tree pins
  `refs/branch-heads/7727` (≈M147); APIs drift across rolls, and several
  commits in the log are pure drift fixes (`JSONReader::ReadDict`,
  `raw_ptr<AudioDeviceModule>`, `RtpTransceiverDirectionToString`).
- `make lint-cxx` catches the recurring subset — symbols used without their
  header. It exists because `Cb.shutdown` shipped using `base::BindOnce` and
  `FROM_HERE` with neither header included. It is a lint, not a compiler:
  clean means "that specific mistake is absent", not "this builds".

When you finish C++ work, **say plainly that it is unverified.** Do not
describe it as done or working.

### Firing the build lane

Use `infra/k8s/chromeless-build/fire-build.sh <ref>`, not `kubectl apply`.
It renders the manifest from `git show <ref>:<path>`, then reads the **live
Job object back** and refuses to leave it running if the spec disagrees with
the ref. Hand-firing desynced the manifest from the live Job three times in
one afternoon (2026-07-30), and every time the build ran to completion and
reported a verdict about the wrong target list.

`--keep-going` sets `NINJA_KEEP_GOING=0` so one pass collects every failing
TU. Without it each drift error costs a full ~30 min cycle to discover.

## Traps that have cost real time

- **A resource LIMIT with no REQUEST is a RESERVATION, and its failure has
  no log.** Kubernetes mirrors an omitted request from the limit, so
  `limits: {ephemeral-storage: 8Gi}` reserves 8Gi at admission. On a node
  that is already heavily reserved the pod is *rejected before it starts* —
  no container, therefore no log — and the Job silently burns retries. The
  lane reads as mysteriously dead rather than as a resource problem.

  Fixed as an instance three times and never as a class (kaniko-push
  2026-06-29, t7 2026-07-02, t8 2026-08-20) while three further build lanes
  and three validation Jobs carried it latent. `make lint-pod-resources`
  now fails on it.

  The wider trap: **reservations here are fiction, so never size a request
  against them.** Measured on triform-8, 2026-08-20 — 81Gi of ephemeral
  reservations against 2.5Gi of actual use; the build's own writable layer
  is 522Mi because all heavy I/O is on the `/work` hostPath. The same
  mistake had already been made twice with cpu (12→6→2) and once with
  memory (96Gi request against a measured 57Gi peak). Size a request
  against **what your workload consumes**, measured; the limit is what
  governs burst.

- **A green build lane is green about a target list, not about the tree.**
  `capture/**/BUILD.gn` declares seven `test()` targets; for ~10 weeks three
  of them appeared in no build manifest at all. They were declared, reviewed,
  merged, and compiled by nothing. When they were finally added, the first
  build failed immediately — `CbAudioTestRecorder` implemented two of
  `webrtc::AudioTransport`'s three pure virtuals, so it was abstract and the
  test could not declare one. Broken the whole time, green the whole time.
  `make lint-build-targets` now fails if any `test()` target is built by no
  lane (and if a lane names a target no `BUILD.gn` declares).

  The header's own `TODO` had predicted the failure exactly, including that
  the build pod would be what found it. **A TODO that names its own
  verification step is a defect nobody has run yet** — treat one as a task,
  not a note.

- **`.claude/worktrees/` holds full checkouts of this repo.** Any repo-wide
  `grep`/`find` returns every hit N+1 times unless you exclude it. It is
  gitignored; keep excluding it in searches.
- **CDP clients must pass `local: true`.** `GET /json/protocol` CHECK-FATALs
  the worker, so an ordinary `chrome-remote-interface` connect SIGABRTs it.
  Nine test files carry this workaround.
- **`Cb.*` is hand-dispatched, not generated.** There is no PDL and no
  `/json/protocol` entry. Params are read with bare `FindString`/`FindBool`
  calls, so an unknown or misspelled key is *silently ignored* — this has
  produced two production bugs (`signalingUseTls` read as `useTls`;
  array-form `iceServers` read only as a string, silently falling back to
  public STUN). If you add a param, add it to the alias/validation path too.
- **`docker compose` needs `CHROMELESS_IMAGE`, `CHROMELESS_USER`, and
  `CHROMELESS_PASS`.** No image is published anywhere; you supply one. The
  credentials gate the gateway and have no default on purpose — a built-in
  password looks like protection. Compose fails fast on each.
- **The standalone stack publishes exactly one port: the gateway (8443).**
  DevTools (9222) and the broker (8080) are internal. Do not republish them to
  make a test easier: 9222 is unauthenticated remote code execution against the
  browser and the image passes `--remote-allow-origins=*`, and the broker with
  no `CHROMELESS_AUTH_PUBKEY` accepts anyone. Use `docker compose exec`.
- **coturn: `lt-cred-mech` for the compose profile, `use-auth-secret` for k8s.**
  These are NOT interchangeable. `signaling/turn.go` only emits static
  credentials, which cannot authenticate against a `use-auth-secret` relay —
  pairing them yields a relay that refuses every allocation while looking
  healthy, with no error anywhere and no video. The Helm chart and
  `infra/k8s/turn-deployment.yaml` are the HMAC-REST half and pair with
  `infra/turn-issuer/`.
- **An "unconfigured" worker is not idle — it restart-loops.**
  `infra/lifecycle/cold-start.sh:63` defaults `SIGNALING_URL` to
  `ws://signaling:8080/ws`, which `launch-chromeless.sh` turns into a real
  dial. Where that name does not resolve, the handshake fails
  (`ERR_NAME_NOT_RESOLVED`) and the browser process **exits**; supervisord
  respawns it about every 30 s. DevTools keeps answering `/json/version`
  throughout, so the pod looks healthy and a CDP client sees a page target that
  vanishes under it. For a genuine CDP-only worker set `SIGNALING_URL=""` —
  that is what leaves `WEBRTC_SIGNALING_HOST` unset, which is the condition
  `cloud_browser_browser_main_parts.cc:843` checks before skipping signaling.
- **`webSocketDebuggerUrl` often has NO PORT** (`ws://localhost/devtools/...`).
  Parsing it naively and defaulting to 80 dials nothing, or something else
  listening locally. Always force 9222 — `infra/gateway/cdp.go` and
  `tests/cdp/conftest.py` both do this, and `infra/k8s/standalone/smoke-probe.py`
  had to learn it the same way.
- **CI runs on Forgejo, and every `uses:` must exist on its action mirror.**
  Forgejo *does* execute `.github/workflows` (no `.forgejo/` needed). But it
  resolves actions from `data.forgejo.org`, and it **git-clones every `uses:`
  in a job before executing any step** — so one unresolvable action fails the
  whole job regardless of its `if:`. The logs show `skipping post step for
  'actions/checkout@v4'; step was not executed`, i.e. the job died before
  checkout.

  This was catastrophic and invisible: **171 of 175 runs failed, with zero
  successes across June–July 2026.** Root causes were unresolvable actions
  (`github-script`, `hadolint-action`, `lychee-action`, `codeql-action`,
  `sbom-action`, `attest-build-provenance`, `action-gh-release`) plus the
  unbuildable `infra/Dockerfile`. Prefer a `run:` step over a third-party
  action; `make lint-workflows` enforces this. A job-level `if:` IS evaluated
  before resolution, so gating a whole job (as `codeql.yml` does) works.

- **Reading CI logs.** The API gives status but not text. For the actual
  failure:
  ```sh
  kubectl exec -n forgejo forgejo-green-postgres-0 -c postgres -- \
    psql -U forgejo -d forgejo -t -A -F'|' -c \
    "SELECT t.log_filename FROM action_task t
       JOIN action_run_job j ON j.task_id=t.id
       JOIN repository p ON p.id=j.repo_id
      WHERE p.name='chromeless' AND j.status=2 ORDER BY t.id DESC LIMIT 5;"
  # then copy it out (the forgejo container has no zstdcat) and decode.
  # The log is PLAIN timestamped text, NOT newline-delimited JSON — the
  # json.loads() one-liner this file used to recommend dies on line 1 with
  # "Extra data". Just strip NULs:
  kubectl cp -n forgejo -c forgejo forgejo-<pod>:/data/actions_log/<file> ./x.zst
  zstdcat x.zst | tr -d '\000' > x.txt
  grep -nE "::error|FAIL|No space left|Job failed" x.txt | cut -c30-180 | tail
  ```

  **Job status codes — the full set. Getting these wrong invents outages.**

  | code | meaning | trap |
  | --- | --- | --- |
  | 1 | success | |
  | 2 | failure | |
  | 3 | cancelled | |
  | 4 | **skipped** | reads as `pending` forever in the commit-status API |
  | 5 | **WAITING** | *not* running — misreading 5 as "in flight" turned a normal queue into a phantom "311 jobs claimed but never started" (2026-08-19) |
  | 6 | running | |

  `pull_request.status` is a DIFFERENT enum on the same instance:
  **0=conflict, 1=checking, 2=mergeable**. A PR at 1 cannot be merged (the API
  returns 405) no matter how green its checks are.

  A Forgejo API token lives in `../tf-multiverse/.env` as `FORGEJO_API_TOKEN`.

## Docs: which to trust

| Trust | Treat as history |
| --- | --- |
| `docs/protocols/` — per-channel wire specs, versioned, accurate | `PROJECT_BRIEF.md` — describes stock Chromium + `getDisplayMedia`; that architecture is gone |
| `docs/build/chromium-from-source.md` — pinning and rolls | `docs/phase-{0,1}-exit-report.md` |
| `capture/signaling/cb_wire_envelope.h` — the authoritative wire contract | Anything referencing `capture/streamer-page/` (deleted in M7) |
| `README.md` — rewritten and accurate as of 2026-07 | `docs/audits/`, `docs/measurements/` — point-in-time |
| `docs/operations/standalone.md` + `infra/gateway/README.md` — the self-hosted path | Anything describing a published `:3000` client or `:8080` broker — that topology is gone |

`docs/operations/triform-deploy.md` documents one real cluster. Useful as a
worked example; not a generic guide.

## Work in your own worktree. Never in the shared checkout.

**Before you touch anything, make a worktree:**

```bash
git worktree add .claude/worktrees/<task> -b <branch>
git worktree lock .claude/worktrees/<task> --reason "active session: <task>"
cd .claude/worktrees/<task>
```

Multiple agents and multiple Claude sessions routinely share this repo
directory, and git gives them no interlock. The primary checkout is for
orientation and spawning worktrees — not for authoring.

This is not hygiene advice. On 2026-08-11 a session in this repo:

1. ran `git checkout <other-branch>`, which **aborted** because
   `infra/compose.yaml` had uncommitted changes,
2. did not check the exit status and committed anyway — landing the commit on
   a *concurrent session's* branch,
3. ran `git reset --hard HEAD~1` to undo that, which **destroyed the other
   session's uncommitted work**: 61 insertions / 148 deletions, never staged,
   therefore absent from the reflog, from `fsck --lost-found`, and from any
   stash. Permanently lost.

The abort message in step 1 named the file that was at risk. It was ignored.

Rules that follow:

- **A failed `git checkout` means STOP**, not "continue and commit". Check
  `$?`, or check `git branch --show-current` before committing.
- **`git reset --hard` in a shared checkout is a destructive operation on
  someone else's data.** Use `git reset --soft` to undo your own commit while
  keeping the tree.
- **Never use `git stash` here.** This repo and tf-multiverse both carry
  stashes belonging to other work (21 in tf-multiverse at last count). To move
  a file between branches use `git show <sha>:<path> > /tmp/...`.
- **`.claude/worktrees/` holds full checkouts and is gitignored.** Exclude it
  from every repo-wide `grep`/`find` or you get N+1 hits.

## Working with the user: decisions must be ASKED, not written down

**If the user has to decide something, use the AskUserQuestion tool. Every
time. Not a paragraph at the end of a message.**

This is not a style preference. A message that ends with "three things are
yours to decide" reads as *finished* — the user sees a completed task and no
prompt, so the thread goes dead while an agent believes it is politely
waiting. That happened repeatedly on 2026-07-30: three open decisions (merge
a green PR, where a RED-by-design test should live, whether to reshard a
backup CronJob) sat in prose across several messages and the session stalled.

Rules:

- A decision only counts as raised once it has gone through
  `AskUserQuestion`. Prose alongside it is fine; prose *instead* of it is not.
- Ask **at the moment you're blocked**, not in a summary at the end. If you
  discover the question mid-task, finish everything that doesn't depend on the
  answer first, then ask.
- Put your recommendation first and label it `(Recommended)`. You have the
  context; make the call easy.
- Batch related decisions into one call (up to 4 questions) rather than
  stringing out several rounds.
- **Don't** ask for things you can determine yourself — a conventional
  default, a fact in the repo, or a choice the user already made. Asking about
  those is its own kind of noise.

What *does* warrant asking: anything outward-facing or hard to reverse (merges,
deploys, prod image promotion, destructive storage operations), and any fork
where different answers mean materially different work.

## Before you report a number, ask what it counted

Every wrong claim made in this repo on 2026-08-19 — five of them in one
session, by two different agents — had the same shape: a query returned a
number, the number was believed, and nobody asked which rows it silently
excluded. None was a reasoning failure. Each was a *counting* failure.

**The four checks, in the order they cost time:**

1. **Did the work actually run?** A job at `status=5` never started; it is not
   a pass and not a fail. Filter `COALESCE(started,0) > 0` before computing any
   pass rate, or "never ran" is indistinguishable from "ran clean". Reported
   recovery: `8 pass / 0 fail`. Actual: `64/2` and `56/8`.

2. **Is the boundary where you think it is?** A fleet rebuild spanned
   15:56–16:25Z. Cutting at 16:00 counted five mid-rebuild failures as
   post-fix. Cut at the END of a transition, not its start, and say which you
   used.

3. **Is it one sample or a rate?** Five consecutive passes against a ~7% base
   rate happens ~70% of the time with no change at all. Give the n. "0 failures
   since the fix" over five runs is not evidence of a fix.

4. **Same reading twice?** PR mergeability was diagnosed three different wrong
   ways because each theory fit one snapshot. Sampling the *same* PRs twenty
   minutes apart refuted all three at once. If a conclusion rests on a
   distribution, re-read the individuals.

**And the meta-check:** if a fix "worked", find the row that should have
changed and confirm it did. `docker image prune` reporting
`Total reclaimed space: 0B` is the fix working; `290.1MB` is it failing. That
one line is worth more than any amount of reasoning about what the code does.

### A guard whose skipped path is silent is not a guard

Four independent instances of this landed in one day, in two repos:

| site | what was skipped | what it looked like |
| --- | --- | --- |
| `if command -v pgrep` around a kill sweep | the whole sweep, on any image without procps | a flaky test |
| a job borrowing dind-gc's own pin label | the pin (GC deletes that label) | the step succeeded |
| log capture scheduled after the specs | the diagnosis, on a `compose up` failure | an 836-byte artifact |
| a DataChannel falling into `other =>` | the entire feature | a working channel |

**Review question: does the guarded-OUT path report differently from the
succeeded path?** If not, it is a silent no-op waiting for the wrong
environment. Prefer a fallback; if a guard is genuinely required, make the
skipped branch *say so*.

Two specific forms worth memorising:

- `if ! cmd; then rc=$?` reads the **negated** pipeline's status, which is
  always 0 — it turns a failure into a green. Use `rc=0; cmd || rc=$?`.
- A guard keyed to **line numbers** in a file other people edit
  (`sed -n '1000,1140p'`) goes stale as the file grows, and fails as a false
  ALARM — which trains everyone to ignore the check that would have caught the
  real thing. Anchor on structure.

### Ask the repo how it lands changes before diagnosing why it didn't

Sibling repos land changes differently, and guessing costs hours. In
`tf-multiverse` a PR does **not** land by merging it: a merge train does, and
every commit on `dev` is a `lane candidate: pr-NNNNN`. A PR nobody submitted
sits open forever with green checks and looks wedged.

The tools say so directly, and each answer is one command:

```sh
scripts/lane-submit --pr N --status   # NOT_ENROLLED / PENDING_INGEST / QUEUED
scripts/fj pr status N                # names the gate AND the remedy, authoritative
```

`scripts/lane-submit` is the landing step (`dev-merge` execs it —
`tf-multiverse/CLAUDE.md:106`), and `docs/runbooks/landing-routes.md` exists
precisely for "my PR will not move".

**This was written after spending hours attributing two stuck PRs to a Forgejo
defect.** They were `NOT_ENROLLED`. The evidence against the theory was already
in hand — other PRs had landed the same day carrying the same
`pull_request.status=1` that was supposedly blocking mine — and the correct
mechanism was documented in the sibling repo's own `CLAUDE.md`, which is the
first file the instructions say to read.

Two habits that would have caught it:

- **When a contradiction appears in your own data, chase it immediately.** "The
  thing I claim is blocking has demonstrably not blocked others" is a refutation,
  not a curiosity.
- **A per-repo status tool beats a general theory.** Before explaining why
  something is stuck, run whatever the repo provides for reporting *why* — it
  encodes the mechanism, so it cannot be wrong about it the way an inference can.

Related failure mode: `lane-submit --status` reported `PENDING_INGEST` for a PR
the ingester log showed it had already enqueued. The summary view lags; the log
and lane membership are ground truth.

### A green PR that will not merge is probably not your PR

`pull_request.status` (0=conflict, 1=checking, 2=mergeable) is separate from
every check on the PR. A PR at **1 cannot be merged — the API returns 405** —
however green it is, and the state is entered once and never restored:

- a **base** push invalidates it, and so does a **head** push. Pushing a fix to
  your own open PR is therefore self-defeating: **finish the branch before you
  open the PR.**
- close/reopen, `GET`ting the PR, and pushing an empty commit were each tested
  and do **not** re-trigger the check.
- `forgejo doctor check --run recalculate-merge-bases` repairs a *different*
  defect (650 stale mergebases on 2026-08-19). It does not fix this — confirmed
  by checking which PRs were in its report before running `--fix`.

Read `pull_request.status` in postgres; the API's `mergeable` field is the same
information but the enum above is what makes it interpretable.

**Unresolved, deliberately:** chromeless #74 held status=2 for five hours and
merged; tf-multiverse #14055 dropped 2→1 in ninety seconds, untouched. Neither
base branch moved during its window, so base churn does not explain the
difference. Candidate variables nobody has tested: base-branch activity in
general, and per-repo queue depth (`dev` is by far the busiest). Do not report
this as understood — three separate confident explanations (starved consumer,
arrival-only checking, a throughput deficit) were each refuted by re-reading
the same PRs twenty minutes later.

### A PR whose CI runs were pruned can never land

Forgejo prunes `action_run` rows, but the commit **statuses** survive. So the
API still lists a context as pending while the job that would report it no
longer exists — and anything gating on that context waits forever.

Measured 2026-08-20, two repos independently:

```
tf-multiverse #13906 (44aabf6d):  60 commit statuses,  0 jobs
chromeless overall:              233 of 411 commits with statuses have 0 jobs
```

Diagnose by comparing the two tables for the same SHA — a context with no
backing job is the signature:

```sql
SELECT count(*) FROM commit_status cs JOIN repository p ON p.id=cs.repo_id
 WHERE p.name='chromeless' AND cs.sha LIKE '<sha>%';
SELECT count(*) FROM action_run_job j JOIN action_run r ON r.id=j.run_id
 JOIN repository p ON p.id=r.repo_id WHERE p.name='chromeless'
   AND r.commit_sha LIKE '<sha>%';
```

Mostly harmless — old merged commits nobody waits on. It bites only when the
orphaned commit is the HEAD of an **open** PR. The cure is a new commit, which
schedules fresh runs.

This is the THIRD distinct Forgejo defect on this instance, alongside the
`status=1` wedge above and stale merge bases (`forgejo doctor check --run
recalculate-merge-bases`). They are independent; fixing one does not touch the
others. Unestablished: whether pruning is age-, count-, or GC-driven.

### Put a known-positive in every probe

An instrument that can only return one answer is not evidence. Before
believing a measurement of absence, measure something you *know* is present
with the same command.

Three instances in two days, each nearly producing a false finding:

- `strings <guest-image> | grep control` returned **0** — and would have been
  reported as "this image has no channels at all". `strings` is not installed
  in that image. The tell was probing `clipboard`, a channel known to be
  there, and also getting 0. `grep -ac` then gave real numbers.
- A `sed` meant to inject a defect into a lint's negative control silently
  failed to match (12-space indent, not 14), so a clean file was compared
  against itself and `clean / clean` was read as the two arms agreeing.
- A repo-wide `grep` for a symbol returned nothing because the search was
  running from a dead CWD, not because the symbol was absent.

The cost is one extra probe. The saving is the difference between "the feature
is missing" and "my instrument is missing", which are indistinguishable
outputs from an unchecked tool.

Same shape as the silent-no-op section above, one level up: there the *action*
was skipped invisibly, here the *measurement* is. Ask both questions —
did the thing run, and could my check have detected it if it had?

### When CI says your code is wrong, check whether it said so before

An error message names a cause; it is not evidence of one. On 2026-08-19 disk
exhaustion arrived wearing three disguises, only the first of which is honest:

```
No space left on device                                    (honest)
rustc-LLVM ERROR: IO failure on output stream              (on a zero-Rust branch)
ERROR — extracted region lacks the pool acquire            (a guard blaming the tree)
```

The third sends you into your own code. Before believing it: **did this same
check pass on this same branch in an earlier run?** If it did and nothing
relevant changed, suspect the runner. Cheap and decisive — it costs one query
and it would have saved an afternoon.

Corollary for the opposite direction: when a job fails, confirm the log
contains the evidence your fix *produces* before concluding the fix failed. A
missing "Killed" line meant the run predated the commit, not that the fix was
wrong.

## Conventions

- **Comments explain *why*.** This codebase is unusually heavily commented and
  that is deliberate — most comments record a failure that was expensive to
  diagnose. Match it: when you fix something subtle, write down what the
  symptom was, not just what the code does.
- **Match the surrounding style** rather than importing your own.
- Protocol changes update the spec in `docs/protocols/` in the same change.
- Don't add a lint rule without checking it against the whole tree first. A
  false positive costs more trust than a missed defect costs time.

## Layout

```
capture/          the embedder (the product)
  build-integration/  main parts, CDP agent, input dispatch, content client
  signaling/          WS client, offerer driver, wire envelope, ICE config
  framesink-capturer/ Viz capture → libwebrtc video track source
  encoder/            x264 / VP9 / NVENC / VAAPI / SVT-AV1 behind one factory
signaling/        Go broker
client/           TypeScript client + demo page
infra/            gateway (standalone TLS/login/nav), compose, Helm, CRDs, lifecycle
build/            Chromium build orchestration + runtime image
tools/lint/       host-runnable static checks
tests/            smoke · integration · e2e · webrtc drivers
```

Ad-hoc cluster drivers and one-shot forensics go in `tests/scratch/`
(gitignored) so they never land in a commit.

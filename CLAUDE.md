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
- **`docker compose` needs `CHROMELESS_IMAGE`.** No image is published
  anywhere; you supply one. Compose fails fast with a message.
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
  # then copy it out (the forgejo container has no zstdcat) and decode:
  kubectl cp -n forgejo -c forgejo forgejo-<pod>:/data/actions_log/<file> ./x.zst
  zstdcat x.zst | python3 -c "import sys,json;[print(json.loads(l).get('content','').rstrip()) for l in sys.stdin]"
  ```
  Job status codes: 1=success, 2=failure, 4=skipped.
  A Forgejo API token lives in `../tf-multiverse/.env` as `FORGEJO_API_TOKEN`.

## Docs: which to trust

| Trust | Treat as history |
| --- | --- |
| `docs/protocols/` — per-channel wire specs, versioned, accurate | `PROJECT_BRIEF.md` — describes stock Chromium + `getDisplayMedia`; that architecture is gone |
| `docs/build/chromium-from-source.md` — pinning and rolls | `docs/phase-{0,1}-exit-report.md` |
| `capture/signaling/cb_wire_envelope.h` — the authoritative wire contract | Anything referencing `capture/streamer-page/` (deleted in M7) |
| `README.md` — rewritten and accurate as of 2026-07 | `docs/audits/`, `docs/measurements/` — point-in-time |

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
infra/            compose, Helm, CRDs + controller, lifecycle scripts
build/            Chromium build orchestration + runtime image
tools/lint/       host-runnable static checks
tests/            smoke · integration · e2e · webrtc drivers
```

Ad-hoc cluster drivers and one-shot forensics go in `tests/scratch/`
(gitignored) so they never land in a commit.

# Contributing to chromeless

Start with [`CLAUDE.md`](./CLAUDE.md). It is written for anyone making changes
here, human or agent, and it records the things that cost time to rediscover.
This file is the short procedural version.

## Before anything else

```bash
make verify        # ~15 s. Everything checkable without Chromium or Docker.
make help          # the rest
```

## Work in your own worktree

Several people and several agent sessions share this repository directory, and
git gives them no interlock. The primary checkout is for orientation and for
spawning worktrees, never for authoring:

```bash
git fetch origin
git worktree add .claude/worktrees/<task> -b <branch> origin/main
git worktree lock .claude/worktrees/<task> --reason "active session: <task>"
cd .claude/worktrees/<task>
```

Branch from `origin/main`, not from whatever the shared checkout happens to be
on — it has sat on a stale branch for weeks at a time. Rules that follow, with
the incident behind each in `CLAUDE.md`:

- a failed `git checkout` means stop, not "continue and commit";
- never `git reset --hard` or `git stash` in the shared checkout;
- `.claude/worktrees/` is gitignored and holds full checkouts — exclude it from
  every repo-wide search.

## Changes land through reviewed pull requests

External contributors should open pull requests at
<https://github.com/IggyGG/chromeless>. Maintainers relay the reviewed exact
head to `forgejo.triform.dev/triform/chromeless`, whose protected CI and
integration lane remains authoritative before the result reaches `main`.
Internal contributors can open the Forgejo PR directly. Finish the branch
before opening its Forgejo PR — a push to an open PR can leave it in a
"checking" state that never resolves (`CLAUDE.md` explains the mechanism).

CI executes `.github/workflows/` on both hosts. Public GitHub jobs use
GitHub-hosted runners and never receive Triform's private registry credentials;
Forgejo resolves actions from its own mirror and runs the integration jobs.
Prefer a `run:` step to a third-party `uses:`; `make lint-workflows` enforces
this.

## Commits

Conventional-commit subjects (`fix(gateway): …`, `feat(client): …`,
`docs: …`, `test(interactive): …`) with a body that records **why** — what the
symptom was, what was measured, what was ruled out. The log is the project's
memory; read a few recent bodies before writing yours.

Stage by exact path. `git add -A` and `git add .` have swept other people's
in-progress files into the wrong commit more than once.

## C++ in `capture/` is unverified until the build lane says otherwise

`capture/` is an out-of-tree Chromium embedder. There is no local Chromium tree
and no way to compile it here; the only compiler is a 1–8 h build lane on a
cluster. So:

- grep `capture/` for an existing use of any Chromium API before you use it, and
  check `docs/build/chromium-7727-api-pins.md`;
- run `make lint-cxx` (a lint, not a compiler);
- write **UNVERIFIED** in the commit body and the PR, and say so plainly in any
  summary. Do not describe C++ work as done or working until the lane has built
  it and `tests/interactive/` has run against the resulting image.

## Tests

| change touches | run |
| --- | --- |
| anything | `make verify` |
| `client/` | `cd client && npm run typecheck && npm test` |
| `infra/gateway/`, `signaling/` | `go test ./...` in that module |
| the user-visible page | `tests/local/` against a deployed stack (real Chrome) |
| input, data channels, dialogs | `tests/interactive/` against a deployed stack |
| `capture/` | the build lane, then `tests/interactive/` on the new image |

A test you have not watched fail proves nothing. When you fix a bug, revert the
fix once and confirm the test goes red.

## Documentation

- Protocol changes update `docs/protocols/` in the same change.
- Comments explain **why**, usually by recording the failure that was expensive
  to diagnose. Match the surrounding style.
- A confirmed defect that cannot be fixed in the same change is written up in
  `docs/findings/` with the file and symbol that would fix it.
- `docs/README.md` says which documents are current and which are history. Keep
  it that way when you add one. `docs/roadmap-ga.md` is the plan of record.

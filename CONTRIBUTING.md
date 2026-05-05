# Contributing to chromeless

Short guide for the team working in a shared worktree across many
parallel tasks. Lives next to [`PROJECT_BRIEF.md`](./PROJECT_BRIEF.md).

## Staging discipline

We work in a single shared worktree. That means several teammates'
work-in-progress files sit side-by-side in the same directory tree
between commits.

**Stage by exact path. Never `git add -A`, `git add .`, or
`git add <directory>` without a clear list of paths.**

Why: those broad forms have repeatedly swept other teammates'
in-progress files into the wrong commit (e.g., T72's stats handler
landed inside T71's commit, T76's binary landed inside its own commit
because nobody pruned it). When that happens:

- The commit message lies about scope.
- Bisect across history loses meaning.
- Reviewers miss diffs that are nominally in someone else's lane.

The right pattern:

```sh
# good — every file is named explicitly
git add capture/streamer-page/streamer.js \
        capture/chromeless-metrics-sidecar/stats_handler.go \
        docs/protocols/stats-channel.md

# also good — pathspec-magic when a directory is genuinely all-yours
git add 'capture/chromeless-metrics-sidecar/*.go' \
        'capture/chromeless-metrics-sidecar/*.md'
```

Things to avoid:

```sh
# BAD — sweeps everything in the worktree
git add -A
git add .

# RISKY — fine in your own private fork; not in the shared worktree
git add capture/        # if anyone else has WIP in capture/
git add tests/          # likewise
```

`git status` before staging, `git diff --cached` before committing.
Built artifacts (Go binaries, dist/ output, *.pid, etc.) belong in
`.gitignore`, not in commits — see the existing
[`.gitignore`](./.gitignore) and any module-local
`*/.gitignore` for the patterns.

## Commit messages

- One commit per task with the exact `T<N>: <short description>`
  message specified in the task's DoD.
- Drive-by fixes (touching files outside your task's lane) get a
  one-line note in the commit message body, not their own commit,
  unless they're substantive enough to need a separate task.
- Co-author lines are optional; we don't require them across
  teammates.

## Branches

- We commit to `main` directly in this worktree. Feature branches
  add coordination overhead the team-lead → teammate task model
  doesn't need.
- Force-pushing or rewriting history on `main` is forbidden.
  Mistakes are fixed forward.

## Testing

- Each module has its own test suite (Go: `go test ./...`; client:
  `npm test`; integration: `cd tests/integration && go test ./...`).
- Run the relevant suite before committing.
- The integration tests in `tests/integration/` are a single Go
  module — if your change touches a file that other teammates'
  tests import (e.g., the shared `envelope` type), the entire
  module's compile health is your problem to keep green even on a
  drive-by edit.
- Don't commit binaries (`signaling/signaling`,
  `capture/chromeless-metrics-sidecar/chromeless-metrics-sidecar`,
  `signaling/turn-issuer/turn-issuer`, etc.). Each module's
  `.gitignore` excludes them; if you see one in `git status`, it
  means an old binary leaked through — delete and re-run
  `go build` into a different output path.

## Cross-team tasks

When a task description says "lead is webrtc-dev; flag chromium-dev
and infra-dev for review":

- The lead writes the central piece (e.g., the protocol or design
  doc + the central module).
- Adjacent-lane edits are the lead's call: do them yourself if
  they're ≤ ~50 lines and uncontroversial; flag the relevant
  teammate in your team-lead message if they're larger.
- The team-lead routes the review on its end.

## Documentation

- Protocol specs live under [`docs/protocols/`](./docs/protocols/).
- Internal trade-off notes live under [`docs/internal/`](./docs/internal/).
- Operational runbooks and observability live under [`infra/`](./infra/).
- One short markdown file per topic; cross-link rather than
  duplicate.

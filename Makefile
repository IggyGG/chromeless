# chromeless — top-level Makefile
#
# Canonical entry points for tests. See tests/README.md for the full
# strategy. Targets that aren't yet wired print a "not implemented" notice
# pointing at the task that will deliver them, rather than silently passing.

.PHONY: help test test-unit test-integration test-smoke test-smoke-all \
        test-harness test-harness-all test-e2e test-all-ci test-all-nightly \
        test-unit-signaling test-unit-client test-unit-harness

help:
	@echo "Targets:"
	@echo "  make test                # legacy alias for test-all-ci (the PR gate)"
	@echo "  make test-all-ci         # PR-blocking subset (unit + integration + smoke)"
	@echo "  make test-all-nightly    # PR-blocking + harness baselines + e2e"
	@echo ""
	@echo "  make test-unit           # all subproject unit tests"
	@echo "  make test-integration    # tests/integration/ — Go toolchain only, no Docker"
	@echo "  make test-smoke          # tests/smoke/container-boot.sh (single canonical smoke)"
	@echo "  make test-smoke-all      # all tests/smoke/*.sh (Linux + Docker)"
	@echo "  make test-harness        # tests/harness/loopback-baseline.sh (single hermetic check)"
	@echo "  make test-harness-all    # all four hermetic harness baselines"
	@echo "  make test-e2e            # tests/e2e/ Playwright specs (full compose stack)"
	@echo ""
	@echo "See tests/regression-suite.md for the full reference + gate split."

# Legacy alias — prior CI configs may invoke `make test`. New work
# should use `make test-all-ci` so the PR-blocking subset is explicit.
test: test-all-ci

# PR-blocking subset (the gate every PR must clear).
# - unit:      < 60 s, no Docker
# - integration: < 30 s, builds signaling binary in TestMain
# - smoke:     ~12 s, container-boot.sh (Docker required; Linux runners on CI)
test-all-ci: test-unit test-integration test-smoke

# Nightly subset (the broader gate; runs every night via dedicated workflows).
# Adds:
# - harness-all:  hermetic baselines (loopback / aliased-warning / sink-lag /
#                 input-latency)
# - e2e:          Playwright against full compose stack
# Note: nightly does NOT include opt-in CHROMELESS_INTEGRATION_LIVE Go tests
# (audio_loopback) — those run inside e2e via Playwright spec 05 anyway.
test-all-nightly: test-all-ci test-harness-all test-e2e

# ---- unit ------------------------------------------------------------------

test-unit: test-unit-signaling test-unit-client test-unit-harness

test-unit-signaling:
	@if [ -d signaling ] && ls signaling/*.go >/dev/null 2>&1; then \
	  echo ">>> go test ./signaling/..."; \
	  ( cd signaling && go test ./... ); \
	else \
	  echo "skip: signaling has no Go sources yet (T13)"; \
	fi

test-unit-client:
	@if [ -f client/package.json ]; then \
	  echo ">>> npm test (client)"; \
	  ( cd client && npm ci --silent && npm test ); \
	else \
	  echo "skip: client has no package.json yet (T14)"; \
	fi

test-unit-harness:
	@if [ -f harness/pyproject.toml ] || [ -f harness/requirements.txt ]; then \
	  echo ">>> pytest harness"; \
	  ( cd harness && python -m pytest ); \
	else \
	  echo "skip: harness has no Python project yet (T11)"; \
	fi

# ---- integration -----------------------------------------------------------

# tests/integration/ is its own Go module (it builds the signaling binary
# from a sibling module), so we cd into it rather than `go test ./...` from
# the repo root. See tests/integration/README.md.
test-integration:
	@if [ -f tests/integration/go.mod ]; then \
	  echo ">>> go test ./... (tests/integration)"; \
	  ( cd tests/integration && go test ./... ); \
	else \
	  echo "skip: tests/integration/ not populated yet"; \
	fi

# ---- smoke -----------------------------------------------------------------

test-smoke:
	@if [ -x tests/smoke/container-boot.sh ]; then \
	  bash tests/smoke/container-boot.sh; \
	else \
	  echo "TODO(T9): tests/smoke/container-boot.sh not implemented yet"; \
	  echo "          (blocked on T7 — infra/Dockerfile)"; \
	  exit 1; \
	fi

# Run every smoke script in tests/smoke/. Each is independent — failure
# of one doesn't short-circuit the others, but the target exits non-zero
# if any failed (after running them all, so the operator sees a complete
# picture). Linux + Docker required for all of them.
test-smoke-all:
	@set +e; failures=0; \
	for s in tests/smoke/*.sh; do \
	  [ -x "$$s" ] || continue; \
	  echo ""; echo ">>> $$s"; \
	  if ! bash "$$s"; then failures=$$((failures+1)); fi; \
	done; \
	if [ "$$failures" -gt 0 ]; then \
	  echo ""; echo "test-smoke-all: $$failures script(s) failed"; \
	  exit 1; \
	fi

# ---- harness ---------------------------------------------------------------

test-harness:
	@if [ -x tests/harness/loopback-baseline.sh ]; then \
	  bash tests/harness/loopback-baseline.sh; \
	else \
	  echo "TODO(T12): tests/harness/loopback-baseline.sh not implemented yet"; \
	  echo "           (blocked on T11 — harness/latency/reconcile.py)"; \
	  exit 1; \
	fi

# Run every hermetic harness baseline. Same fail-aggregate pattern as
# test-smoke-all. Excludes phase1-baseline.sh — that's operator-only
# (needs a real cam.mp4 + jsonl, not bundled).
#
# y4m-loopback-baseline.sh is also hermetic (decodes 30 frames from
# the bundled or auto-generated y4m fixture) so it lives here. The
# fixture itself is .gitignore'd; the script auto-regenerates when
# missing on a dev box, or skips loudly on a CI runner without
# ffmpeg + chromium.
test-harness-all:
	@set +e; failures=0; \
	for s in tests/harness/loopback-baseline.sh \
	         tests/harness/aliased-warning-baseline.sh \
	         tests/harness/sink-lag-baseline.sh \
	         tests/harness/input-latency-loopback.sh \
	         tests/harness/y4m-loopback-baseline.sh; do \
	  [ -x "$$s" ] || { echo "skip: $$s missing or not executable"; continue; }; \
	  echo ""; echo ">>> $$s"; \
	  if ! bash "$$s"; then failures=$$((failures+1)); fi; \
	done; \
	if [ "$$failures" -gt 0 ]; then \
	  echo ""; echo "test-harness-all: $$failures script(s) failed"; \
	  exit 1; \
	fi

# ---- e2e -------------------------------------------------------------------

# Playwright E2E (T33). Heavyweight: needs Docker for the compose stack
# and a one-time `npx playwright install --with-deps chromium` to pull
# the browser binaries. Not in the default `make test` PR gate — see
# tests/README.md. Use `npm install --silent` so first-run output stays
# manageable; subsequent runs are no-ops.
test-e2e:
	@if [ -f tests/e2e/package.json ]; then \
	  echo ">>> playwright test (tests/e2e)"; \
	  ( cd tests/e2e && npm install --silent --no-audit --no-fund && npm run test:e2e ); \
	else \
	  echo "skip: tests/e2e/ not populated yet (Phase 1+)"; \
	fi

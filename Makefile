# cloud-browser-webrtc — top-level Makefile
#
# Canonical entry points for tests. See tests/README.md for the full
# strategy. Targets that aren't yet wired print a "not implemented" notice
# pointing at the task that will deliver them, rather than silently passing.

.PHONY: help test test-unit test-smoke test-harness test-e2e \
        test-unit-signaling test-unit-client test-unit-harness

help:
	@echo "Targets:"
	@echo "  make test           # = test-unit + test-smoke (the PR gate)"
	@echo "  make test-unit      # all subproject unit tests"
	@echo "  make test-smoke     # tests/smoke/ — requires Docker"
	@echo "  make test-harness   # tests/harness/ validation; requires loopback rig"
	@echo "  make test-e2e       # tests/e2e/ — Phase 1+"
	@echo ""
	@echo "See tests/README.md for the full testing strategy."

test: test-unit test-smoke

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

# ---- smoke -----------------------------------------------------------------

test-smoke:
	@if [ -x tests/smoke/container-boot.sh ]; then \
	  bash tests/smoke/container-boot.sh; \
	else \
	  echo "TODO(T9): tests/smoke/container-boot.sh not implemented yet"; \
	  echo "          (blocked on T7 — infra/Dockerfile)"; \
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

# ---- e2e -------------------------------------------------------------------

test-e2e:
	@if [ -d tests/e2e ] && [ -n "$$(ls -A tests/e2e 2>/dev/null)" ]; then \
	  echo "TODO: wire E2E runner (Phase 1+)"; \
	  exit 1; \
	else \
	  echo "skip: tests/e2e/ not populated yet (Phase 1+)"; \
	fi

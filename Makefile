# chromeless — top-level Makefile
#
# Canonical entry points for tests. See tests/README.md for the full
# strategy. Targets that aren't yet wired print a "not implemented" notice
# pointing at the task that will deliver them, rather than silently passing.

.PHONY: help standalone-up standalone-down verify lint lint-cxx lint-workflows lint-github-boundary lint-shell lint-build-targets \
        lint-runtime-contracts lint-tests-wired lint-pod-resources lint-yaml-dupe-keys \
        lint-guest-release lint-deploy-pin test-interactive test test-unit test-integration \
        test-smoke test-smoke-all test-harness test-harness-all test-e2e \
        test-all-ci test-all-nightly \
        test-unit-signaling test-unit-client test-unit-harness \
        test-unit-go-modules test-unit-encoder

help:
	@echo "Targets:"
	@echo "  make verify              # START HERE — everything runnable without a"
	@echo "                           # Chromium tree or Docker (~15 s)"
	@echo ""
	@echo "  make lint                # all static checks"
	@echo "  make lint-cxx            # C++ include lint — catches missing #includes"
	@echo "                           # that would otherwise fail 4-8 h into a build"
	@echo "  make lint-workflows      # every 'uses:' must exist on the CI action mirror"
	@echo "  make lint-github-boundary # public jobs cannot consume private worker images"
	@echo "  make lint-build-targets  # every test() target is built by some lane"
	@echo "  make lint-pod-resources  # ephemeral-storage limit implies a reservation"
	@echo "  make lint-yaml-dupe-keys # a duplicate key silently discards the first value"
	@echo "  make lint-guest-release  # the worker-image pin names real code"
	@echo "  make lint-deploy-pin     # manifests agree with the guest-release pin"
	@echo "  make test-interactive    # real Chrome + real worker (needs a live stack)"
	@echo "  make lint-runtime-contracts # hermetic launcher/runtime contracts"
	@echo ""
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
	@echo "  make standalone-up       # the self-hosted stack, one command, Docker only"
	@echo "                           # (CHROMELESS_IMAGE=<worker image> is the one input)"
	@echo "  make standalone-down     # stop it; credentials and certs are kept"
	@echo ""
	@echo "See tests/regression-suite.md for the full reference + gate split."

# ---- verify ----------------------------------------------------------------
#
# The default entry point for a human or an agent who just changed something.
#
# Everything here runs WITHOUT a Chromium checkout and WITHOUT Docker, in
# roughly fifteen seconds. That constraint is the point: the embedder in
# capture/ can only be compiled inside a full Chromium tree (4-8 h cold, see
# build/chromeless-build.sh), so for most edits the compiler is simply not
# available as a feedback signal. `make verify` is what stands in for it.
#
# It is NOT a substitute for the build lane. A clean `make verify` means the
# fast checks pass; C++ still needs a real build before you trust it.
verify: lint test-unit test-integration
	@echo ""
	@echo ">>> verify OK — fast checks pass."
	@echo ">>> NOTE: capture/ was not compiled (needs a Chromium tree)."
	@echo ">>>       Run the build lane before trusting C++ changes."

# ---- lint ------------------------------------------------------------------

lint: lint-cxx lint-workflows lint-github-boundary lint-shell lint-build-targets lint-runtime-contracts \
      lint-tests-wired lint-silent-noop lint-pod-resources lint-yaml-dupe-keys \
      lint-guest-release lint-deploy-pin

# Every `uses:` must exist on the CI host's action mirror. Forgejo resolves
# all of them before running any step, so one missing action fails the whole
# job — this was the largest single cause of CI failures in this repo.
# Offline by default; `--online` re-probes the mirror.
lint-workflows:
	@echo ">>> workflow actions lint"
	@python3 tools/lint/workflow_actions_lint.py

# The checked-in worker pin is private. GitHub-hosted runners must never be
# given Triform registry credentials, so image-dependent jobs need an explicit
# public-image opt-in while Forgejo continues to run them unconditionally.
lint-github-boundary:
	@echo ">>> GitHub public/private image boundary lint"
	@python3 tools/lint/github_public_boundary_lint.py
	@python3 tools/lint/test_github_public_boundary_lint.py >/dev/null && \
	  echo ">>> GitHub boundary lint self-tests pass" || \
	  { echo "!!! GitHub boundary lint SELF-TESTS FAILED"; exit 1; }

# Static include check for the Chromium embedder. See the docstring in
# tools/lint/cxx_include_lint.py for what it does and deliberately doesn't.
lint-cxx:
	@echo ">>> cxx include lint"
	@python3 tools/lint/cxx_include_lint.py capture
	@python3 tools/lint/test_cxx_include_lint.py >/dev/null && \
	  echo ">>> cxx-include-lint self-tests pass" || \
	  { echo "!!! cxx-include-lint SELF-TESTS FAILED — the linter itself is broken"; exit 1; }

# Every test() target declared in capture/ must appear in some build lane's
# CHROMELESS_BUILD_TARGETS. Three of the seven did not, for ~10 weeks, and the
# first compile after they were added failed immediately — see the docstring.
# The lanes were green about a smaller set of files than the tree contains.
lint-build-targets:
	@echo ">>> build targets lint"
	@python3 tools/lint/build_targets_lint.py
	@python3 tools/lint/test_build_targets_lint.py >/dev/null && \
	  echo ">>> build-targets-lint self-tests pass" || \
	  { echo "!!! build-targets-lint SELF-TESTS FAILED — the linter itself is broken"; exit 1; }

# The worker-image pin must name real code, and every lane that boots a worker
# must read it rather than the out-of-git CHROMELESS_IMAGE variable. That
# variable went weeks stale with no check able to say so, and the e2e lane's
# first real run failed on the PIN rather than on the code.
lint-guest-release:
	@echo ">>> guest release pin lint"
	@python3 tools/lint/guest_release_lint.py

# Hermetic runtime contracts — static greps plus a launcher dry-run with
# CHROMELESS_BROWSER_BIN=/bin/echo, so nothing executes. No Docker, no Chromium,
# no cluster; the whole set runs in well under a second.
#
# These three lived in tests/runtime/ invoked by NOTHING. Two passed the entire
# time. The third had been failing for so long that its failure was invisible —
# it asserted a pre-M7 token-in-URL shape and read capture/streamer-page/, a
# directory deleted in M7. Rewriting it surfaced a second dead assertion:
# CHROMELESS_AUTOSTART_STREAMER is not referenced by the launcher at all.
#
# A red test that nothing runs is indistinguishable from no test.
# Every test entrypoint under tests/ must be reachable from a make target, a
# workflow, a build script, or a k8s pod template. 34 files were reachable from
# NONE of those — written, reviewed, merged, run by nothing. One had been RED
# for months without anyone knowing.
#
# Sibling of lint-build-targets, which exists because three test() targets went
# uncompiled for ten weeks. Same defect, one directory over, same fix.
lint-tests-wired:
	@echo ">>> tests-wired lint"
	@python3 tools/lint/tests_wired_lint.py

# Two idioms whose FAILURE is indistinguishable from success, both of which
# shipped green while doing nothing:
#   `if ! cmd; then rc=$$?`  — $$? is the negated pipeline's status, always 0,
#                              so a failed step exits 0 and reports GREEN.
#   `sed -n 'N,Mp' other/file | grep` — an assertion keyed to line numbers goes
#                              stale as the file grows and fails as a FALSE
#                              ALARM (58 failures / 45 PRs, 2026-08-19).
# Self-tested against the real historical defects: a lint that has never fired
# is indistinguishable from one that cannot fire, which is this very bug.
lint-silent-noop:
	@echo ">>> silent no-op lint"
	@python3 tools/lint/silent_noop_lint.py .
	@python3 tools/lint/test_silent_noop_lint.py >/dev/null && \
	  echo "    self-test: both arms pass" || \
	  { echo "    self-test FAILED — the lint cannot be trusted"; exit 1; }

# An ephemeral-storage LIMIT with no REQUEST is a RESERVATION, because K8s
# mirrors the omitted request from the limit. On a node that is already
# heavily ephemeral-reserved the pod is rejected at admission -- which means
# no container, and therefore NO LOG. The Job retries and every attempt dies
# identically, so the lane reads as mysteriously dead rather than as a
# resource problem. That gap between symptom and cause is why this is a lint.
#
# Measured on triform-8 2026-08-20: 81Gi of ephemeral RESERVATIONS across the
# node corresponded to 2.5Gi of ACTUAL use, and the build image's writable
# layer is 522Mi. The reservations were fiction and the build could not run.
#
# Fixed three times as an instance and never as a class: kaniko-push
# (2026-06-29), build-job-x264-t7 (2026-07-02), build-job-x264/t8
# (2026-08-20). Three further lanes (nvenc, vaapi, x264-t2) and three
# validation Jobs were found carrying it latent, never having been fired on
# a full node. Same defect, six directories over, same fix.
# Every manifest that pins a deployable image must agree with
# build/guest-release.json. Exists because a user lost a day to a stack whose
# manifest and cluster disagreed: `kubectl apply` silently rolled the worker
# back to a build predating the fix it was meant to carry.
#
# Also requires the pin to carry a sha256 DIGEST, because a tag is not
# evidence of which artifact runs — the gateway's standalone-v9 tag was
# rebuilt over by another session while the Deployment still reported v9.
# Right tag, wrong artifact.
lint-deploy-pin:
	@echo ">>> deploy pin lint"
	@python3 tools/lint/deploy_pin_lint.py

lint-pod-resources:
	@echo ">>> pod resource request lint"
	@python3 tools/lint/pod_resource_request_lint.py infra
	@python3 tools/lint/test_pod_resource_request_lint.py >/dev/null 2>&1 && \
	  echo ">>> pod-resource-request-lint self-tests pass" || \
	  { echo "!!! pod-resource-request-lint SELF-TESTS FAILED — the linter itself is broken"; exit 1; }

# A duplicate mapping key silently discards the EARLIER value. Found in
# build-job-x264-t7.yaml on 2026-09-08, where twenty lines of measured
# reasoning argued for a 64Gi memory request while every build actually got
# the 72Gi on the next line. kubectl, PyYAML and the lane all accept it
# without a word, so nothing but this catches it.
lint-yaml-dupe-keys:
	@echo ">>> yaml duplicate key lint"
	@python3 tools/lint/yaml_duplicate_key_lint.py .

lint-runtime-contracts:
	@echo ">>> runtime contracts"
	@fail=0; \
	for f in tests/runtime/*.sh; do \
	  printf '  %-46s ' "$$(basename $$f)"; \
	  if bash "$$f" >/tmp/rt-$$$$.log 2>&1; then echo "ok"; \
	  else echo "FAIL"; sed 's/^/      /' /tmp/rt-$$$$.log | head -6; fail=1; fi; \
	  rm -f /tmp/rt-$$$$.log; \
	done; \
	if [ $$fail -ne 0 ]; then echo "!!! a runtime contract failed"; exit 1; fi

lint-shell:
	@if command -v shellcheck >/dev/null 2>&1; then \
	  echo ">>> shellcheck"; \
	  shellcheck infra/launch-chromeless.sh infra/lifecycle/*.sh tests/smoke/*.sh 2>&1 | head -40; \
	else \
	  echo "skip: shellcheck not installed"; \
	fi

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
test-all-nightly: test-all-ci test-harness-all test-e2e test-interactive

# ---- unit ------------------------------------------------------------------

test-unit: test-unit-signaling test-unit-go-modules test-unit-client test-unit-harness \
           test-unit-encoder

# Every OTHER Go module in the tree. `test-unit` used to run `signaling/` and
# nothing else, so eight modules holding 13 _test.go files were gated by no make
# target and no workflow — while tests/regression-suite.md:38-46 listed them as
# PR-blocking. They were not. Six passed the whole time; nobody was looking.
#
# Discovered by `find -name go.mod` rather than hardcoded, so a new module is
# picked up automatically instead of silently joining the ungated pile. That is
# the same failure shape as the unbuilt test() targets (see lint-build-targets)
# and it is fixed the same way: derive the list, never hand-maintain it.
#
# Reconfirmed from the other direction when infra/gateway/ landed on
# feat/standalone-mode with 31 tests that no target invoked: `make verify` was
# green and had run none of them. Two branches independently hit the same gap
# within days and wrote the same fix, which is about as clear a signal as this
# kind of thing gives.
#
# signaling/ is excluded (it has its own target above) and tests/integration/
# too (that is `make test-integration` — it builds a binary and is not a unit
# test). .claude/worktrees/ holds full checkouts of this repo, so it MUST be
# pruned or every module is found N+1 times; see CLAUDE.md.
test-unit-go-modules:
	@echo ">>> go test — every module except signaling/ and tests/integration/"
	@fail=0; \
	for gomod in $$(find . -name go.mod -not -path "./.claude/*" \
	                  -not -path "./signaling/*" \
	                  -not -path "./tests/integration/*" | sort); do \
	  d=$$(dirname $$gomod); \
	  printf '  %-52s ' "$$d"; \
	  if ( cd $$d && go test ./... >/tmp/gomod-$$$$.log 2>&1 ); then \
	    echo "ok"; \
	  else \
	    echo "FAIL"; sed 's/^/      /' /tmp/gomod-$$$$.log | head -8; fail=1; \
	  fi; \
	  rm -f /tmp/gomod-$$$$.log; \
	done; \
	if [ $$fail -ne 0 ]; then echo "!!! a Go module failed"; exit 1; fi

# 13 node:test cases pinning encoderImplementation strings, so a silent fallback
# to Chromium's stock encoders fails loudly. Run by nothing until now, despite
# passing in 1.3s with zero dependencies — and despite this repo having already
# shipped exactly that regression once (every image was profile=sw for two
# months because the profile pipe was severed and nothing checked).
test-unit-encoder:
	@if [ -f tests/webrtc/encoder-assertions.test.mjs ]; then \
	  echo ">>> node --test tests/webrtc/encoder-assertions.test.mjs"; \
	  node --test tests/webrtc/encoder-assertions.test.mjs; \
	else \
	  echo "skip: tests/webrtc/encoder-assertions.test.mjs not present"; \
	fi

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

# The guard used to test `harness/pyproject.toml` / `harness/requirements.txt`.
# NEITHER HAS EVER EXISTED. The real file is one directory down —
# harness/latency/requirements.txt — so this target printed "skip: ... yet (T11)"
# on every run since it was written and nobody read it. A target that can only
# ever skip is the silent-skip class living in the Makefile itself.
#
# It still skips when there are no tests to run, but now says which of the two
# reasons applies, because "no python project" and "python project with no
# tests" want different fixes.
test-unit-harness:
	@if [ ! -f harness/latency/requirements.txt ] && [ ! -f harness/pyproject.toml ]; then \
	  echo "skip: harness has no Python project (looked for harness/latency/requirements.txt)"; \
	elif ! find harness -name 'test_*.py' -o -name '*_test.py' | grep -q .; then \
	  echo "skip: harness has a Python project but ZERO test files — nothing to run."; \
	  echo "      (this is a real gap, not a passing state; see the reconciler in harness/latency/)"; \
	else \
	  echo ">>> pytest harness"; \
	  ( cd harness && python3 -m pytest ); \
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

# The two-sided real-user suite: real Google Chrome running the real client
# bundle against a real deployed worker, with the WORKER'S OWN DevTools as the
# independent oracle. Input is dispatched as genuine DOM events on the
# client's <video>, never as synthetic CDP input at the client — that would
# bypass the very code under test.
#
# This target exists because the suite was invoked by NOTHING: no make target,
# no workflow, zero hits across all six. It escaped tools/lint/tests_wired_lint
# only because tests/**/README.md counts as an invoker and its README names
# run.py — true for a human, false for CI. 28 checks that nothing ran.
#
# NOT in test-all-ci. It needs a deployed standalone stack and cluster access;
# run-against-cluster.sh arranges the port-forwards, reaps stray test Chromes,
# checks the fixture endpoint and reads the credentials — each of which gave a
# plausible wrong answer at least once when done by hand. Nightly, beside
# test-e2e. Pass INTERACTIVE_ARGS="--only mouse" or "--restart".
#
# Prerequisites are documented in tests/interactive/README.md. The suite
# reports what is missing rather than failing obscurely.
test-interactive:
	@if [ -f tests/interactive/run-against-cluster.sh ]; then \
	  echo ">>> interactive suite (tests/interactive) — needs a deployed standalone stack"; \
	  ./tests/interactive/run-against-cluster.sh $(INTERACTIVE_ARGS); \
	else \
	  echo "skip: tests/interactive/ not present"; \
	fi

# ---- standalone: the self-hosted stack, one command --------------------------
#
# Wraps infra/standalone-up.sh: builds the gateway image (which carries keygen
# and worker-token, so no Go toolchain is needed on the host), mints the keypair
# + the worker's token + a login into infra/.env ONCE, brings the stack up with
# `compose up --wait`, and prints the URL and credentials. CHROMELESS_IMAGE is
# the one input you must bring — this repo publishes no worker image.
standalone-up:
	@./infra/standalone-up.sh

standalone-down:
	@./infra/standalone-up.sh --down

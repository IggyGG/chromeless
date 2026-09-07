#!/usr/bin/env bash
# infra/standalone-up.sh — the standalone stack, one command, Docker only.
#
#   CHROMELESS_IMAGE=<worker image> ./infra/standalone-up.sh
#   CHROMELESS_IMAGE=<worker image> ./infra/standalone-up.sh --down
#
# Or `make standalone-up` / `make standalone-down` from the repo root.
#
# WHAT THIS REPLACES
# ------------------
# The documented quickstart was three steps:
#
#   eval "$(cd infra/gateway && go run ./cmd/keygen)"
#   CHROMELESS_IMAGE=… CHROMELESS_USER=… CHROMELESS_PASS=… \
#     docker compose -f infra/compose.yaml up
#
# and it had three ways to go wrong that this script closes:
#
#   1. It needed a Go toolchain on the host for keygen — the ONE prerequisite
#      the rest of the stack had already removed. The tools are now baked into
#      the gateway image, and this script runs them from there.
#   2. The three exports lived in the shell that ran `eval`. Open a new
#      terminal, run `compose up` again, and the broker boots with a DIFFERENT
#      key than the one the worker's token was signed with — every connection
#      then dies as a signature mismatch and neither log says the keys differ.
#      This script writes them to infra/.env, which compose reads from the
#      directory of the compose file on every invocation, so they are stable
#      across terminals and restarts and rotate only when you ask.
#   3. Choosing a password. Left to the operator it is `hunter2` in the docs
#      and in far too many deployments. Generated here, once, and printed.
#
# WHAT IT DOES NOT DO: build the worker image. That is a from-source Chromium
# build (4–8 h; build/chromeless-build.sh) and this repo publishes no image, so
# CHROMELESS_IMAGE stays the one thing you must bring. It fails fast on that
# before pulling anything.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE="$HERE/compose.yaml"
ENV_FILE="$HERE/.env"
GATEWAY_IMAGE="chromeless-gateway:dev"

die() { echo "standalone-up: $*" >&2; exit 1; }

compose() { docker compose -f "$COMPOSE" "$@"; }

if [[ "${1:-}" == "--down" ]]; then
    compose down
    echo "standalone-up: stack is down. Credentials and certificates are kept:"
    echo "  $ENV_FILE          (delete to rotate the keys and password)"
    echo "  volume gateway_certs  (docker volume rm chromeless_gateway_certs to re-warn browsers)"
    exit 0
fi

# ---- preconditions ----------------------------------------------------------

command -v docker >/dev/null 2>&1 || die "docker is not on PATH. Install Docker Desktop or Docker Engine first."
docker info >/dev/null 2>&1 || die "the Docker daemon is not running (docker info failed)."
docker compose version >/dev/null 2>&1 || die "docker compose (v2) is not available."

[[ -n "${CHROMELESS_IMAGE:-}" ]] || die "CHROMELESS_IMAGE is not set.
  The browser is a from-source Chromium build and this repo publishes no image
  (build/chromeless-build.sh), so you have to name one:
    CHROMELESS_IMAGE=my-registry/chromeless:cr7727-abc1234 $0"

case "$(uname -m)" in
    arm64|aarch64)
        echo "standalone-up: NOTE this host is $(uname -m) and the worker image is amd64-only."
        echo "  Docker will run it under emulation, which is slow or fails outright."
        echo "  See docs/operations/standalone.md for running the worker on a Linux box or cluster."
        ;;
esac

# ---- 1. credentials, once ---------------------------------------------------

if [[ -f "$ENV_FILE" ]]; then
    echo ">>> using existing credentials in $ENV_FILE (delete it to rotate)"
else
    echo ">>> building the gateway image (it carries keygen)"
    # `docker build`, NOT `compose build`: compose interpolates the WHOLE file
    # for every subcommand, and CHROMELESS_USER / CHROMELESS_PASS are `?err`
    # required — so `compose build gateway` fails on the very variables this
    # step exists to create. Found by running it.
    docker build -q -f "$HERE/gateway/Dockerfile" -t "$GATEWAY_IMAGE" "$HERE/.." >/dev/null \
        || die "the gateway image failed to build (docker build -f infra/gateway/Dockerfile .)"

    echo ">>> minting the keypair, the worker's token, and a login"
    # keygen prints three `export` lines; strip the keyword for a compose .env.
    # SESSION_ID is threaded through so the token is scoped to the session the
    # worker actually joins — a mismatch is refused as `sid mismatch`, which
    # reads like a signing problem rather than a naming one.
    SESSION_ID="${SESSION_ID:-dev}"
    keys="$(docker run --rm -e "SESSION_ID=$SESSION_ID" --entrypoint /keygen "$GATEWAY_IMAGE" 2>/dev/null)" \
        || die "keygen failed inside $GATEWAY_IMAGE — is the image built from a tree that has infra/gateway/cmd/keygen?"
    # 20 alphanumerics from the OS entropy source; no dependency on openssl.
    pass="$(head -c 64 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | cut -c1-20)"
    [[ ${#pass} -eq 20 ]] || die "could not generate a password from /dev/urandom"

    umask 077
    {
        echo "# Written by infra/standalone-up.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ). Delete to rotate."
        echo "# compose reads this file from the directory of compose.yaml on every invocation."
        echo "$keys" | sed 's/^export //'
        echo "SESSION_ID=$SESSION_ID"
        echo "CHROMELESS_USER=${CHROMELESS_USER:-chromeless}"
        echo "CHROMELESS_PASS=${CHROMELESS_PASS:-$pass}"
    } > "$ENV_FILE"
    echo "    wrote $ENV_FILE (mode 0600)"
fi

# CHROMELESS_IMAGE is deliberately NOT written to .env: the image is what you
# roll, and a pin that survives in a dotfile is how a stale guest gets tested.
export CHROMELESS_IMAGE

# ---- 2. up --------------------------------------------------------------------

echo ">>> starting the stack"
compose up -d --wait 2>&1 | tail -5 || {
    echo
    echo "standalone-up: a service did not become healthy. What compose knows:"
    compose ps -a --format 'table {{.Name}}\t{{.State}}\t{{.ExitCode}}\t{{.Status}}'
    echo "  A dead container reports as 'unhealthy'. Check the ExitCode column before the probe."
    echo "  Worker log: docker compose -f $COMPOSE exec chromium cat /var/log/supervisor/chromium.err.log"
    exit 1
}

# ---- 3. tell the operator what to do -----------------------------------------

port="$(grep -E '^CHROMELESS_PORT=' "$ENV_FILE" 2>/dev/null | cut -d= -f2- || true)"
port="${CHROMELESS_PORT:-${port:-8443}}"
user="$(grep -E '^CHROMELESS_USER=' "$ENV_FILE" | cut -d= -f2-)"
pass="$(grep -E '^CHROMELESS_PASS=' "$ENV_FILE" | cut -d= -f2-)"

cat <<MSG

  chromeless is up.

    open      https://localhost:${port}
    username  ${user}
    password  ${pass}

  The certificate is self-signed: accept it once and the wss:// dial is covered
  too (same origin). The worker takes ~15-45 s to boot Chromium; the page
  connects on load and says "waiting for offer" until then.

  logs      docker compose -f $COMPOSE logs -f
  down      $0 --down
MSG

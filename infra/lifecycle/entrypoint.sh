#!/usr/bin/env bash
# infra/lifecycle/entrypoint.sh — container PID-1 entrypoint chain.
#
# Order:
#   1. dumb-init (Dockerfile ENTRYPOINT) -> this script
#   2. cold-start.sh: per-session bootstrap; writes resolved env to
#      /run/chromeless-session/env
#   3. source /run/chromeless-session/env so SESSION_ID and friends propagate
#      through supervisord and into the chromium / idle-watchdog
#      programs (supervisord's environment= directive is additive on top
#      of the inherited env).
#   4. exec supervisord — it inherits our env and we hand off PID 2 to it.

set -eu

/usr/local/bin/cold-start.sh

if [ -f /run/chromeless-session/env ]; then
    set -a
    # shellcheck disable=SC1091
    . /run/chromeless-session/env
    set +a
fi

exec /usr/bin/supervisord -c /etc/supervisor/supervisord.conf

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

# The chromium program's stderr_logfile is /dev/console (CV2-ICE
# observability, 9cb4d01): firecracker captures the guest console to
# the worker host's serial.log, which is what makes ICE-gathering
# diagnosable from outside the microVM. But /dev/console only exists
# in a VM — a k8s pod or plain docker container has none, and
# supervisord then fails the program with the maximally-misleading
#   spawnerr: unknown error making dispatchers for 'chromium': EACCES
# retrying forever, chromium never starts. Found 2026-07-30 by the
# first REAL container run of CI (smoke/e2e with CHROMELESS_IMAGE set):
# every container-runtime consumer of this image was broken; only
# firecracker guests worked. Fix at the seam that knows the substrate:
# if there is no usable console, provide the file sink in its place so
# the conf stays a single artifact and firecracker keeps its serial
# stream. (Symlink, not conf-rewrite: supervisord re-reads the path on
# every respawn, and the conf is baked read-only for cbuser.)
if [ ! -w /dev/console ] 2>/dev/null; then
    ln -sf /var/log/supervisor/chromium.err.log /dev/console 2>/dev/null || {
        # /dev may be read-only for us (runAsNonRoot pods). Fall back to
        # rewriting a runtime copy of the conf — /etc is root-owned, so
        # copy to /run (always writable tmpfs in this image's layout).
        sed 's|^stderr_logfile=/dev/console$|stderr_logfile=/var/log/supervisor/chromium.err.log|' \
            /etc/supervisor/supervisord.conf > /run/supervisord.runtime.conf
        exec /usr/bin/supervisord -c /run/supervisord.runtime.conf
    }
fi

exec /usr/bin/supervisord -c /etc/supervisor/supervisord.conf

#!/bin/sh
# infra/lifecycle/scrub-pod.sh — scrub the chromeless container's
# tenant-touched state in-place so the pod can return to the warm
# pool. Called by the BrowserSession controller via Pod-exec when
# the pool's `spec.recyclePolicy: ScrubAndReturn` (T90).
#
# Idempotent. Safe to run on a warm pod (no-op on empty profile).
# Exits 0 with literal "[scrub] OK" on stdout when the post-scrub
# verification passes; non-zero on any failure.
#
# Contract with the controller:
#   - controller calls scrub-pod.sh via `kubectl exec`-equivalent.
#   - controller greps stdout for "[scrub] OK" — diagnostic logs go
#     to stderr.
#   - non-zero exit OR missing OK marker → controller falls back to
#     RecreatePod (deletes the pod, replenishment loop creates a
#     fresh one). Failures are observable but never silent.
#
# What gets scrubbed:
#   - Chromium user-data-dir: cookies, history, localStorage,
#     IndexedDB, Service Workers, downloaded files.
#   - Other home subdirs Chromium writes to (Downloads/, .cache/,
#     .pki/, .config/dconf/).
#   - /tmp scratch (excluding the X11 socket dir which other
#     sidecars depend on).
#   - /run/chromeless-session/{id,env} so the next chromium restart picks up
#     fresh per-session env (set by the controller via supervisord
#     environment= update OR by the next pod-level cold-start.sh).
#
# What is NOT scrubbed:
#   - Read-only image content under / (we run readOnlyRootFilesystem).
#   - Volumes the controller doesn't own (PulseAudio sockets — they
#     get reset by chromium-restart anyway).
#   - System dotfiles Chromium didn't touch (.bashrc etc).

set -eu

USER_DATA_DIR="/home/cbuser/.config/chromium"
HOME_DIR="/home/cbuser"
CHROMELESS_SESSION_DIR="/run/chromeless-session"
SUPERVISORCTL="/usr/bin/supervisorctl"
SUPERVISORD_CONF="/etc/supervisor/supervisord.conf"

err() { printf '[scrub] %s\n' "$*" >&2; }
log() { printf '[scrub] %s\n' "$*" >&2; }

log "starting at $(date -u +%Y-%m-%dT%H:%M:%SZ)"

# --- 1. stop chromium so it releases handles to user-data-dir ----------
if [ -x "$SUPERVISORCTL" ]; then
    "$SUPERVISORCTL" -c "$SUPERVISORD_CONF" stop chromium >&2 || true
fi

# --- 2. wipe user-data-dir ---------------------------------------------
if [ -d "$USER_DATA_DIR" ]; then
    # rm with hidden-file globbing across the directory contents but
    # NOT the directory itself (we want to re-use the mountpoint).
    find "$USER_DATA_DIR" -mindepth 1 -delete 2>/dev/null || {
        err "FAIL: could not clear $USER_DATA_DIR"
        exit 1
    }
fi

# --- 3. wipe other tenant-written home subdirs -------------------------
for d in Downloads .cache .pki .config/dconf .config/google-chrome; do
    # ${HOME_DIR:?} so a misconfigured / empty $HOME_DIR can never
    # expand to "rm -rf /$d" (shellcheck SC2115).
    if [ -d "${HOME_DIR:?}/$d" ]; then
        rm -rf "${HOME_DIR:?}/$d"
    fi
done

# --- 4. wipe per-session env so a fresh value lands on next start ------
# We delete; the next entrypoint chain (after a pod-level cold-start) or
# the controller's environment= update will write the new SESSION_ID.
rm -f "$CHROMELESS_SESSION_DIR/id" "$CHROMELESS_SESSION_DIR/env"

# --- 5. clear /tmp (preserve the X11 socket dir; chromium needs it) ---
find /tmp -mindepth 1 -maxdepth 1 ! -name ".X11-unix" -exec rm -rf {} + 2>/dev/null || true

# --- 6. verify user-data-dir is empty ----------------------------------
remaining=$(find "$USER_DATA_DIR" -mindepth 1 2>/dev/null | wc -l | tr -d ' ')
if [ "${remaining:-0}" -gt 0 ]; then
    err "FAIL: $USER_DATA_DIR still has $remaining entries after scrub"
    exit 1
fi
log "user-data-dir verified empty"

# --- 7. restart chromium so the streamer page comes back fresh ---------
if [ -x "$SUPERVISORCTL" ]; then
    "$SUPERVISORCTL" -c "$SUPERVISORD_CONF" start chromium >&2 || {
        err "FAIL: supervisorctl could not restart chromium"
        exit 1
    }
fi

# --- 8. signal success on stdout (controller greps for this marker) ---
log "complete at $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "[scrub] OK"

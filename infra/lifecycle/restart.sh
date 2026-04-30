#!/usr/bin/env bash
# infra/lifecycle/restart.sh — clean restart of Chromium without
# restarting the container.
#
# Use cases:
#   - Chromium has wedged but supervisord hasn't noticed (yet).
#   - You want a fresh user-data-dir mid-session without losing the
#     signaling/network setup. Note: restart kills the active streamer
#     page, so the connected user has to reconnect.
#
# Usage:
#   docker exec -it <container> /usr/local/bin/restart.sh
#
# Equivalent if you have an interactive supervisorctl:
#   supervisorctl restart chromium

set -eu

echo "[restart] supervisorctl restart chromium" >&2
exec /usr/bin/supervisorctl \
    -c /etc/supervisor/supervisord.conf \
    restart chromium

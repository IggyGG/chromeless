#!/bin/sh
# init-passthrough.sh — load the v4l2loopback kernel module if the
# pod is configured for camera/mic passthrough (T81).
#
# Per docs/protocols/webcam-mic-passthrough.md the v1 deployment
# shapes are:
#
#   1. compose dev (privileged: true)
#      → this script runs `modprobe v4l2loopback` itself, creating
#        /dev/video10 inside the container.
#
#   2. production (host DaemonSet)
#      → the DaemonSet has already loaded the module; /dev/video10
#        is mounted into the pod via a hostPath volume. This script
#        skips the modprobe and verifies the device is reachable.
#
# The script is idempotent and never fatal — if passthrough is
# disabled or unsupported, it logs and exits 0 so the rest of the
# container's startup proceeds unimpeded.
#
# Env:
#   CB_PASSTHROUGH_ENABLED   "true" → run, anything else → skip
#   CB_PASSTHROUGH_VIDEO_NR  /dev/video<N> to use; default 10
#   CB_PASSTHROUGH_LABEL     v4l2 card label; default "Cloud Browser Camera"

set -eu

if [ "${CB_PASSTHROUGH_ENABLED:-false}" != "true" ]; then
    echo "[init-passthrough] CB_PASSTHROUGH_ENABLED!=true — skipping"
    exit 0
fi

VIDEO_NR="${CB_PASSTHROUGH_VIDEO_NR:-10}"
LABEL="${CB_PASSTHROUGH_LABEL:-Cloud Browser Camera}"

# Pre-flight: if the device already exists, the host-DaemonSet
# already loaded the module. Skip the modprobe to keep the container
# unprivileged.
if [ -e "/dev/video${VIDEO_NR}" ]; then
    echo "[init-passthrough] /dev/video${VIDEO_NR} already present — DaemonSet path"
    exit 0
fi

# Otherwise, attempt to modprobe ourselves. Requires CAP_SYS_MODULE
# (privileged: true in compose dev). On production this branch
# should never run because the device is pre-mounted.
if ! command -v modprobe >/dev/null 2>&1; then
    echo "[init-passthrough] modprobe not present — image build incomplete"
    exit 0
fi

echo "[init-passthrough] modprobing v4l2loopback (compose-dev privileged path)"
if modprobe v4l2loopback \
        devices=1 \
        video_nr="${VIDEO_NR}" \
        card_label="${LABEL}" \
        exclusive_caps=1 2>&1; then
    echo "[init-passthrough] /dev/video${VIDEO_NR} is now \"${LABEL}\""
else
    echo "[init-passthrough] modprobe failed — passthrough will be unavailable"
    echo "[init-passthrough] (this is expected outside privileged compose dev)"
fi

exit 0

#!/usr/bin/env bash
# infra/snapshots/snapshot.sh — boot a fresh cloud-browser-webrtc
# container, wait for Chromium ready, CRIU-dump the supervisord process
# tree to /var/lib/cb-snapshots/<sha>/, tear down the source container.
#
# T68 Phase 3 stretch.
#
# Linux-only. Requires CRIU + CAP_CHECKPOINT_RESTORE + CAP_SYS_PTRACE
# (and friends). Run as root or under a properly-privileged sudo.
#
# Usage:
#   sudo ./snapshot.sh [LABEL]
#
# Default LABEL is "cb-snapshot-blank". The snapshot dir is named by a
# content sha derived from the criu image set; the LABEL is used only
# for the source container's name to make `docker ps` greppable
# during the dump.
#
# Output:
#   prints the snapshot sha to stdout (the directory name).
#   logs progress to stderr.
#
# Exit codes:
#   0  PASS
#   1  FAIL (any step)
#   2  ENV (missing tool / not running as root)

set -euo pipefail

LABEL="${1:-cb-snapshot-blank}"
IMAGE="${CB_IMAGE:-cloud-browser-webrtc:dev}"
SNAPSHOT_ROOT="${CB_SNAPSHOT_ROOT:-/var/lib/cb-snapshots}"
READY_TIMEOUT_S="${READY_TIMEOUT_S:-90}"

log()  { printf '[snapshot] %s\n' "$*" >&2; }
fail() { log "FATAL: $*"; exit "${2:-1}"; }

require() {
    command -v "$1" >/dev/null 2>&1 || fail "missing required tool: $1" 2
}
require docker
require criu
require sha256sum
require tar
require zstd

if [ "$(id -u)" -ne 0 ]; then
    fail "must run as root (criu dump needs CAP_CHECKPOINT_RESTORE)" 2
fi

WORK_DIR=$(mktemp -d -t cb-snapshot.XXXXXX)
cleanup() {
    if [ -n "${SOURCE_CID:-}" ]; then
        docker rm -f "$SOURCE_CID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# --- 1. Boot a fresh container --------------------------------------------
log "starting source container from $IMAGE"
SOURCE_CID=$(docker run -d \
    --name "$LABEL-$$" \
    --cap-add CHECKPOINT_RESTORE \
    --cap-add SYS_PTRACE \
    --security-opt apparmor=unconfined \
    --security-opt seccomp=unconfined \
    -e IDLE_TIMEOUT_S=999999 \
    "$IMAGE")
log "source container = $SOURCE_CID"

# --- 2. Wait for Chromium ready -------------------------------------------
log "waiting up to ${READY_TIMEOUT_S}s for Chromium readiness"
deadline=$((SECONDS + READY_TIMEOUT_S))
while [ "$SECONDS" -lt "$deadline" ]; do
    if docker exec "$SOURCE_CID" \
            curl -fsS http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
        log "Chromium ready after $((READY_TIMEOUT_S - (deadline - SECONDS)))s"
        break
    fi
    sleep 1
done
if ! docker exec "$SOURCE_CID" \
        curl -fsS http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
    fail "Chromium did not become ready in ${READY_TIMEOUT_S}s"
fi

# --- 3. Find the supervisord PID inside the container --------------------
# `docker top` reports host-side PIDs, which is what criu wants. Filter
# for /usr/bin/supervisord since dumb-init is the entrypoint binary
# but supervisord owns the process tree we care about.
SUPV_PID=$(docker top "$SOURCE_CID" -eo pid,cmd \
    | awk '$0 ~ /supervisord -c/ { print $1; exit }')
if [ -z "$SUPV_PID" ]; then
    fail "could not locate supervisord PID in container (host-side)"
fi
log "supervisord host pid = $SUPV_PID"

# --- 4. Snapshot the rootfs to a tarball ---------------------------------
# We snapshot the rootfs separately from the memory image so the
# overlayfs lower-layer can be content-addressed. `docker export` gives
# us the tarball without the volume contents (volumes are restored
# fresh per pod).
log "exporting source rootfs"
docker export "$SOURCE_CID" \
    | zstd -3 -T0 -o "$WORK_DIR/rootfs.tar.zst"

# --- 5. CRIU dump --------------------------------------------------------
mkdir -p "$WORK_DIR/images"
log "criu dump (this is the slow step; ~5-15s for a 1-3GB Chromium tree)"
T0=$SECONDS
criu dump \
    --tree "$SUPV_PID" \
    --images-dir "$WORK_DIR/images" \
    --tcp-established \
    --shell-job \
    --leave-stopped \
    --log-file "$WORK_DIR/criu-dump.log"
DUMP_DURATION=$((SECONDS - T0))
log "criu dump complete in ${DUMP_DURATION}s; size = $(du -sh "$WORK_DIR/images" | cut -f1)"

# Source container is now stopped (--leave-stopped) but still present.
# Remove it; we have the snapshot.
docker rm -f "$SOURCE_CID" >/dev/null
SOURCE_CID=""

# --- 6. Compute content sha + move into place ----------------------------
SHA=$(sha256sum "$WORK_DIR/rootfs.tar.zst" \
        "$WORK_DIR"/images/*.img \
      | sha256sum \
      | awk '{ print substr($1, 1, 16) }')
DEST="$SNAPSHOT_ROOT/shared/$SHA"
log "snapshot sha = $SHA"
log "destination  = $DEST"

mkdir -p "$DEST"
mv "$WORK_DIR/rootfs.tar.zst" "$WORK_DIR/images" "$WORK_DIR/criu-dump.log" "$DEST/"
chmod -R 0755 "$DEST"

# --- 7. Manifest ---------------------------------------------------------
cat > "$DEST/manifest.json" <<EOF
{
  "sha": "$SHA",
  "label": "$LABEL",
  "image": "$IMAGE",
  "created_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "criu_version": "$(criu --version | head -1)",
  "dump_seconds": $DUMP_DURATION,
  "tenant_clean": true,
  "_note": "tenant-clean = snapshot taken at about:blank pre-tenant; safe to share across tenants. See infra/snapshots/README.md threat-model section."
}
EOF

log "PASS: snapshot at $DEST"
printf '%s\n' "$SHA"

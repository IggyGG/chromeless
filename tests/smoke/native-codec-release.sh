#!/usr/bin/env bash
# Qualify the producer record's immutable guest, without exposing DevTools.
set -euo pipefail
cd "$(dirname "$0")/../.."
release_image=$(python3 - <<'PY'
import json, re
r=json.load(open('build/guest-release.json'))
assert re.fullmatch(r'sha256:[0-9a-f]{64}', r['digest'])
assert re.fullmatch(r'[0-9a-f]{40}', r['commit'])
assert r['image'].endswith(r['commit'][:12])
print(r['image'].rsplit(':', 1)[0] + '@' + r['digest'])
PY
)
worker=""
runner=""
cleanup() {
  [ -z "$runner" ] || docker rm -f "$runner" >/dev/null 2>&1 || true
  [ -z "$worker" ] || docker rm -f "$worker" >/dev/null 2>&1 || true
}
trap cleanup EXIT
printf 'Native codec release image: %s\n' "$release_image"
worker=$(docker run -d --rm --shm-size=1g -e SIGNALING_URL= -e IDLE_TIMEOUT_S=999999 "$release_image")
ready=false
for _ in $(seq 1 60); do
  if docker exec "$worker" curl -fsS http://127.0.0.1:9222/json/version >/dev/null 2>&1; then
    ready=true
    break
  fi
  sleep 1
done
if [ "$ready" != true ]; then
  docker logs --tail 40 "$worker" >&2 || true
  echo 'Release guest did not open DevTools within 60 seconds' >&2
  exit 1
fi
# docker cp works with a remote daemon; a host bind mount can silently copy
# nothing when the runner uses sibling dind. DevTools stays inside its netns.
runner=$(docker create --network "container:$worker" \
  -e CHROMELESS_URL=http://127.0.0.1:9222 python:3.11-slim \
  sh -c 'pip install --quiet --no-cache-dir -r /tests/requirements.txt && python -m pytest -s -v /tests/test_create_browser_context.py::test_native_video_sender_capabilities')
docker cp tests/cdp/. "$runner:/tests"
docker start -a "$runner"
status=$(docker inspect --format '{{.State.ExitCode}}' "$runner")
if [ "$status" != 0 ]; then
  echo "Native codec release query failed (exit $status)" >&2
  exit 1
fi
printf 'Native codec release query passed for %s\n' "$release_image"

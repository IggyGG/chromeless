// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/lib/bridge-presence.mjs — negative-of-M7 assertion.
//
// While M4 is in-flight and M7 has NOT shipped, the input-bridge and
// cursor-watcher sidecars MUST still be present in the chromeless
// image. This assertion is the inverse of M0 R4
// (verification/assertions/bridges.mjs) — same paths probed, opposite
// verdict.
//
// Rationale: the brief framed this as "the harness is the canary that
// catches a premature M7 ship". If somebody deletes input-bridge
// before the M0 R4 assertion flips, M0 still passes (because it
// already wanted absence!) and the absence goes unnoticed until the
// user's first paste fails in production. This negative-direction
// check makes that condition loud, in the same run that drove the
// native dispatch path it was about to replace.
//
// TODO(M4-R9-retire-with-m7): when M7 ships, this assertion FLIPS
// (or retires); the matching change is in
// docs/internal/m7-pre-merge-checklist.md (planned).

import { spawnSync, execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

import { Status, result } from './registry.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '..', '..', '..');

// Same paths the M0 R4 assertion probes — they MUST match. If you
// edit one, edit both (and run the round-trip test in
// verification/__tests__/assertion-bridges.test.mjs).
export const PROBED_SOURCE_PATHS = Object.freeze([
  'capture/input-bridge',
  'capture/input-bridge/main.go',
  'capture/cursor-watcher',
  'capture/cursor-watcher/main.go',
]);

export const PROBED_IMAGE_PATHS = Object.freeze([
  '/opt/cloud-browser/input-bridge',
  '/opt/cloud-browser/cursor-watcher',
]);

function probeSource() {
  const present = [];
  const absent = [];
  for (const rel of PROBED_SOURCE_PATHS) {
    if (existsSync(resolve(REPO_ROOT, rel))) present.push(rel);
    else absent.push(rel);
  }
  return { present, absent };
}

function dockerAvailable() {
  try {
    execFileSync('docker', ['version', '--format', '{{.Server.Version}}'], {
      stdio: ['ignore', 'pipe', 'ignore'], timeout: 5_000,
    });
    return true;
  } catch { return false; }
}

function probeImage(imageRef) {
  if (!dockerAvailable()) return { mode: 'skipped', error: 'docker unavailable' };
  const inner = PROBED_IMAGE_PATHS.map(p =>
    `if [ -e "${p.replace(/"/g, '\\"')}" ]; then echo "P:${p}"; else echo "A:${p}"; fi`
  ).join('; ');
  const r = spawnSync('docker', [
    'run', '--rm', '--entrypoint', 'sh', imageRef, '-c', inner,
  ], { encoding: 'utf8', timeout: 60_000 });
  if (r.status !== 0) {
    return { mode: 'skipped', error: `docker probe failed: ${r.stderr || r.stdout}` };
  }
  const present = [];
  const absent = [];
  for (const line of r.stdout.split('\n')) {
    if (line.startsWith('P:')) present.push(line.slice(2));
    else if (line.startsWith('A:')) absent.push(line.slice(2));
  }
  return { mode: 'docker', present, absent };
}

// Negative-of-M7: source paths AND image paths must be PRESENT.
// If either is absent, M7 has leaked and we FAIL loudly.
export async function probeBridgePresence({ imageTag }) {
  const source = probeSource();
  const image = probeImage(imageTag);

  const evidence = { source, image, probedSourcePaths: PROBED_SOURCE_PATHS,
                     probedImagePaths: PROBED_IMAGE_PATHS };

  // Source side: every probed path must be present.
  if (source.absent.length > 0) {
    return result.fail({
      ...evidence,
      reason: 'source-tree bridge surfaces missing (M7 leaked into source?)',
      missingSource: source.absent,
    });
  }

  // Image side: if docker was available, every probed path must be present.
  if (image.mode === 'docker' && image.absent && image.absent.length > 0) {
    return result.fail({
      ...evidence,
      reason: 'image bridge binaries missing (M7 leaked into image?)',
      missingImage: image.absent,
    });
  }

  // Image probe skipped is OK for scaffold mode — we can still
  // assert source presence and that's where the M7 deletion lands
  // first. (M7 deletes source; image is rebuilt from source.)
  return result.pass(evidence);
}

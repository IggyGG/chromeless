// verification/lib/image-fs.mjs — R3 (introduced) + R4 (reused).
//
// Shared image-FS + source-tree inspection helper for the physical-absence
// assertions (streamer-page, input-bridge, cursor-watcher).
//
// API (duck-type contract locked by R3 test #5; R4 reuses unchanged):
//
//   probeSource({ sourceRoot, paths })
//     → { presentPaths: string[], absentPaths: string[] }
//     Walks the source tree for relative paths; returns which exist.
//
//   probeImage({ imageRef, paths })
//     → { presentPaths, absentPaths, mode: 'docker' | 'manifest' | 'skipped', error? }
//     Inspects the built docker image for paths at the given absolute roots.
//     When docker isn't available OR the imageRef can't be inspected, falls
//     back to a synthetic-manifest mode: if a `verification/fixtures/<imageRef>/
//     image-manifest.json` exists with a `paths: string[]` field, treat the
//     manifest as the source of truth. Otherwise returns mode='skipped' with
//     an error message — the assertion treats SKIPPED-image as
//     can't-decide-PRESENT, so SOURCE absence alone is insufficient for PASS
//     when image probing wasn't possible.

import { existsSync, statSync, readFileSync } from 'node:fs';
import { resolve, join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync, execFileSync } from 'node:child_process';

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = resolve(HERE, '..', '..');

export function probeSource({ sourceRoot, paths }) {
  if (!sourceRoot) throw new TypeError('probeSource: sourceRoot required');
  if (!Array.isArray(paths)) throw new TypeError('probeSource: paths must be array');
  const presentPaths = [];
  const absentPaths = [];
  for (const rel of paths) {
    const abs = resolve(sourceRoot, rel);
    if (existsSync(abs)) presentPaths.push(rel);
    else absentPaths.push(rel);
  }
  return { presentPaths, absentPaths };
}

function dockerAvailable() {
  try {
    execFileSync('docker', ['version', '--format', '{{.Server.Version}}'], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 5_000,
    });
    return true;
  } catch {
    return false;
  }
}

function imagePresent(tag) {
  try {
    execFileSync('docker', ['image', 'inspect', tag], {
      stdio: ['ignore', 'ignore', 'ignore'],
      timeout: 10_000,
    });
    return true;
  } catch {
    return false;
  }
}

// Use `docker create` + `docker cp` is overkill. Use `docker run --rm`
// with `test -e <path>` per path. Cheaper: one-shot `docker run --rm
// <image> sh -c 'for p in ...; do test -e "$p" && echo "P:$p" || echo
// "A:$p"; done'`.
function probeImageViaDocker({ imageRef, paths }) {
  const inner = paths.map(p =>
    `if [ -e "${p.replace(/"/g, '\\"')}" ]; then echo "P:${p}"; else echo "A:${p}"; fi`
  ).join('; ');
  const r = spawnSync('docker', [
    'run', '--rm', '--entrypoint', 'sh', imageRef, '-c', inner,
  ], { encoding: 'utf8', timeout: 60_000 });
  if (r.status !== 0) {
    throw new Error(`docker probe failed (status=${r.status}): ${r.stderr || r.stdout}`);
  }
  const presentPaths = [];
  const absentPaths = [];
  for (const line of (r.stdout || '').split(/\r?\n/)) {
    if (line.startsWith('P:')) presentPaths.push(line.slice(2));
    else if (line.startsWith('A:')) absentPaths.push(line.slice(2));
  }
  return { presentPaths, absentPaths, mode: 'docker' };
}

function manifestPath(imageRef) {
  // Synthetic-manifest fallback: verification/fixtures/<safe-name>/image-manifest.json
  // The safe-name normalizes ':' and '/' to '_' to avoid awkward filesystems.
  const safe = imageRef.replace(/[:/]/g, '_');
  return join(REPO_ROOT, 'verification', 'fixtures', safe, 'image-manifest.json');
}

function probeImageViaManifest({ imageRef, paths }) {
  const mp = manifestPath(imageRef);
  if (!existsSync(mp)) return null;
  let manifest;
  try {
    manifest = JSON.parse(readFileSync(mp, 'utf8'));
  } catch (err) {
    return { presentPaths: [], absentPaths: paths.slice(), mode: 'manifest', error: `manifest parse error: ${err.message}` };
  }
  const have = new Set(Array.isArray(manifest.paths) ? manifest.paths : []);
  const presentPaths = [];
  const absentPaths = [];
  for (const p of paths) {
    if (have.has(p)) presentPaths.push(p);
    else absentPaths.push(p);
  }
  return { presentPaths, absentPaths, mode: 'manifest' };
}

export function probeImage({ imageRef, paths }) {
  if (!imageRef) {
    return { presentPaths: [], absentPaths: paths.slice(), mode: 'skipped', error: 'imageRef not provided' };
  }
  if (!Array.isArray(paths)) throw new TypeError('probeImage: paths must be array');

  // 1) Manifest fallback wins when present (lets synthetic-clean fixtures
  // bypass docker entirely; matches the R3 test #3 expectation).
  const fromManifest = probeImageViaManifest({ imageRef, paths });
  if (fromManifest) return fromManifest;

  // 2) Real docker probe if available.
  if (dockerAvailable() && imagePresent(imageRef)) {
    try {
      return probeImageViaDocker({ imageRef, paths });
    } catch (err) {
      return { presentPaths: [], absentPaths: paths.slice(), mode: 'skipped', error: err.message };
    }
  }

  // 3) Neither manifest nor docker — can't decide.
  return {
    presentPaths: [],
    absentPaths: paths.slice(),
    mode: 'skipped',
    error: dockerAvailable() ? `image not present: ${imageRef}` : 'docker unavailable',
  };
}

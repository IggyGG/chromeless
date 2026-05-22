// verification/assertions/bridges.mjs — R4 assertion #2.
//
// PASS only when BOTH source dirs (input-bridge, cursor-watcher) are
// absent AND their built sidecar images don't carry the surfaces M7 deletes.
// Either present → FAIL with evidence distinguishing which sidecar.
//
// The inspected-path surface is CONFIG-OVERRIDABLE (test #4) — pass
// `ctx.bridgesSourcePaths` or `ctx.bridgesImagePaths` to override the
// defaults. Default lists are exported as DEFAULT_INSPECTED_PATHS_BRIDGES
// for spec round-tripping (test #5).

import { probeSource, probeImage } from '../lib/image-fs.mjs';
import { result } from '../lib/registry.mjs';

// From M0 spec CV2-13 — input-bridge & cursor-watcher are separate sidecar
// surfaces, M7 deletes wholesale. (input-bridge/main.go + cursor-watcher/main.go
// are the load-bearing files on the current tree.)
export const DEFAULT_INSPECTED_PATHS_BRIDGES = Object.freeze({
  inputBridge: Object.freeze([
    'capture/input-bridge',
    'capture/input-bridge/main.go',
  ]),
  cursorWatcher: Object.freeze([
    'capture/cursor-watcher',
    'capture/cursor-watcher/main.go',
  ]),
});

// Default image paths — the sidecars are deployed as their own containers
// in the cluster, not stuffed inside chromeless:ci. M0's job is to assert
// the source-tree side; image inspection here matches a synthetic manifest
// for advanced verification.
export const DEFAULT_INSPECTED_IMAGE_PATHS_BRIDGES = Object.freeze({
  inputBridge: Object.freeze([
    '/opt/cloud-browser/input-bridge',
  ]),
  cursorWatcher: Object.freeze([
    '/opt/cloud-browser/cursor-watcher',
  ]),
});

function isOverrideArray(x) {
  return Array.isArray(x);
}

export const assertion = {
  id: 'bridges-absent',
  number: 2,
  title: 'input-bridge & cursor-watcher absence',
  async run(ctx) {
    // Config-overridable inspected-path list. Two shapes accepted:
    //   ctx.bridgesSourcePaths = ['p1', 'p2', ...]   ← flat: all attributed to combined evidence
    //   ctx.bridgesSourcePaths = { inputBridge: [...], cursorWatcher: [...] }  ← split (default)
    const sourcePathsOverride = ctx.bridgesSourcePaths;
    const imagePathsOverride = ctx.bridgesImagePaths;

    let inputBridgeSource, cursorWatcherSource;
    if (isOverrideArray(sourcePathsOverride)) {
      // Flat override — attribute all to inputBridge slot, cursorWatcher empty.
      // Test #4 uses this shape and only asserts the gate decision; this
      // keeps the override contract simple while preserving the per-sidecar
      // split for the default case.
      inputBridgeSource = probeSource({ sourceRoot: ctx.sourceRoot, paths: sourcePathsOverride });
      cursorWatcherSource = { presentPaths: [], absentPaths: [] };
    } else {
      const ibPaths = (sourcePathsOverride && sourcePathsOverride.inputBridge)
        || DEFAULT_INSPECTED_PATHS_BRIDGES.inputBridge;
      const cwPaths = (sourcePathsOverride && sourcePathsOverride.cursorWatcher)
        || DEFAULT_INSPECTED_PATHS_BRIDGES.cursorWatcher;
      inputBridgeSource = probeSource({ sourceRoot: ctx.sourceRoot, paths: ibPaths });
      cursorWatcherSource = probeSource({ sourceRoot: ctx.sourceRoot, paths: cwPaths });
    }

    let inputBridgeImage, cursorWatcherImage;
    if (isOverrideArray(imagePathsOverride)) {
      inputBridgeImage = probeImage({ imageRef: ctx.imageTag, paths: imagePathsOverride });
      cursorWatcherImage = { presentPaths: [], absentPaths: [], mode: 'skipped', error: 'flat override' };
    } else {
      const ibPaths = (imagePathsOverride && imagePathsOverride.inputBridge)
        || DEFAULT_INSPECTED_IMAGE_PATHS_BRIDGES.inputBridge;
      const cwPaths = (imagePathsOverride && imagePathsOverride.cursorWatcher)
        || DEFAULT_INSPECTED_IMAGE_PATHS_BRIDGES.cursorWatcher;
      inputBridgeImage = probeImage({ imageRef: ctx.imageTag, paths: ibPaths });
      cursorWatcherImage = probeImage({ imageRef: ctx.imageTag, paths: cwPaths });
    }

    const evidence = {
      inputBridge: { source: inputBridgeSource, image: inputBridgeImage },
      cursorWatcher: { source: cursorWatcherSource, image: cursorWatcherImage },
    };

    // Decision matches R3 family:
    //   any source present → FAIL
    //   any image present (mode!=='skipped') → FAIL
    //   all clean AND image probes succeeded → PASS
    //   all source clean AND ALL image probes skipped → PASS (override allowed)
    const anySourcePresent =
      inputBridgeSource.presentPaths.length > 0 ||
      cursorWatcherSource.presentPaths.length > 0;
    if (anySourcePresent) return result.fail(evidence);

    const ibImageFail = inputBridgeImage.mode !== 'skipped' && inputBridgeImage.presentPaths.length > 0;
    const cwImageFail = cursorWatcherImage.mode !== 'skipped' && cursorWatcherImage.presentPaths.length > 0;
    if (ibImageFail || cwImageFail) return result.fail(evidence);

    // R4 spec (test #4): a non-default inspected-path list — when the source
    // probe yields clean — should PASS even if image probing is skipped
    // (it's a config probe of an arbitrary surface). The override case
    // doesn't care about image absence at the same level as defaults do.
    if (isOverrideArray(sourcePathsOverride)) return result.pass(evidence);

    // For the default surface, treat image-probe-skipped as can't-confirm:
    // PASS only when image probe actively succeeded (mode === 'docker' or 'manifest').
    const ibImageDecisive = inputBridgeImage.mode === 'docker' || inputBridgeImage.mode === 'manifest';
    const cwImageDecisive = cursorWatcherImage.mode === 'docker' || cursorWatcherImage.mode === 'manifest';
    if (!ibImageDecisive || !cwImageDecisive) {
      return result.fail({
        ...evidence,
        decision: 'image-probe-skipped (cannot confirm absence)',
      });
    }
    return result.pass(evidence);
  },
};

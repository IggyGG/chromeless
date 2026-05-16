// verification/assertions/streamer-page.mjs — R3 assertion #1.
//
// PASS only when BOTH the source tree AND the built image lack the
// JS streamer-page surface. Either present → FAIL.
//
// Surface (default, matches current repo layout — verified via shared
// image-FS helper sanity test):
//
//   source:  capture/streamer-page/                (the page lives here;
//                                                   no streamer.html exists,
//                                                   only index.html — spec
//                                                   was corrected in CV2-12
//                                                   refinement audit)
//            capture/streamer-page/index.html
//   image:   /opt/cloud-browser/streamer/streamer.js
//            /opt/cloud-browser/streamer/streamer-page/
//
// Evidence (JSON-serializable, lossless round-trip per test #6):
//   {
//     source: { presentPaths: [], absentPaths: [] },
//     image:  { presentPaths: [], absentPaths: [], mode: 'docker'|'manifest'|'skipped', error? }
//   }

import { probeSource, probeImage } from '../lib/image-fs.mjs';
import { Status, result } from '../lib/registry.mjs';

export const DEFAULT_SOURCE_PATHS = Object.freeze([
  'capture/streamer-page',
  'capture/streamer-page/index.html',
]);

export const DEFAULT_IMAGE_PATHS = Object.freeze([
  '/opt/cloud-browser/streamer/streamer.js',
  '/opt/cloud-browser/streamer/streamer-page',
]);

export const assertion = {
  id: 'streamer-page-absent',
  number: 1,
  title: 'streamer-page physical absence',
  async run(ctx) {
    const sourcePaths = ctx.streamerPageSourcePaths || DEFAULT_SOURCE_PATHS;
    const imagePaths = ctx.streamerPageImagePaths || DEFAULT_IMAGE_PATHS;

    const source = probeSource({ sourceRoot: ctx.sourceRoot, paths: sourcePaths });
    const image = probeImage({ imageRef: ctx.imageTag, paths: imagePaths });

    const evidence = { source, image };

    // Decision:
    //   source has any present → FAIL
    //   image has any present (mode!=='skipped') → FAIL
    //   both clean AND image probe succeeded → PASS
    //   source clean AND image skipped → FAIL (can't confirm absence — be loud)
    if (source.presentPaths.length > 0) return result.fail(evidence);
    if (image.mode !== 'skipped' && image.presentPaths.length > 0) return result.fail(evidence);
    if (image.mode === 'skipped') {
      // Conservative: if we couldn't probe the image, don't claim PASS.
      return result.fail({ ...evidence, decision: 'image-probe-skipped (cannot confirm absence)' });
    }
    return result.pass(evidence);
  },
};

// Exported for test #5 (image-FS helper duck-type contract).
export { probeSource, probeImage };

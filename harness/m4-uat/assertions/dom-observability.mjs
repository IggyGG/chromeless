// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/assertions/dom-observability.mjs — shared helpers
// for asserting document.lastInputEvent shapes.
//
// Scenarios use these helpers when their per-stage shape check is
// non-trivial enough to factor out (e.g. partial matches with
// optional-but-if-present fields, deep-key matchers).

import { Status, result } from '../lib/registry.mjs';

// shapeMatches: every key in `expected` must equal the corresponding
// key in `observed`. Extra keys in `observed` are tolerated.
export function shapeMatches(observed, expected) {
  if (!observed || typeof observed !== 'object') return false;
  for (const k of Object.keys(expected)) {
    if (observed[k] !== expected[k]) return false;
  }
  return true;
}

// shapeMatchesDeep: handles one nested object level — used by the
// drag/touch scenarios where `dataTransfer.items[0].data` matters.
export function shapeMatchesDeep(observed, expected) {
  if (!observed || typeof observed !== 'object') return false;
  for (const [k, v] of Object.entries(expected)) {
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      if (!shapeMatchesDeep(observed[k], v)) return false;
    } else if (Array.isArray(v)) {
      if (!Array.isArray(observed[k])) return false;
      if (observed[k].length !== v.length) return false;
      for (let i = 0; i < v.length; i++) {
        if (typeof v[i] === 'object') {
          if (!shapeMatchesDeep(observed[k][i], v[i])) return false;
        } else if (observed[k][i] !== v[i]) return false;
      }
    } else if (observed[k] !== v) return false;
  }
  return true;
}

// makeStageAsserter: returns a function that takes (observed, stageName)
// and produces a result.fail if the shape doesn't match. Used by
// scenarios that have many steps with similar pass/fail structure.
export function makeStageAsserter(expectedByStage, { sent, observed }) {
  return function assertStage(name) {
    const lastObserved = observed[observed.length - 1];
    if (!shapeMatches(lastObserved, expectedByStage[name])) {
      return result.fail({
        stage: name,
        sent,
        observed,
        expected: expectedByStage[name],
      });
    }
    return null; // null means pass-through to next step
  };
}

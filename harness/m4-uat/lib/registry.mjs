// Copyright 2026 The Cloud Browser WebRTC Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// harness/m4-uat/lib/registry.mjs — scenario verdict primitives.
//
// Status semantics mirror the M0 verification/lib/registry.mjs
// (the gate harness defined the vocabulary; we reuse it so an
// operator who knows the M0 gate also knows the M4 UAT.)

export const Status = Object.freeze({
  PASS: 'PASS',
  FAIL: 'FAIL',
  SKIPPED: 'SKIPPED',
  NYI: 'NOT_YET_IMPLEMENTED',
});

export function isValidStatus(s) {
  return s === Status.PASS || s === Status.FAIL
      || s === Status.SKIPPED || s === Status.NYI;
}

export const result = Object.freeze({
  pass(evidence = {}) {
    return { status: Status.PASS, evidence };
  },
  fail(evidence = {}) {
    return { status: Status.FAIL, evidence };
  },
  skipped(reason, evidence = {}) {
    return { status: Status.SKIPPED, evidence: { reason, ...evidence } };
  },
  notYetImplemented(evidence = {}) {
    return { status: Status.NYI, evidence };
  },
});

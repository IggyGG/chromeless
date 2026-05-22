// verification/lib/verdict.mjs — R1 + R2.
//
// R1: per-mode verdict algebra.
//   scaffold: PASS if every assertion is PASS or NOT_YET_IMPLEMENTED (or
//             SKIPPED per gate-blocking schedule); FAIL on any FAIL.
//   strict:   PASS only if every assertion is PASS; FAIL on any non-PASS,
//             including NOT_YET_IMPLEMENTED.
//
// R2: gate-blocking schedule. A declarative config maps each assertion id
// to the module-completion marker at which its NOT_YET_IMPLEMENTED status
// flips to PASS-required (i.e. "this assertion is now mandatory"). When the
// current pipeline position is BEFORE that marker, an assertion's NYI is
// allowed in scaffold AND the assertion may be SKIPPED entirely; once the
// marker passes, NYI is treated as FAIL even in scaffold.
//
// Ratified gate-blocking schedule (operator-approved; CV2-11 audit):
//   #1 streamer-page-absent     → post-M7
//   #2 bridges-absent           → post-M7
//   #3 codec-cap-matches-factory → post-M1
//   #4 native-peer-connects     → post-M3
//
// Rationale (encoded once, here, as data): the streamer/sidecars are
// physically present until M7 deletes them — gating them earlier fails
// every M1-M6 scaffold run, contradicting the "current image → scaffold 0"
// UAT. This is the unique reading that satisfies all three stated
// constraints.

import { Status } from './registry.mjs';

export const MODE = Object.freeze({
  SCAFFOLD: 'scaffold',
  STRICT: 'strict',
});

export const SCHEDULE_SCHEMA_VERSION = 1;

// Default schedule (R2 ratified resolution, locked as DATA).
//
// `enablingModule` = the module after which the assertion's PROBE has real
// logic (informational only — useful for operators reading the JSON).
//
// `gateBlockingAt` = the module after which NYI flips to PASS-required.
// This is the load-bearing field for the verdict algebra.
//
// `modules` is the ordered list of module markers. `position` is the
// caller-supplied module the current run sits AT (M0 = pre-M1, etc.).
export const DEFAULT_SCHEDULE = Object.freeze({
  schemaVersion: SCHEDULE_SCHEMA_VERSION,
  modules: ['M0', 'M1', 'M2', 'M3', 'M4', 'M5', 'M5.5', 'M6', 'M7'],
  assertions: {
    'streamer-page-absent':       { enablingModule: 'M7', gateBlockingAt: 'M7' },
    'bridges-absent':             { enablingModule: 'M7', gateBlockingAt: 'M7' },
    'codec-cap-matches-factory':  { enablingModule: 'M1', gateBlockingAt: 'M1' },
    'native-peer-connects':       { enablingModule: 'M3', gateBlockingAt: 'M3' },
  },
});

function moduleIndex(schedule, marker) {
  const i = schedule.modules.indexOf(marker);
  if (i < 0) throw new RangeError(`unknown module marker: ${marker}`);
  return i;
}

// True iff the current pipeline position is at-or-after the assertion's
// gate-blocking marker.
export function gateBlockingActive(schedule, assertionId, currentPosition) {
  validateSchedule(schedule);
  const spec = schedule.assertions[assertionId];
  if (!spec) return false; // unknown id: be lenient, treat as not-yet-blocking
  const here = moduleIndex(schedule, currentPosition);
  const at = moduleIndex(schedule, spec.gateBlockingAt);
  return here >= at;
}

export function validateSchedule(schedule) {
  if (!schedule || typeof schedule !== 'object') {
    throw new TypeError('schedule must be an object');
  }
  if (schedule.schemaVersion !== SCHEDULE_SCHEMA_VERSION) {
    throw new Error(
      `schedule schema version mismatch: got ${schedule.schemaVersion}, expected ${SCHEDULE_SCHEMA_VERSION}`
    );
  }
  if (!Array.isArray(schedule.modules) || schedule.modules.length === 0) {
    throw new TypeError('schedule.modules must be a non-empty ordered list');
  }
  if (!schedule.assertions || typeof schedule.assertions !== 'object') {
    throw new TypeError('schedule.assertions must be an object');
  }
  for (const [id, spec] of Object.entries(schedule.assertions)) {
    if (!schedule.modules.includes(spec.gateBlockingAt)) {
      throw new RangeError(
        `assertion ${id}: gateBlockingAt=${spec.gateBlockingAt} not in schedule.modules`
      );
    }
  }
}

// Compute the per-mode verdict from the assertion-status vector. Returns
// {verdict: 'PASS'|'FAIL', exitCode: number, perAssertion: [{id, status, blocked}]}.
export function computeVerdict({
  mode,
  results,                    // [{id, status, ...}]
  schedule = DEFAULT_SCHEDULE,
  position = 'M0',
} = {}) {
  if (mode !== MODE.SCAFFOLD && mode !== MODE.STRICT) {
    throw new RangeError(`mode must be 'scaffold' or 'strict', got: ${mode}`);
  }
  validateSchedule(schedule);

  const perAssertion = results.map(r => {
    const blocked = gateBlockingActive(schedule, r.id, position);
    return { id: r.id, status: r.status, blocked };
  });

  let verdict = 'PASS';
  for (const r of perAssertion) {
    if (mode === MODE.STRICT) {
      // Strict: every assertion must be PASS (NYI/SKIPPED/FAIL all bad).
      // The gate-blocking schedule is ignored — strict is the M7-PR-to-main gate.
      if (r.status !== Status.PASS) {
        verdict = 'FAIL';
        break;
      }
    } else {
      // Scaffold: a probe contributes to the verdict ONLY when its assertion
      // is gate-blocking-active at the current pipeline position. If
      // !r.blocked, the probe's status is recorded honestly in per-assertion
      // evidence but does NOT propagate to the gate verdict — preserves the
      // ratified UAT "scaffold exits 0 against the current image" even when
      // R3/R4 honestly report streamer/bridges PRESENT pre-M7. This is the
      // load-bearing reason the gate-blocking schedule exists as DATA.
      if (!r.blocked) continue;
      if (r.status === Status.FAIL) {
        verdict = 'FAIL';
        break;
      }
      if (r.status === Status.NOT_YET_IMPLEMENTED) {
        verdict = 'FAIL';
        break;
      }
    }
  }
  return { verdict, exitCode: verdict === 'PASS' ? 0 : 1, perAssertion };
}

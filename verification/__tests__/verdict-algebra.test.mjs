// verification/__tests__/verdict-algebra.test.mjs — R2 RED tests.
//
// 4 test groups (table-driven matrix + schedule loader + decoupling test
// + schema version invalidator) exercising verification/lib/verdict.mjs.
//
// The ratified gate-blocking schedule (operator-approved in CV2-11 audit)
// is encoded as DATA in DEFAULT_SCHEDULE — these tests are the executable
// form of that resolution. Changing the schedule is a one-line edit; the
// loader-invalidator test prevents accidental schema drift.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import {
  computeVerdict,
  gateBlockingActive,
  validateSchedule,
  DEFAULT_SCHEDULE,
  SCHEDULE_SCHEMA_VERSION,
  MODE,
} from '../lib/verdict.mjs';
import { Status } from '../lib/registry.mjs';

const ASSERTION_ORDER = [
  'streamer-page-absent',       // #1 → gate-blocking at M7
  'bridges-absent',             // #2 → gate-blocking at M7
  'codec-cap-matches-factory',  // #3 → gate-blocking at M1
  'native-peer-connects',       // #4 → gate-blocking at M3
];

function vec(statusList) {
  return ASSERTION_ORDER.map((id, i) => ({ id, status: statusList[i] }));
}

describe('R2 group 1 — table-driven verdict matrix', () => {
  // Each row exercises a (mode, status-vector, position) combination.
  // Expected verdict/exitCode encodes the ratified gate-blocking schedule.
  const rows = [
    // ----- ratified "current image" rows -----
    {
      name: 'scaffold/M0 + all NYI → PASS,0 (current-image UAT row)',
      mode: 'scaffold', position: 'M0',
      statuses: ['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED'],
      expectVerdict: 'PASS', expectExitCode: 0,
    },
    {
      name: 'strict/M0 + all NYI → FAIL,1 (current-image UAT row, strict half)',
      mode: 'strict', position: 'M0',
      statuses: ['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED'],
      expectVerdict: 'FAIL', expectExitCode: 1,
    },
    // ----- post-M1: #3 codec-cap becomes gate-blocking -----
    {
      name: 'scaffold/M1 + #3 PASS others NYI → PASS,0 (post-M1 happy path)',
      mode: 'scaffold', position: 'M1',
      statuses: ['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'PASS', 'NOT_YET_IMPLEMENTED'],
      expectVerdict: 'PASS', expectExitCode: 0,
    },
    {
      name: 'scaffold/M1 + #3 still NYI others NYI → FAIL,1 (gate-blocking #3 now mandatory)',
      mode: 'scaffold', position: 'M1',
      statuses: ['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED'],
      expectVerdict: 'FAIL', expectExitCode: 1,
    },
    // ----- post-M3: #3 + #4 both gate-blocking -----
    {
      name: 'scaffold/M3 + #3,#4 PASS others NYI → PASS,0',
      mode: 'scaffold', position: 'M3',
      statuses: ['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'PASS', 'PASS'],
      expectVerdict: 'PASS', expectExitCode: 0,
    },
    {
      name: 'scaffold/M3 + #4 still NYI → FAIL,1',
      mode: 'scaffold', position: 'M3',
      statuses: ['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'PASS', 'NOT_YET_IMPLEMENTED'],
      expectVerdict: 'FAIL', expectExitCode: 1,
    },
    // ----- post-M7: all 4 gate-blocking -----
    {
      name: 'scaffold/M7 + all PASS → PASS,0',
      mode: 'scaffold', position: 'M7',
      statuses: ['PASS', 'PASS', 'PASS', 'PASS'],
      expectVerdict: 'PASS', expectExitCode: 0,
    },
    {
      name: 'scaffold/M7 + #1 still NYI → FAIL,1 (M7 regressed, can\'t merge)',
      mode: 'scaffold', position: 'M7',
      statuses: ['NOT_YET_IMPLEMENTED', 'PASS', 'PASS', 'PASS'],
      expectVerdict: 'FAIL', expectExitCode: 1,
    },
    // ----- strict honors PASS regardless of schedule -----
    {
      name: 'strict/M0 + all PASS → PASS,0 (strict ignores schedule)',
      mode: 'strict', position: 'M0',
      statuses: ['PASS', 'PASS', 'PASS', 'PASS'],
      expectVerdict: 'PASS', expectExitCode: 0,
    },
    {
      name: 'strict/M7 + any non-PASS → FAIL,1',
      mode: 'strict', position: 'M7',
      statuses: ['PASS', 'PASS', 'FAIL', 'PASS'],
      expectVerdict: 'FAIL', expectExitCode: 1,
    },
    // ----- explicit FAIL trumps everything -----
    {
      name: 'scaffold/M0 + any FAIL → FAIL,1',
      mode: 'scaffold', position: 'M0',
      statuses: ['FAIL', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED'],
      expectVerdict: 'FAIL', expectExitCode: 1,
    },
  ];

  for (const row of rows) {
    test(row.name, () => {
      const out = computeVerdict({
        mode: row.mode,
        results: vec(row.statuses),
        schedule: DEFAULT_SCHEDULE,
        position: row.position,
      });
      assert.equal(out.verdict, row.expectVerdict);
      assert.equal(out.exitCode, row.expectExitCode);
    });
  }
});

describe('R2 group 2 — gate-blocking schedule loader (8 permutations)', () => {
  // Each module marker MM ∈ {pre, post} is a 2-state bit; with M1, M3, M7
  // that's 8 permutations of (position vs each gate-blocking marker). We
  // probe by varying `position` across all 8 logical states and asserting
  // gateBlockingActive matches the ratified mapping.
  const cases = [
    // position → expected [#1, #2, #3, #4] blocked vector
    { position: 'M0', expected: [false, false, false, false] }, // pre-M1, pre-M3, pre-M7
    { position: 'M1', expected: [false, false, true,  false] }, // post-M1
    { position: 'M2', expected: [false, false, true,  false] },
    { position: 'M3', expected: [false, false, true,  true]  }, // post-M3
    { position: 'M4', expected: [false, false, true,  true]  },
    { position: 'M5', expected: [false, false, true,  true]  },
    { position: 'M6', expected: [false, false, true,  true]  },
    { position: 'M7', expected: [true,  true,  true,  true]  }, // post-M7
  ];
  for (const c of cases) {
    test(`position=${c.position} → blocked=${c.expected.join(',')}`, () => {
      const actual = ASSERTION_ORDER.map(id => gateBlockingActive(DEFAULT_SCHEDULE, id, c.position));
      assert.deepEqual(actual, c.expected);
    });
  }
});

describe('R2 group 3 — probe-authorship vs gate-blocking decoupling', () => {
  // Encodes the ratified spec resolution: even if #3's probe is "authored"
  // and returns NYI (because its real check needs M1 deletion to be observable),
  // scaffold at M0 stays PASS because gate-blocking is false there.
  // This is the executable form of: "probe-authorship schedule ≠ gate-blocking schedule".
  test('scaffold/M0: #3 NYI is permissible (pre-M1, gate-blocking inactive)', () => {
    const out = computeVerdict({
      mode: 'scaffold',
      results: vec(['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED']),
      schedule: DEFAULT_SCHEDULE,
      position: 'M0',
    });
    assert.equal(out.verdict, 'PASS');
    // The per-assertion meta should report blocked=false for #3 at M0.
    const a3 = out.perAssertion.find(x => x.id === 'codec-cap-matches-factory');
    assert.equal(a3.blocked, false);
  });

  test('scaffold/M1: #3 NYI flips to FAIL because gate-blocking active', () => {
    const out = computeVerdict({
      mode: 'scaffold',
      results: vec(['NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED', 'NOT_YET_IMPLEMENTED']),
      schedule: DEFAULT_SCHEDULE,
      position: 'M1',
    });
    assert.equal(out.verdict, 'FAIL');
    const a3 = out.perAssertion.find(x => x.id === 'codec-cap-matches-factory');
    assert.equal(a3.blocked, true);
  });
});

describe('R2 group 4 — schema-version invalidator', () => {
  test('validateSchedule throws when schemaVersion does not match', () => {
    const stale = { ...DEFAULT_SCHEDULE, schemaVersion: SCHEDULE_SCHEMA_VERSION + 100 };
    assert.throws(() => validateSchedule(stale), /schema version mismatch/);
  });
  test('validateSchedule throws when assertion gateBlockingAt is unknown', () => {
    const bad = {
      schemaVersion: SCHEDULE_SCHEMA_VERSION,
      modules: ['M0', 'M1'],
      assertions: { 'streamer-page-absent': { gateBlockingAt: 'M99' } },
    };
    assert.throws(() => validateSchedule(bad), /not in schedule\.modules/);
  });
  test('DEFAULT_SCHEDULE itself validates cleanly', () => {
    validateSchedule(DEFAULT_SCHEDULE);
  });
});

describe('R2 — guard rails (defensive)', () => {
  test('unknown mode throws', () => {
    assert.throws(
      () => computeVerdict({ mode: 'foo', results: vec(['PASS', 'PASS', 'PASS', 'PASS']) }),
      /mode must be/
    );
  });
});

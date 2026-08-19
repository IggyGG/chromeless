// conformance/lib/check.mjs — the check contract.
//
// Deliberately the same shape as verification/lib/registry.mjs's assertion
// contract, because that one is already proven by 10 test files and a live
// CI gate. The differences are the ones the audience forces:
//
//   * a check names the SPEC CLAUSE it enforces. verification/ answers "is
//     this repo's tree correct", where the reader already has the tree. This
//     answers "is YOUR deployment correct", where the reader may have never
//     seen this repo — so "FAIL" is useless without "here is the rule, and
//     here is where it is written".
//
//   * required vs advisory. verification/ has a gate-blocking SCHEDULE
//     keyed to migration milestones (M0..M7), which is meaningless to a
//     third party. The equivalent axis here is whether a check is part of
//     the contract (required: a conforming deployment MUST pass) or a
//     recommendation (advisory: passing is better, failing is legal).
//
// A check:
//   {
//     id:       'rejects-sdp-offer-tag',      // stable, kebab-case
//     suite:    'signaling-wire',
//     title:    'off-contract tag `sdp_offer` is rejected',
//     spec:     'capture/signaling/cb_wire_envelope.h:40',
//     required: true,
//     run:      async (ctx) => result.pass({...}) | result.fail({...}) | ...
//   }

export const Status = Object.freeze({
  PASS: 'PASS',
  FAIL: 'FAIL',
  SKIP: 'SKIP',      // not applicable to this target/role (not a failure)
  ERROR: 'ERROR',    // the check itself broke — never silently a pass
});

export const VALID_STATUSES = new Set(Object.values(Status));

export function isValidStatus(s) {
  return VALID_STATUSES.has(s);
}

// Evidence discipline: `expected` and `observed` are what makes a failure
// actionable without a debugger, so fail()/pass() take them explicitly
// rather than letting each check invent its own blob shape.
export const result = {
  pass: (evidence = {}) => ({ status: Status.PASS, evidence }),
  fail: ({ expected, observed, hint, ...rest } = {}) => ({
    status: Status.FAIL,
    evidence: { expected, observed, ...(hint ? { hint } : {}), ...rest },
  }),
  skip: (reason, evidence = {}) => ({
    status: Status.SKIP,
    evidence: { reason, ...evidence },
  }),
  error: (message, evidence = {}) => ({
    status: Status.ERROR,
    evidence: { message, ...evidence },
  }),
};

export function createRegistry(checks = []) {
  const byId = new Map();
  for (const c of checks) {
    if (!c || !c.id || typeof c.run !== 'function') {
      throw new TypeError(`registry: invalid check: ${JSON.stringify(c)}`);
    }
    if (byId.has(c.id)) throw new Error(`registry: duplicate check id: ${c.id}`);
    if (!c.spec) throw new TypeError(`registry: check ${c.id} has no spec reference`);
    if (typeof c.required !== 'boolean') {
      throw new TypeError(`registry: check ${c.id} must declare required:boolean`);
    }
    byId.set(c.id, c);
  }
  return {
    list() { return [...byId.values()]; },
    get(id) { return byId.get(id); },
    has(id) { return byId.has(id); },
    bySuite(suite) { return [...byId.values()].filter(c => c.suite === suite); },
  };
}

// Verdict algebra.
//
// PASS iff no REQUIRED check failed or errored. Advisory failures are
// reported loudly but do not set the exit code — otherwise "recommended"
// silently becomes "mandatory" and the distinction the kit advertises is a
// lie.
//
// ERROR on a required check counts as FAIL: a check that could not run has
// NOT demonstrated conformance, and treating "couldn't tell" as "fine" is
// how a green result stops meaning anything.
//
// All-SKIP is reported as its own verdict rather than PASS. A run where
// nothing was applicable proved nothing, and a caller that sees PASS will
// reasonably believe otherwise.
export function computeVerdict(results) {
  const required = results.filter(r => r.required);
  const ran = results.filter(r => r.status !== Status.SKIP);

  if (ran.length === 0) {
    return { verdict: 'NOTHING_RAN', exitCode: 1, failed: [], advisoryFailed: [] };
  }

  const failed = required
    .filter(r => r.status === Status.FAIL || r.status === Status.ERROR)
    .map(r => r.id);
  const advisoryFailed = results
    .filter(r => !r.required && (r.status === Status.FAIL || r.status === Status.ERROR))
    .map(r => r.id);

  return {
    verdict: failed.length === 0 ? 'PASS' : 'FAIL',
    exitCode: failed.length === 0 ? 0 : 1,
    failed,
    advisoryFailed,
  };
}

// verification/lib/registry.mjs — R1 assertion-registry contract.
//
// An assertion is a small object:
//   {
//     id:        'streamer-page-absent',     // stable; gate-blocking config keys on this
//     number:    1,                          // 1..4 in spec order
//     title:     'streamer-page physical absence',
//     run:       async (ctx) => ({ status, evidence?, error? })
//   }
//
// `ctx` is the gate runtime context (mode, image tag, target, timeout, etc.)
// — see native-peer-gate.mjs main() for full shape.
//
// `status` is one of:
//   'PASS'                  probe ran, criterion satisfied
//   'FAIL'                  probe ran, criterion violated (evidence describes)
//   'NOT_YET_IMPLEMENTED'   probe is shimmed / pre-enabling-module
//   'SKIPPED'               gate-blocking schedule disabled this assertion at the current pipeline position
//
// `evidence` is a JSON-serializable detail blob the gate JSON pipes through.
// `error` is set when the probe itself threw (status='FAIL', evidence
// describes the failure context).

export const Status = Object.freeze({
  PASS: 'PASS',
  FAIL: 'FAIL',
  NOT_YET_IMPLEMENTED: 'NOT_YET_IMPLEMENTED',
  SKIPPED: 'SKIPPED',
});

export const VALID_STATUSES = new Set(Object.values(Status));

export function isValidStatus(s) {
  return VALID_STATUSES.has(s);
}

// Convenience constructors for assertion `run` returns.
export const result = {
  pass: (evidence = {}) => ({ status: Status.PASS, evidence }),
  fail: (evidence = {}) => ({ status: Status.FAIL, evidence }),
  notYetImplemented: (evidence = {}) => ({ status: Status.NOT_YET_IMPLEMENTED, evidence }),
  skipped: (evidence = {}) => ({ status: Status.SKIPPED, evidence }),
};

// Default registry for the actual gate — real assertions plug in via
// verification/native-peer-gate.mjs at boot; R3/R4/R5/R6 each add one.
// Kept empty here so tests can inject synthetic stubs without ceremony.
export function createRegistry(assertions = []) {
  const byId = new Map();
  for (const a of assertions) {
    if (!a || !a.id || typeof a.run !== 'function') {
      throw new TypeError(`registry: invalid assertion: ${JSON.stringify(a)}`);
    }
    if (byId.has(a.id)) throw new Error(`registry: duplicate assertion id: ${a.id}`);
    byId.set(a.id, a);
  }
  return {
    list() { return [...byId.values()]; },
    get(id) { return byId.get(id); },
    has(id) { return byId.has(id); },
  };
}

// Stub set R1 uses to lock the JSON contract before any real probe exists.
// R2 swaps real assertions in via the same shape.
export function defaultStubAssertions() {
  return [
    {
      id: 'streamer-page-absent',
      number: 1,
      title: 'streamer-page physical absence',
      async run() { return result.notYetImplemented({ stub: true }); },
    },
    {
      id: 'bridges-absent',
      number: 2,
      title: 'input-bridge & cursor-watcher absence',
      async run() { return result.notYetImplemented({ stub: true }); },
    },
    {
      id: 'codec-cap-matches-factory',
      number: 3,
      title: 'codec capability matches encoder-factory',
      async run() { return result.notYetImplemented({ stub: true }); },
    },
    {
      id: 'native-peer-connects',
      number: 4,
      title: 'native peer connectivity',
      async run() { return result.notYetImplemented({ stub: true }); },
    },
  ];
}

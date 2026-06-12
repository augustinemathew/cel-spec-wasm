// Pinned-fixture e2e for the conformance harness: runs a representative
// in-scope corpus file (`basic.textproto`) end-to-end through compile →
// eval → compare and asserts the outcome counts.  This is the CI gate
// that exercises the whole harness (textproto → corpus → classify →
// runner → compare) against the in-process `compiler.wasm` + the TS
// evaluator; the full-corpus run lives behind the `npm run conformance`
// script.  Compilation runs in-process (no native CLI), so this runs
// unconditionally.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7.

import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { runCorpus, type ConformanceReport } from './harness.js';

const CORPUS_DIR = fileURLToPath(
  new URL('../../../../spec/tests/simple/testdata/', import.meta.url),
);

// One shared run of the pinned file; the cases assert slices of it.
let report: ConformanceReport | undefined;

async function pinnedReport(): Promise<ConformanceReport> {
  report ??= await runCorpus({ corpusDir: CORPUS_DIR, files: ['basic'] });
  return report;
}

describe('conformance harness — pinned basic.textproto', () => {
  it('runs the file with zero unexplained failures', async () => {
    const r = await pinnedReport();
    expect(r.total).toBe(43);
    expect(r.fail).toBe(0);
    // Every row is accounted for: pass + skip + fail == total.
    expect(r.pass + r.skip + r.fail).toBe(r.total);
    // A meaningful number of real rows pass (not all-skip).
    expect(r.pass).toBeGreaterThan(30);
  }, 60_000);

  it('skips only with categorized reasons', async () => {
    const r = await pinnedReport();
    for (const [category, count] of r.skipByCategory) {
      expect(count).toBeGreaterThan(0);
      // Every skip carries one of the known categories.
      expect([
        'disable_check',
        'check_only',
        'envelope',
        'type_env',
        'bindings',
        'object_value',
        'proto_unimpl',
        'ext_unimpl',
        'static_subset',
        'compile_unimpl',
        'eval_unimpl',
        'embedded_nul',
      ]).toContain(category);
    }
  });
});

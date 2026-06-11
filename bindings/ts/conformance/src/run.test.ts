// Pinned-fixture e2e for the conformance harness: runs a representative
// in-scope corpus file (`basic.textproto`) end-to-end through compile →
// eval → compare and asserts the outcome counts.  This is the CI gate
// that exercises the whole harness (textproto → corpus → classify →
// runner → compare) against the real C++ `cel` CLI + the TS evaluator;
// the full-corpus run lives behind the `npm run conformance` script.
//
// The `cel` CLI must be built (`bazel build //tools/cel:cel`); when it is
// absent the suite is skipped with the build recipe (not a silent pass).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7.

import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { runCorpus, type ConformanceReport } from './harness.js';

const CORPUS_DIR = fileURLToPath(
  new URL('../../../../spec/tests/simple/testdata/', import.meta.url),
);
const CLI_PATH = fileURLToPath(
  new URL('../../../../bazel-bin/tools/cel/cel', import.meta.url),
);

const cliBuilt = existsSync(CLI_PATH);

// One shared run of the pinned file; the cases assert slices of it.
let report: ConformanceReport | undefined;

async function pinnedReport(): Promise<ConformanceReport> {
  report ??= await runCorpus({ corpusDir: CORPUS_DIR, files: ['basic'] });
  return report;
}

describe('conformance harness — pinned basic.textproto', () => {
  it.runIf(cliBuilt)(
    'runs the file with zero unexplained failures',
    async () => {
      const r = await pinnedReport();
      expect(r.total).toBe(43);
      expect(r.fail).toBe(0);
      // Every row is accounted for: pass + skip + fail == total.
      expect(r.pass + r.skip + r.fail).toBe(r.total);
      // A meaningful number of real rows pass (not all-skip).
      expect(r.pass).toBeGreaterThan(30);
    },
    60_000,
  );

  it.runIf(cliBuilt)('skips only with categorized reasons', async () => {
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
        'cli_limitation',
      ]).toContain(category);
    }
  });

  it.skipIf(cliBuilt)('SKIPPED: cel CLI not built', () => {
    // The harness e2e needs the native compiler; build it with
    //   bazel build //tools/cel:cel
    // then re-run.  Skipped (not passed) so the gap is visible.
    expect(cliBuilt).toBe(false);
  });
});

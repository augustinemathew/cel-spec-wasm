// Tests for the per-row runner's outcome classification, driven end-to-
// end through the real compiler + evaluator on small synthetic rows.
// Each case pins one branch of `runRow`: a value match, an eval-error
// match, a divide-by-zero error value, the pre-compile scope skips, and
// the compile-failure carve-outs (static_subset / ext / proto).
//
// Needs the `cel` CLI built; the suite skips with the build recipe when
// it is absent (not a silent pass).

import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { Engine } from '@cel-wasm/eval';
import { beforeAll, describe, expect, it } from 'vitest';

import type { SimpleTest } from './corpus.js';
import { runRow, type RowResult } from './runner.js';

const CLI_PATH = fileURLToPath(
  new URL('../../../../bazel-bin/tools/cel/cel', import.meta.url),
);
const cliBuilt = existsSync(CLI_PATH);

function row(overrides: Partial<SimpleTest>): SimpleTest {
  return {
    file: 't',
    section: 's',
    name: 'n',
    expr: '1',
    container: '',
    disableCheck: false,
    checkOnly: false,
    typeEnv: [],
    bindings: new Map(),
    unsupportedBindingReason: undefined,
    matcher: { kind: 'value', value: { kind: 'int', value: 1n } },
    ...overrides,
  };
}

describe('runRow — classification', () => {
  let engine: Engine;

  beforeAll(async () => {
    if (cliBuilt) {
      engine = await Engine.create();
    }
  });

  async function run(test: SimpleTest): Promise<RowResult> {
    return runRow(test, engine);
  }

  it.runIf(cliBuilt)('passes a matching int value', async () => {
    const r = await run(
      row({
        expr: '40 + 2',
        matcher: { kind: 'value', value: { kind: 'int', value: 42n } },
      }),
    );
    expect(r.outcome).toBe('pass');
  });

  it.runIf(cliBuilt)('fails a mismatched value', async () => {
    const r = await run(
      row({
        expr: '1',
        matcher: { kind: 'value', value: { kind: 'int', value: 2n } },
      }),
    );
    expect(r.outcome).toBe('fail');
  });

  it.runIf(cliBuilt)(
    'passes an eval-error matcher on divide-by-zero',
    async () => {
      const r = await run(
        row({ expr: '1 / 0', matcher: { kind: 'evalError' } }),
      );
      expect(r.outcome).toBe('pass');
    },
  );

  it.runIf(cliBuilt)('passes the implicit bool-true matcher', async () => {
    const r = await run(row({ expr: '2 > 1', matcher: { kind: 'boolTrue' } }));
    expect(r.outcome).toBe('pass');
  });

  it.runIf(cliBuilt)('skips a disable_check row before compiling', async () => {
    const r = await run(row({ disableCheck: true }));
    expect(r).toMatchObject({ outcome: 'skip', category: 'disable_check' });
  });

  it.runIf(cliBuilt)('skips a dyn expression as static_subset', async () => {
    const r = await run(
      row({
        expr: 'dyn(1) + dyn(2)',
        matcher: { kind: 'value', value: { kind: 'int', value: 3n } },
      }),
    );
    expect(r).toMatchObject({ outcome: 'skip', category: 'static_subset' });
  });

  it.runIf(cliBuilt)(
    'skips an out-of-scope optionals-extension call',
    async () => {
      // The optionals extension (`.?` optional-chaining) is out of scope:
      // it reaches the compiler as a `dyn` the static-subset gate rejects
      // (or, for the `{?`/`[?` init forms, as an ext-lib parse failure).
      // Either way it is a SKIP, never a FAIL.
      const r = await run(
        row({
          expr: "{'c': 1}.?c.orValue(0)",
          matcher: { kind: 'value', value: { kind: 'int', value: 1n } },
        }),
      );
      expect(r.outcome).toBe('skip');
      expect(['static_subset', 'ext_unimpl']).toContain(r.category);
    },
  );

  it.runIf(cliBuilt)('skips a proto construction as proto_unimpl', async () => {
    const r = await run(
      row({
        expr: 'TestAllTypes{single_int64: 17}.single_int64',
        container: 'cel.expr.conformance.proto3',
        matcher: { kind: 'value', value: { kind: 'int', value: 17n } },
      }),
    );
    expect(r).toMatchObject({ outcome: 'skip', category: 'proto_unimpl' });
  });

  it.runIf(cliBuilt)(
    'skips host-aggregate equality as an eval_unimpl gap',
    async () => {
      const r = await run(
        row({
          expr: '[1, 2] == x',
          typeEnv: [
            {
              name: 'x',
              type: {
                kind: 'list',
                elem: { kind: 'primitive', name: 'INT64' },
              },
            },
          ],
          bindings: new Map([['x', [3n, 4n]]]),
          matcher: { kind: 'value', value: { kind: 'bool', value: false } },
        }),
      );
      expect(r).toMatchObject({ outcome: 'skip', category: 'eval_unimpl' });
    },
  );

  it.skipIf(cliBuilt)('SKIPPED: cel CLI not built', () => {
    // Build with `bazel build //tools/cel:cel`, then re-run.
    expect(cliBuilt).toBe(false);
  });
});

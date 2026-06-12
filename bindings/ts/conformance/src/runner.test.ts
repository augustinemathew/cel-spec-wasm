// Tests for the per-row runner's outcome classification, driven end-to-
// end through the in-process `compiler.wasm` + the TS evaluator on small
// synthetic rows.  Each case pins one branch of `runRow`: a value match,
// an eval-error match, a divide-by-zero error value, the pre-compile scope
// skips, and the compile-failure carve-outs (static_subset / ext / proto).
//
// Compilation runs in-process (no native CLI), so these run unconditionally.

import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { Engine } from '@cel-wasm/eval';
import { DescriptorSet } from '@cel-wasm/eval/proto';
import { beforeAll, describe, expect, it } from 'vitest';

import type { SimpleTest } from './corpus.js';
import { buildBoundMessage } from './proto-compare.js';
import { runRow, type ProtoEnv, type RowResult } from './runner.js';
import { parseTextproto } from './textproto.js';

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
    engine = await Engine.create();
  });

  async function run(test: SimpleTest): Promise<RowResult> {
    return runRow(test, engine);
  }

  it('passes a matching int value', async () => {
    const r = await run(
      row({
        expr: '40 + 2',
        matcher: { kind: 'value', value: { kind: 'int', value: 42n } },
      }),
    );
    expect(r.outcome).toBe('pass');
  });

  it('fails a mismatched value', async () => {
    const r = await run(
      row({
        expr: '1',
        matcher: { kind: 'value', value: { kind: 'int', value: 2n } },
      }),
    );
    expect(r.outcome).toBe('fail');
  });

  it('passes an eval-error matcher on divide-by-zero', async () => {
    const r = await run(row({ expr: '1 / 0', matcher: { kind: 'evalError' } }));
    expect(r.outcome).toBe('pass');
  });

  it('passes the implicit bool-true matcher', async () => {
    const r = await run(row({ expr: '2 > 1', matcher: { kind: 'boolTrue' } }));
    expect(r.outcome).toBe('pass');
  });

  it('skips a disable_check row before compiling', async () => {
    const r = await run(row({ disableCheck: true }));
    expect(r).toMatchObject({ outcome: 'skip', category: 'disable_check' });
  });

  it('skips a dyn expression as static_subset', async () => {
    const r = await run(
      row({
        expr: 'dyn(1) + dyn(2)',
        matcher: { kind: 'value', value: { kind: 'int', value: 3n } },
      }),
    );
    expect(r).toMatchObject({ outcome: 'skip', category: 'static_subset' });
  });

  it('skips an out-of-scope optionals-extension call', async () => {
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
  });

  it('skips a proto construction as proto_unimpl', async () => {
    const r = await run(
      row({
        expr: 'TestAllTypes{single_int64: 17}.single_int64',
        container: 'cel.expr.conformance.proto3',
        matcher: { kind: 'value', value: { kind: 'int', value: 17n } },
      }),
    );
    expect(r).toMatchObject({ outcome: 'skip', category: 'proto_unimpl' });
  });

  it('skips host-aggregate equality as an eval_unimpl gap', async () => {
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
  });
});

// With the descriptor set wired into both compile (as `descriptorSetBytes`
// marshalled through the compiler wasm) and the Engine
// (`Engine.create({descriptors})`), proto construction / field-read rows run
// instead of skipping.  These pin the object_value PASS path + the field-read
// PASS path end-to-end.
const FDS_PATH = fileURLToPath(
  new URL('../fixtures/cel_conformance_protos.fds', import.meta.url),
);
const protoReady = existsSync(FDS_PATH);

describe('runRow — proto descriptor path', () => {
  let engine: Engine;
  let proto: ProtoEnv;

  beforeAll(async () => {
    if (protoReady) {
      const bytes = new Uint8Array(readFileSync(FDS_PATH));
      engine = await Engine.create({ descriptors: bytes });
      proto = {
        descriptors: DescriptorSet.fromFileDescriptorSet(bytes),
        descriptorSetBytes: bytes,
      };
    }
  });

  it.runIf(protoReady)('passes an object_value construction row', async () => {
    const r = await runRow(
      row({
        expr: 'cel.expr.conformance.proto3.TestAllTypes{single_int64: 17}',
        matcher: {
          kind: 'value',
          value: {
            kind: 'object',
            fqn: 'cel.expr.conformance.proto3.TestAllTypes',
            message: parseTextproto('single_int64: 17'),
          },
        },
      }),
      engine,
      proto,
    );
    expect(r.outcome).toBe('pass');
  });

  it.runIf(protoReady)('passes a proto field-read row', async () => {
    const r = await runRow(
      row({
        expr: 'cel.expr.conformance.proto3.TestAllTypes{single_int64: 17}.single_int64',
        matcher: { kind: 'value', value: { kind: 'int', value: 17n } },
      }),
      engine,
      proto,
    );
    expect(r.outcome).toBe('pass');
  });

  // WKT-typed field construction from a scalar — the scalar is wrapped into the
  // target WKT message (proto3.textproto "int64_wrapper" / "value" / "struct").
  // These passed skip→pass once `cel_set_field` learned to wrap.
  it.runIf(protoReady)('passes Int64Value wrapper construction', async () => {
    const r = await runRow(
      row({
        expr: 'cel.expr.conformance.proto3.TestAllTypes{single_int64_wrapper: -321}',
        matcher: {
          kind: 'value',
          value: {
            kind: 'object',
            fqn: 'cel.expr.conformance.proto3.TestAllTypes',
            message: parseTextproto('single_int64_wrapper { value: -321 }'),
          },
        },
      }),
      engine,
      proto,
    );
    expect(r.outcome).toBe('pass');
  });

  it.runIf(protoReady)('passes a dynamic Value construction', async () => {
    const r = await runRow(
      row({
        expr: "cel.expr.conformance.proto3.TestAllTypes{single_value: 'foo'}",
        matcher: {
          kind: 'value',
          value: {
            kind: 'object',
            fqn: 'cel.expr.conformance.proto3.TestAllTypes',
            message: parseTextproto('single_value { string_value: "foo" }'),
          },
        },
      }),
      engine,
      proto,
    );
    expect(r.outcome).toBe('pass');
  });

  it.runIf(protoReady)('passes a Struct construction from a map', async () => {
    const r = await runRow(
      row({
        expr: "cel.expr.conformance.proto3.TestAllTypes{single_struct: {'one': 1, 'two': 2}}",
        matcher: {
          kind: 'value',
          value: {
            kind: 'object',
            fqn: 'cel.expr.conformance.proto3.TestAllTypes',
            message: parseTextproto(
              'single_struct { fields { key: "one" value { number_value: 1.0 } } ' +
                'fields { key: "two" value { number_value: 2.0 } } }',
            ),
          },
        },
      }),
      engine,
      proto,
    );
    expect(r.outcome).toBe('pass');
  });

  // A message-typed variable BINDING (proto2/proto3 "singular_bind" rows):
  // the corpus loader lowers the object_value binding to a protobufjs message
  // (buildBoundMessage) and the eval binding backs it by its own $type.
  it.runIf(protoReady)('passes a message-binding field-read row', async () => {
    if (proto.descriptors === undefined) {
      throw new Error('descriptors not loaded');
    }
    const bound = buildBoundMessage(
      proto.descriptors,
      'cel.expr.conformance.proto3.TestAllTypes',
      parseTextproto('single_int64: -99'),
    );
    const r = await runRow(
      row({
        expr: 'x.single_int64',
        typeEnv: [
          {
            name: 'x',
            type: {
              kind: 'message',
              fqn: 'cel.expr.conformance.proto3.TestAllTypes',
            },
          },
        ],
        bindings: new Map([['x', bound]]),
        matcher: { kind: 'value', value: { kind: 'int', value: -99n } },
      }),
      engine,
      proto,
    );
    expect(r.outcome).toBe('pass');
  });

  // Out-of-range 32-bit WKT-wrapper assignment is a SEPARATE gap (the binding
  // wraps but does not narrow-check), classified as a verified eval_unimpl skip
  // — NOT the scalar-wrapping feature.  dynamic.textproto "field_assign_*_range".
  it.runIf(protoReady)(
    'skips an out-of-range 32-bit wrapper assignment',
    async () => {
      const r = await runRow(
        row({
          expr: 'cel.expr.conformance.proto3.TestAllTypes{single_int32_wrapper: 12345678900}',
          container: 'cel.expr.conformance.proto3',
          matcher: { kind: 'evalError' },
        }),
        engine,
        proto,
      );
      expect(r).toMatchObject({ outcome: 'skip', category: 'eval_unimpl' });
    },
  );

  it.skipIf(protoReady)('SKIPPED: FDS fixture not built', () => {
    expect(protoReady).toBe(false);
  });
});

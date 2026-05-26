/**
 * Real-wasm e2e — scalars. Every test title reads
 * `<source expression>  →  <expected output>`; the describe block names the
 * INPUT. Covers: proto message scalar field reads (exact CEL kind), scalar
 * input↔output identity with edge values, arena-backed string/bytes
 * results (computed in wasm), and literal/error return kinds.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { Value } from '../../src/value.js';
import { Activation } from '../../src/activation.js';
import { CelKind } from '../../src/celvalue.js';
import { setup, src, show, SESSION_TOKEN, type Harness } from './harness.js';

let h: Harness;
beforeAll(async () => {
  h = await setup();
});

interface Row {
  readonly fixture: string;
  readonly want: Value;
}
// `<expr>  →  <output>` titles, with the source pulled from the manifest.
function titled(rows: readonly Row[]): (Row & { title: string })[] {
  return rows.map((r) => ({
    ...r,
    title: `${src(r.fixture)}  →  ${show(r.want)}`,
  }));
}

describe('input: proto Customer message — scalar field reads', () => {
  const ROWS: Row[] = [
    { fixture: 'scalar_string.wasm', want: Value.string('Ann') },
    { fixture: 'scalar_int.wasm', want: Value.int(30n) },
    { fixture: 'scalar_int64.wasm', want: Value.int(42n) },
    { fixture: 'scalar_uint.wasm', want: Value.uint(7n) },
    { fixture: 'scalar_uint64.wasm', want: Value.uint(100000n) },
    { fixture: 'scalar_double.wasm', want: Value.double(9.5) },
    { fixture: 'scalar_bool.wasm', want: Value.bool(true) },
    { fixture: 'scalar_bytes.wasm', want: Value.bytes(SESSION_TOKEN) },
  ];
  it.each(titled(ROWS))('$title', async ({ fixture, want }) => {
    expect(await h.run(fixture, h.bind('u', h.protoCustomer()))).toEqual(want);
  });
});

describe('input: a bound scalar `x` — identity (eval `x`) round-trips', () => {
  interface IdRow {
    readonly fixture: string;
    readonly value: Value;
  }
  const ROWS: IdRow[] = [
    { fixture: 'id_int.wasm', value: Value.int(41n) },
    { fixture: 'id_int.wasm', value: Value.int(-(2n ** 63n)) }, // INT64_MIN
    { fixture: 'id_uint.wasm', value: Value.uint(7n) },
    { fixture: 'id_uint.wasm', value: Value.uint(2n ** 64n - 1n) }, // UINT64_MAX
    { fixture: 'id_double.wasm', value: Value.double(2.5) },
    { fixture: 'id_double.wasm', value: Value.double(Infinity) },
    { fixture: 'id_double.wasm', value: Value.double(NaN) },
    { fixture: 'id_bool.wasm', value: Value.bool(true) },
    { fixture: 'id_bool.wasm', value: Value.bool(false) },
    { fixture: 'id_string.wasm', value: Value.string('héllo🌍') },
    {
      fixture: 'id_bytes.wasm',
      value: Value.bytes(new Uint8Array([0, 9, 255, 0])),
    },
  ];
  const rows = ROWS.map((r) => ({
    ...r,
    title: `x = ${show(r.value)}  →  ${show(r.value)}`,
  }));
  it.each(rows)('$title', async ({ fixture, value }) => {
    expect(await h.run(fixture, h.bind('x', value))).toEqual(value);
  });
});

describe('input: two bound scalars — arena-backed string/bytes result', () => {
  it(`${src('str_concat.wasm')}  (a="foo", b="bar")  →  "foobar"`, async () => {
    const act = h.bind('a', Value.string('foo')).bind('b', Value.string('bar'));
    expect(await h.run('str_concat.wasm', act)).toEqual(Value.string('foobar'));
  });

  it(`${src('bytes_concat.wasm')}  (0x01ff, 0x00)  →  0x01ff00`, async () => {
    const act = h
      .bind('a', Value.bytes(new Uint8Array([0x01, 0xff])))
      .bind('b', Value.bytes(new Uint8Array([0x00])));
    expect(await h.run('bytes_concat.wasm', act)).toEqual(
      Value.bytes(new Uint8Array([0x01, 0xff, 0x00])),
    );
  });
});

describe('input: none — literal & error return kinds', () => {
  const ROWS: Row[] = [
    { fixture: 'lit_null.wasm', want: Value.null() },
    { fixture: 'err_div0.wasm', want: { kind: CelKind.Error, errorCode: 11 } },
  ];
  it.each(titled(ROWS))('$title', async ({ fixture, want }) => {
    expect(await h.run(fixture, new Activation())).toEqual(want);
  });
});

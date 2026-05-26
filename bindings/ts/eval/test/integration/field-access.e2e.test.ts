/**
 * Real-wasm e2e — field access. Test titles read
 * `<source expression>  →  <expected output>`; the describe names the
 * INPUT (proto message vs JS object vs HostMsg3). Covers: field access that
 * agrees across both backings, a JSON object fed to a PROTO-typed
 * expression (no registry), and enums + proto repeated element types.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { Value, asBool } from '../../src/value.js';
import { Program } from '../../src/program.js';
import { Activation } from '../../src/activation.js';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { setup, src, show, TESTDATA, type Harness } from './harness.js';

let h: Harness;
beforeAll(async () => {
  h = await setup();
});

interface Row {
  readonly fixture: string;
  readonly want: Value;
}
function titled(rows: readonly Row[]): (Row & { title: string })[] {
  return rows.map((r) => ({
    ...r,
    title: `${src(r.fixture)}  →  ${show(r.want)}`,
  }));
}

// Field access that yields the SAME result through a real proto message and
// a plain JS object that mirrors it.
const DUAL: Row[] = [
  { fixture: 'has_field.wasm', want: Value.bool(true) }, // has(u.name)
  { fixture: 'has_absent.wasm', want: Value.bool(true) }, // has(u.billing_address)
  { fixture: 'nested_msg.wasm', want: Value.string('NYC') }, // u.billing_address.city
  { fixture: 'repeated_index.wasm', want: Value.string('gold') }, // u.tags[0]
  { fixture: 'map_string.wasm', want: Value.string('v') }, // u.metadata["k"]
];

describe('input: proto Customer message — field access', () => {
  it.each(titled(DUAL))('$title', async ({ fixture, want }) => {
    expect(await h.run(fixture, h.bind('u', h.protoCustomer()))).toEqual(want);
  });
  it(`${src('map_int.wasm')}  →  50  (proto int-keyed map)`, async () => {
    expect(await h.run('map_int.wasm', h.bind('u', h.protoCustomer()))).toEqual(
      Value.int(50n),
    );
  });
});

describe('input: plain JS object (mirrors the proto) — same field access', () => {
  it.each(titled(DUAL))('$title', async ({ fixture, want }) => {
    expect(await h.run(fixture, h.bind('u', h.objectCustomer()))).toEqual(want);
  });
});

describe('input: JS object into a PROTO-compiled expression (no registry)', () => {
  // `policy.wasm` was type-checked against the Customer proto in C++; here a
  // plain JS object drives it, and the engine has NO TypeRegistry.
  const POLICY = src('policy.wasm'); // u.is_premium && u.age > 18
  let bare: Awaited<ReturnType<Harness['bareEngine']>>;
  beforeAll(async () => {
    bare = await h.bareEngine();
  });
  async function evalPolicy(u: Value): Promise<Value> {
    const inst = await bare.plan(
      Program.fromBytes(readFileSync(join(TESTDATA, 'policy.wasm'))),
    );
    return inst.eval(new Activation().bind('u', u));
  }

  it(`${POLICY}  ({premium, age 30})  →  true`, async () => {
    expect(
      await evalPolicy(Value.object({ is_premium: true, age: 30n })),
    ).toEqual(Value.bool(true));
  });
  it(`${POLICY}  ({premium, age 5})  →  false`, async () => {
    expect(
      await evalPolicy(Value.object({ is_premium: true, age: 5n })),
    ).toEqual(Value.bool(false));
  });
  it(`${POLICY}  ({not premium})  →  false`, async () => {
    expect(
      await evalPolicy(Value.object({ is_premium: false, age: 30n })),
    ).toEqual(Value.bool(false));
  });
  it(`${POLICY}  — SAME wasm also accepts a real proto message`, async () => {
    expect(
      asBool(await h.run('policy.wasm', h.bind('u', h.protoCustomer()))),
    ).toBe(true);
  });
});

describe('input: proto HostMsg3 — enum + repeated element types', () => {
  const ROWS: Row[] = [
    { fixture: 'enum_return.wasm', want: Value.int(7n) }, // m.kind (enum → int)
    { fixture: 'rep_int_index.wasm', want: Value.int(11n) }, // m.rep_i32[0]
    { fixture: 'rep_bool_index.wasm', want: Value.bool(true) }, // m.rep_b[0]
    { fixture: 'rep_dbl_index.wasm', want: Value.double(1.5) }, // m.rep_f64[0]
    { fixture: 'rep_msg_field.wasm', want: Value.int(99n) }, // m.rep_msg[0].i32
  ];
  it.each(titled(ROWS))('$title', async ({ fixture, want }) => {
    expect(await h.run(fixture, h.bind('m', h.protoHostMsg()))).toEqual(want);
  });
});

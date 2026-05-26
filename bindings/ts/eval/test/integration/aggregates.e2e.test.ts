/**
 * Real-wasm e2e — aggregates as RETURN VALUES. Test titles read
 * `<source expression>  →  <expected output>`; the describe names the
 * INPUT. Covers messages / lists / maps returned and materialized
 * (asMessage/asList/asMap), through both proto and object backings, AND
 * arena-backed list/map literals built inside wasm (CEL_LIST_ARENA /
 * CEL_MAP_ARENA) — including lists of messages, primitives, and nulls.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import {
  Value,
  asInt,
  asList,
  asMap,
  asMessage,
  asString,
} from '../../src/value.js';
import { CelKind } from '../../src/celvalue.js';
import { Activation } from '../../src/activation.js';
import { setup, src, type Harness } from './harness.js';

let h: Harness;
beforeAll(async () => {
  h = await setup();
});

describe('input: proto Customer — aggregate returns, materialized out', () => {
  it(`${src('msg_return.wasm')}  →  message {name:"Ann"} (typed round-trip)`, async () => {
    const r = await h.run('msg_return.wasm', h.bind('u', h.protoCustomer()));
    expect(r.kind).toBe(CelKind.Message);
    expect(asMessage(r).getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
    const typed = h.customerReg.toMessage(asMessage(r)) as unknown as {
      name: string;
    };
    expect(typed.name).toBe('Ann');
  });

  it(`${src('list_return.wasm')}  →  ["gold", "vip"]`, async () => {
    const r = await h.run('list_return.wasm', h.bind('u', h.protoCustomer()));
    expect(asList(r).map(asString)).toEqual(['gold', 'vip']);
  });

  it(`${src('map_return.wasm')}  →  {"k": "v"}`, async () => {
    const r = await h.run('map_return.wasm', h.bind('u', h.protoCustomer()));
    expect(asMap(r).map(([k, v]) => [asString(k), asString(v)])).toEqual([
      ['k', 'v'],
    ]);
  });
});

describe('input: proto HostMsg3 — repeated returns (ints, list-of-message)', () => {
  it(`${src('rep_int_return.wasm')}  →  [11, 22]`, async () => {
    const r = await h.run('rep_int_return.wasm', h.bind('m', h.protoHostMsg()));
    expect(asList(r).map(asInt)).toEqual([11n, 22n]);
  });

  it(`${src('rep_msg_return.wasm')}  →  [message{i32:99}] (list OF messages)`, async () => {
    const r = await h.run('rep_msg_return.wasm', h.bind('m', h.protoHostMsg()));
    const elems = asList(r);
    expect(elems).toHaveLength(1);
    expect(asMessage(elems[0]!).getField('i32')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 99n },
    });
  });
});

describe('input: plain JS object — aggregate returns', () => {
  it(`${src('msg_return.wasm')}  →  message readable generically`, async () => {
    const r = await h.run('msg_return.wasm', h.bind('u', h.objectCustomer()));
    expect(asMessage(r).getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
  });
  it(`${src('list_return.wasm')}  →  ["gold", "vip"]`, async () => {
    const r = await h.run('list_return.wasm', h.bind('u', h.objectCustomer()));
    expect(asList(r).map(asString)).toEqual(['gold', 'vip']);
  });
  it(`${src('map_return.wasm')}  →  {"k": "v"}`, async () => {
    const r = await h.run('map_return.wasm', h.bind('u', h.objectCustomer()));
    expect(asMap(r).map(([k, v]) => [asString(k), asString(v)])).toEqual([
      ['k', 'v'],
    ]);
  });
});

describe('input: none — arena list literals (built in wasm)', () => {
  it(`${src('arena_list_int.wasm')}  →  [1, 2, 3]`, async () => {
    const r = await h.run('arena_list_int.wasm', h.bind('_', Value.null()));
    expect(asList(r)).toEqual([Value.int(1n), Value.int(2n), Value.int(3n)]);
  });
  it(`${src('arena_list_str.wasm')}  →  ["a", "b"]`, async () => {
    const r = await h.run('arena_list_str.wasm', h.bind('_', Value.null()));
    expect(asList(r).map(asString)).toEqual(['a', 'b']);
  });
  it(`${src('arena_list_bool.wasm')}  →  [true, false]`, async () => {
    const r = await h.run('arena_list_bool.wasm', h.bind('_', Value.null()));
    expect(asList(r)).toEqual([Value.bool(true), Value.bool(false)]);
  });
  it(`${src('arena_list_null.wasm')}  →  [null, null]`, async () => {
    const r = await h.run('arena_list_null.wasm', h.bind('_', Value.null()));
    expect(asList(r)).toEqual([Value.null(), Value.null()]);
  });
});

describe('input: none — arena map literals (built in wasm)', () => {
  it(`${src('arena_map_ii.wasm')}  →  {1: 10, 2: 20}`, async () => {
    const r = await h.run('arena_map_ii.wasm', h.bind('_', Value.null()));
    const pairs = asMap(r).map(([k, v]) => [asInt(k), asInt(v)]);
    expect(pairs).toEqual([
      [1n, 10n],
      [2n, 20n],
    ]);
  });
  it(`${src('arena_map_si.wasm')}  →  {"a": 1}`, async () => {
    const r = await h.run('arena_map_si.wasm', h.bind('_', Value.null()));
    expect(asMap(r).map(([k, v]) => [asString(k), asInt(v)])).toEqual([
      ['a', 1n],
    ]);
  });
});

describe('input: proto Customer — arena aggregates CONTAINING messages', () => {
  it(`${src('arena_list_msg.wasm')}  →  [message, message] (list of protos)`, async () => {
    const r = await h.run(
      'arena_list_msg.wasm',
      h.bind('u', h.protoCustomer()),
    );
    const elems = asList(r);
    expect(elems).toHaveLength(2);
    for (const el of elems) {
      expect(asMessage(el).getField('name')).toEqual({
        host: 'scalar',
        value: { kind: CelKind.String, value: 'Ann' },
      });
    }
  });

  it(`${src('arena_map_smsg.wasm')}  →  {"x": message}`, async () => {
    const r = await h.run(
      'arena_map_smsg.wasm',
      h.bind('u', h.protoCustomer()),
    );
    const pairs = asMap(r);
    expect(pairs).toHaveLength(1);
    const [key, val] = pairs[0]!;
    expect(asString(key)).toBe('x');
    expect(asMessage(val).getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
  });
});

describe('input: none — message CONSTRUCTION (cel_make_message + set_field)', () => {
  it(`${src('mk_message.wasm')}  →  message {name:"Ann", age:7, premium}`, async () => {
    const r = await h.run('mk_message.wasm', new Activation());
    expect(r.kind).toBe(CelKind.Message);
    expect(asMessage(r).getField('name')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'Ann' },
    });
    expect(asMessage(r).getField('age')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 7n },
    });
    expect(asMessage(r).getField('is_premium')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Bool, bool: true },
    });
  });
});

describe('input: caller-built Value.list / Value.map variables', () => {
  it(`${src('listidx.wasm')}  (xs=[10,20,30])  →  20`, async () => {
    const xs = Value.list([Value.int(10n), Value.int(20n), Value.int(30n)]);
    expect(await h.run('listidx.wasm', h.bind('xs', xs))).toEqual(
      Value.int(20n),
    );
  });
  it(`${src('maplookup.wasm')}  (m={"k":7})  →  7`, async () => {
    const m = Value.map([[Value.string('k'), Value.int(7n)]]);
    expect(await h.run('maplookup.wasm', h.bind('m', m))).toEqual(
      Value.int(7n),
    );
  });
});

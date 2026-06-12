// Marshal — write each declared variable's CelValue into its slot.
//
// These tests drive the marshal over a plain `ArrayBuffer` (no wasm
// instance): a hand-built {@link MarshalEnv} backs `view` / `bytes` with
// the buffer and `activationArena` / codec arena with a simple bump
// allocator above the slot region.  That isolates the marshal's
// per-`repr` encoding + the message-var coercion path from the runtime,
// per the WI's "unit-cover the marshal/coercion path" guidance.

import * as protobuf from 'protobufjs';
import { describe, expect, it } from 'vitest';

import { ExternrefTable } from './externref.js';
import type { HostListBacking, HostMapBacking } from './host/aggregates.js';
import {
  CelMarshalError,
  Repr,
  marshalActivation,
  totalActivationBufferBytes,
} from './marshal.js';
import type { MarshalEnv } from './marshal.js';
import { ProtoMessageBacking } from './proto/backing.js';
import { DescriptorSet } from './proto/descriptors.js';
import type { CodecEnv } from './resolving-codec.js';
import {
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CEL_VALUE_SIZE,
  CelKind,
} from './types.js';
import type { CelInput, TypeEntry, VariableEntry } from './types.js';

// A 4 KiB buffer: slots live in the low region, the bump arena above it.
const BUFFER_BYTES = 4096;
const ARENA_FLOOR = 1024;

interface Harness {
  readonly env: MarshalEnv;
  readonly view: DataView;
  readonly bytes: Uint8Array;
  readonly refs: ExternrefTable;
}

function makeHarness(
  descriptors?: DescriptorSet,
  types: readonly TypeEntry[] = [],
): Harness {
  const buffer = new ArrayBuffer(BUFFER_BYTES);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const refs = new ExternrefTable();
  let cursor = ARENA_FLOOR;
  const alloc = (n: number): number => {
    const ptr = cursor;
    cursor += (n + 7) & ~7;
    return ptr;
  };
  const codec: CodecEnv = {
    view: () => view,
    bytes: () => bytes,
    refs,
    arenaAlloc: alloc,
  };
  const env: MarshalEnv = {
    view: () => view,
    bytes: () => bytes,
    refs,
    codec,
    activationArena: { alloc },
    descriptors,
    types,
  };
  return { env, view, bytes, refs };
}

function variable(name: string, repr: Repr, slotOffset: number): VariableEntry {
  return { name, repr, slotOffset, localIndex: 0 };
}

function kindAt(view: DataView, slot: number): number {
  return view.getUint32(slot + CEL_VALUE_KIND_OFFSET, true);
}

function refAt(view: DataView, slot: number): number {
  return view.getUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, true);
}

// ── Scalars ─────────────────────────────────────────────────────────

describe('marshalActivation — scalars', () => {
  it('writes a NULL CelValue', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('x', Repr.NULL, 0)], { x: null });
    expect(kindAt(h.view, 0)).toBe(CelKind.NULL);
  });

  it('writes a BOOL CelValue', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('b', Repr.BOOL, 0)], { b: true });
    expect(kindAt(h.view, 0)).toBe(CelKind.BOOL);
    expect(h.view.getInt32(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(1);
  });

  it('writes an INT CelValue from a bigint', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('i', Repr.INT, 0)], { i: -42n });
    expect(kindAt(h.view, 0)).toBe(CelKind.INT);
    expect(h.view.getBigInt64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(-42n);
  });

  it('writes an INT CelValue from an integral number', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('i', Repr.INT, 0)], { i: 7 });
    expect(h.view.getBigInt64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(7n);
  });

  it('writes a UINT CelValue', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('u', Repr.UINT, 0)], {
      u: 18446744073709551615n,
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.UINT);
    expect(h.view.getBigUint64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(
      18446744073709551615n,
    );
  });

  it('writes a DOUBLE CelValue', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('d', Repr.DOUBLE, 0)], { d: 3.5 });
    expect(kindAt(h.view, 0)).toBe(CelKind.DOUBLE);
    expect(h.view.getFloat64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(3.5);
  });

  it('writes a STRING CelValue into the activation arena', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('s', Repr.STRING, 0)], { s: 'héllo' });
    expect(kindAt(h.view, 0)).toBe(CelKind.STRING);
    const ptr = h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET, true);
    const len = h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET + 4, true);
    expect(ptr).toBeGreaterThanOrEqual(ARENA_FLOOR);
    expect(new TextDecoder().decode(h.bytes.subarray(ptr, ptr + len))).toBe(
      'héllo',
    );
  });

  it('writes an empty STRING with a zero pointer and zero length', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('s', Repr.STRING, 0)], { s: '' });
    expect(kindAt(h.view, 0)).toBe(CelKind.STRING);
    expect(h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(0);
    expect(h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET + 4, true)).toBe(0);
  });

  it('writes a BYTES CelValue', () => {
    const h = makeHarness();
    const payload = Uint8Array.from([1, 2, 3]);
    marshalActivation(h.env, [variable('y', Repr.BYTES, 0)], { y: payload });
    expect(kindAt(h.view, 0)).toBe(CelKind.BYTES);
    const ptr = h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET, true);
    expect([...h.bytes.subarray(ptr, ptr + 3)]).toEqual([1, 2, 3]);
  });
});

// ── Aggregates ──────────────────────────────────────────────────────

describe('marshalActivation — aggregates', () => {
  it('interns an array as a host list (LIST_HOST slot)', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('l', Repr.LIST, 0)], {
      l: [1n, 2n, 3n],
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.LIST_HOST);
    const backing = h.refs.list.lookup(refAt(h.view, 0)) as HostListBacking;
    expect(backing.elements).toEqual([1n, 2n, 3n]);
  });

  it('interns a Map as a host map (MAP_HOST slot)', () => {
    const h = makeHarness();
    const map = new Map<CelInput, CelInput>([
      ['a', 1n],
      ['b', 2n],
    ]);
    marshalActivation(h.env, [variable('m', Repr.MAP, 0)], { m: map });
    expect(kindAt(h.view, 0)).toBe(CelKind.MAP_HOST);
    const backing = h.refs.map.lookup(refAt(h.view, 0)) as HostMapBacking;
    expect(backing.entries).toEqual([
      { key: 'a', value: 1n },
      { key: 'b', value: 2n },
    ]);
  });

  it('interns a plain object as a host map', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('m', Repr.MAP, 0)], {
      m: { x: true, y: false },
    });
    const backing = h.refs.map.lookup(refAt(h.view, 0)) as HostMapBacking;
    expect(backing.entries).toEqual([
      { key: 'x', value: true },
      { key: 'y', value: false },
    ]);
  });
});

// ── Message coercion (§A.4.6) ───────────────────────────────────────

const TEST_ROOT = protobuf.Root.fromJSON({
  nested: {
    test: {
      nested: {
        Person: {
          fields: {
            name: { type: 'string', id: 1 },
            age: { type: 'int32', id: 2 },
          },
        },
      },
    },
  },
});

describe('marshalActivation — message variables', () => {
  const types: TypeEntry[] = [{ id: 0, fullyQualifiedName: 'test.Person' }];

  it('coerces a plain object to a message backing (MESSAGE slot)', () => {
    const descriptors = DescriptorSet.fromRoot(TEST_ROOT);
    const h = makeHarness(descriptors, types);
    marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], {
      p: { name: 'Ada', age: 36 },
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.MESSAGE);
    const backing = h.refs.message.lookup(refAt(h.view, 0));
    expect(backing).toBeInstanceOf(ProtoMessageBacking);
    const proto = backing as ProtoMessageBacking;
    expect(proto.typeName).toBe('test.Person');
    expect(proto.readField('name')).toBe('Ada');
    expect(proto.readField('age')).toBe(36n);
  });

  it('uses a protobufjs message directly as its backing', () => {
    const descriptors = DescriptorSet.fromRoot(TEST_ROOT);
    const h = makeHarness(descriptors, types);
    const Person = TEST_ROOT.lookupType('test.Person');
    const msg = Person.create({ name: 'Grace', age: 45 });
    marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], { p: msg });
    const backing = h.refs.message.lookup(
      refAt(h.view, 0),
    ) as ProtoMessageBacking;
    expect(backing.readField('name')).toBe('Grace');
    expect(backing.readField('age')).toBe(45n);
  });

  it('throws when a message variable has no descriptors', () => {
    const h = makeHarness(undefined, types);
    expect(() => {
      marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], {
        p: { name: 'x' },
      });
    }).toThrow(CelMarshalError);
  });

  // A program that constructs no message literal interns only the null
  // sentinel ({id: 0, fqn: ''}) in its types table; a bound protobufjs
  // message must still back by its own `$type`, not the sentinel.
  it('backs a protobufjs message by its own $type when the types table is the null sentinel', () => {
    const descriptors = DescriptorSet.fromRoot(TEST_ROOT);
    const h = makeHarness(descriptors, [{ id: 0, fullyQualifiedName: '' }]);
    const Person = TEST_ROOT.lookupType('test.Person');
    const msg = Person.create({ name: 'Ada', age: 36 });
    marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], { p: msg });
    const backing = h.refs.message.lookup(
      refAt(h.view, 0),
    ) as ProtoMessageBacking;
    expect(backing.typeName).toBe('test.Person');
    expect(backing.readField('name')).toBe('Ada');
  });

  it('binds a protobufjs message even without descriptors ($type suffices)', () => {
    const h = makeHarness(undefined, []);
    const Person = TEST_ROOT.lookupType('test.Person');
    const msg = Person.create({ name: 'Grace', age: 45 });
    marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], { p: msg });
    const backing = h.refs.message.lookup(
      refAt(h.view, 0),
    ) as ProtoMessageBacking;
    expect(backing.readField('age')).toBe(45n);
  });

  it('skips the null sentinel when resolving a plain object via the types table', () => {
    const descriptors = DescriptorSet.fromRoot(TEST_ROOT);
    const h = makeHarness(descriptors, [
      { id: 0, fullyQualifiedName: '' },
      { id: 1, fullyQualifiedName: 'test.Person' },
    ]);
    marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], {
      p: { name: 'Ada', age: 36 },
    });
    const backing = h.refs.message.lookup(
      refAt(h.view, 0),
    ) as ProtoMessageBacking;
    expect(backing.typeName).toBe('test.Person');
    expect(backing.readField('age')).toBe(36n);
  });
});

// ── Wrapper-message peel at activation bind ─────────────────────────
// A wrapper-DECLARED variable carries the collapsed scalar repr
// (`compiler/ir/typed_ast.cc:56`), so binding the wrapper MESSAGE
// (`Int32Value{value: 5}`) peels the inner `value` field — the mirror
// of the C++ wrapper-coercion at activation bind
// (`TryEncodeWktWrapperMessage`, eval/instance.cc).  Pins the
// conformance `dynamic/<wrapper>/var` rows.

const WRAPPER_ROOT = protobuf.Root.fromJSON({
  nested: {
    google: {
      nested: {
        protobuf: {
          nested: {
            BoolValue: { fields: { value: { type: 'bool', id: 1 } } },
            Int32Value: { fields: { value: { type: 'int32', id: 1 } } },
            Int64Value: { fields: { value: { type: 'int64', id: 1 } } },
            UInt32Value: { fields: { value: { type: 'uint32', id: 1 } } },
            UInt64Value: { fields: { value: { type: 'uint64', id: 1 } } },
            FloatValue: { fields: { value: { type: 'float', id: 1 } } },
            DoubleValue: { fields: { value: { type: 'double', id: 1 } } },
            StringValue: { fields: { value: { type: 'string', id: 1 } } },
            BytesValue: { fields: { value: { type: 'bytes', id: 1 } } },
          },
        },
      },
    },
  },
});

function wrapperMsg(fqn: string, value: unknown): protobuf.Message {
  return WRAPPER_ROOT.lookupType(fqn).create({ value });
}

describe('marshalActivation — wrapper-message peel', () => {
  it('peels BoolValue into a BOOL slot', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('b', Repr.BOOL, 0)], {
      b: wrapperMsg('google.protobuf.BoolValue', true),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.BOOL);
    expect(h.view.getInt32(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(1);
  });

  it.each([
    ['google.protobuf.Int32Value', -123, -123n],
    ['google.protobuf.Int64Value', 2000000, 2000000n],
  ])('peels %s into an INT slot', (fqn, value, want) => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('i', Repr.INT, 0)], {
      i: wrapperMsg(fqn, value),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.INT);
    expect(h.view.getBigInt64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(want);
  });

  it.each([
    ['google.protobuf.UInt32Value', 4294967295, 4294967295n],
    ['google.protobuf.UInt64Value', 7, 7n],
  ])('peels %s into a UINT slot', (fqn, value, want) => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('u', Repr.UINT, 0)], {
      u: wrapperMsg(fqn, value),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.UINT);
    expect(h.view.getBigUint64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(want);
  });

  it('peels DoubleValue into a DOUBLE slot', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('d', Repr.DOUBLE, 0)], {
      d: wrapperMsg('google.protobuf.DoubleValue', 3.5),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.DOUBLE);
    expect(h.view.getFloat64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(3.5);
  });

  it('peels FloatValue with float32 narrowing', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('d', Repr.DOUBLE, 0)], {
      d: wrapperMsg('google.protobuf.FloatValue', 3.1416),
    });
    expect(h.view.getFloat64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(
      Math.fround(3.1416),
    );
  });

  it('peels StringValue into the activation arena', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('s', Repr.STRING, 0)], {
      s: wrapperMsg('google.protobuf.StringValue', 'wrapped'),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.STRING);
    const ptr = h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET, true);
    const len = h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET + 4, true);
    expect(new TextDecoder().decode(h.bytes.subarray(ptr, ptr + len))).toBe(
      'wrapped',
    );
  });

  it('peels BytesValue into a BYTES slot', () => {
    const h = makeHarness();
    marshalActivation(h.env, [variable('y', Repr.BYTES, 0)], {
      y: wrapperMsg('google.protobuf.BytesValue', Uint8Array.from([9, 8])),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.BYTES);
    const ptr = h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET, true);
    expect([...h.bytes.subarray(ptr, ptr + 2)]).toEqual([9, 8]);
  });

  it('rejects a wrapper from the WRONG scalar family', () => {
    const h = makeHarness();
    expect(() => {
      marshalActivation(h.env, [variable('b', Repr.BOOL, 0)], {
        b: wrapperMsg('google.protobuf.Int32Value', 1),
      });
    }).toThrow(CelMarshalError);
  });

  it('does NOT peel a wrapper bound to a MESSAGE-declared variable', () => {
    const types: TypeEntry[] = [{ id: 0, fullyQualifiedName: 'x' }];
    const h = makeHarness(DescriptorSet.fromRoot(WRAPPER_ROOT), types);
    marshalActivation(h.env, [variable('p', Repr.MESSAGE, 0)], {
      p: wrapperMsg('google.protobuf.Int32Value', 5),
    });
    expect(kindAt(h.view, 0)).toBe(CelKind.MESSAGE);
    const backing = h.refs.message.lookup(
      refAt(h.view, 0),
    ) as ProtoMessageBacking;
    expect(backing.typeName).toBe('google.protobuf.Int32Value');
  });

  it('still rejects a non-wrapper message bound to a scalar variable', () => {
    const h = makeHarness();
    const Person = TEST_ROOT.lookupType('test.Person');
    expect(() => {
      marshalActivation(h.env, [variable('i', Repr.INT, 0)], {
        i: Person.create({ name: 'x' }),
      });
    }).toThrow(CelMarshalError);
  });
});

// ── Errors / boundaries ─────────────────────────────────────────────

describe('marshalActivation — errors', () => {
  it('throws when a declared variable is unbound', () => {
    const h = makeHarness();
    expect(() => {
      marshalActivation(h.env, [variable('x', Repr.INT, 0)], {});
    }).toThrow(CelMarshalError);
  });

  it('throws on a repr/value type mismatch', () => {
    const h = makeHarness();
    expect(() => {
      marshalActivation(h.env, [variable('s', Repr.STRING, 0)], { s: 1n });
    }).toThrow(CelMarshalError);
  });

  it('marshals two variables into distinct slots', () => {
    const h = makeHarness();
    marshalActivation(
      h.env,
      [variable('x', Repr.INT, 0), variable('y', Repr.INT, CEL_VALUE_SIZE)],
      { x: 10n, y: 32n },
    );
    expect(h.view.getBigInt64(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(10n);
    expect(
      h.view.getBigInt64(CEL_VALUE_SIZE + CEL_VALUE_PAYLOAD_OFFSET, true),
    ).toBe(32n);
  });
});

describe('totalActivationBufferBytes', () => {
  it('sums string + bytes payloads padded to 8', () => {
    const vars = [
      variable('s', Repr.STRING, 0),
      variable('y', Repr.BYTES, CEL_VALUE_SIZE),
    ];
    const total = totalActivationBufferBytes(vars, {
      s: 'abc', // 3 bytes → 8
      y: Uint8Array.from([1, 2, 3, 4, 5]), // 5 bytes → 8
    });
    expect(total).toBe(16);
  });

  it('counts only string / bytes reprs', () => {
    const vars = [variable('i', Repr.INT, 0), variable('s', Repr.STRING, 24)];
    expect(totalActivationBufferBytes(vars, { i: 1n, s: 'hi' })).toBe(8);
  });

  it('is zero for an all-scalar activation', () => {
    const vars = [variable('i', Repr.INT, 0)];
    expect(totalActivationBufferBytes(vars, { i: 1n })).toBe(0);
  });
});

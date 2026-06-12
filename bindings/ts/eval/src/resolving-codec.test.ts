// Resolving codec — read/write CelValues that reference externref
// backings (MESSAGE / LIST_HOST / MAP_HOST), over a plain ArrayBuffer.

import * as protobuf from 'protobufjs';
import { describe, expect, it } from 'vitest';

import { ExternrefTable } from './externref.js';
import type { HostListBacking, HostMapBacking } from './host/aggregates.js';
import { ProtoMessageBacking } from './proto/backing.js';
import {
  encodeCelValue,
  internMessageBacking,
  resolveCelValue,
} from './resolving-codec.js';
import type { CodecEnv } from './resolving-codec.js';
import {
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CelKind,
} from './types.js';
import type { CelValue } from './types.js';

const BUFFER_BYTES = 4096;
const ARENA_FLOOR = 1024;

interface Harness {
  readonly env: CodecEnv;
  readonly view: DataView;
  readonly refs: ExternrefTable;
}

function makeHarness(): Harness {
  const buffer = new ArrayBuffer(BUFFER_BYTES);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const refs = new ExternrefTable();
  let cursor = ARENA_FLOOR;
  const env: CodecEnv = {
    view: () => view,
    bytes: () => bytes,
    refs,
    arenaAlloc: (n: number) => {
      const ptr = cursor;
      cursor += (n + 7) & ~7;
      return ptr;
    },
  };
  return { env, view, refs };
}

function stampHandle(
  view: DataView,
  slot: number,
  kind: CelKind,
  ref: number,
): void {
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ref, true);
}

// ── resolveCelValue (read) ──────────────────────────────────────────

describe('resolveCelValue — externref kinds', () => {
  it('resolves a LIST_HOST slot to a JS array', () => {
    const h = makeHarness();
    const backing: HostListBacking = { elements: [1n, 'a', true] };
    const ref = h.refs.list.intern(backing);
    stampHandle(h.view, 0, CelKind.LIST_HOST, ref);
    expect(resolveCelValue(h.env, 0)).toEqual([1n, 'a', true]);
  });

  it('resolves a MAP_HOST slot to a JS Map', () => {
    const h = makeHarness();
    const backing: HostMapBacking = {
      entries: [
        { key: 'a', value: 1n },
        { key: 'b', value: 2n },
      ],
    };
    const ref = h.refs.map.intern(backing);
    stampHandle(h.view, 0, CelKind.MAP_HOST, ref);
    const out = resolveCelValue(h.env, 0);
    expect(out).toBeInstanceOf(Map);
    expect((out as Map<CelValue, CelValue>).get('a')).toBe(1n);
    expect((out as Map<CelValue, CelValue>).get('b')).toBe(2n);
  });

  it('resolves a MESSAGE slot to its decoded object', () => {
    const h = makeHarness();
    const root = protobuf.Root.fromJSON({
      nested: {
        Pt: {
          fields: { x: { type: 'int32', id: 1 }, y: { type: 'int32', id: 2 } },
        },
      },
    });
    const Pt = root.lookupType('Pt');
    const backing = new ProtoMessageBacking(Pt, Pt.create({ x: 3, y: 4 }));
    const ref = h.refs.message.intern(backing);
    stampHandle(h.view, 0, CelKind.MESSAGE, ref);
    expect(resolveCelValue(h.env, 0)).toEqual({ x: 3n, y: 4n });
  });

  it('resolves an empty LIST_HOST (wild slot) to an empty array', () => {
    const h = makeHarness();
    stampHandle(h.view, 0, CelKind.LIST_HOST, 999);
    expect(resolveCelValue(h.env, 0)).toEqual([]);
  });

  it('resolves a wild MESSAGE slot to null', () => {
    const h = makeHarness();
    stampHandle(h.view, 0, CelKind.MESSAGE, 999);
    expect(resolveCelValue(h.env, 0)).toBeNull();
  });

  it('delegates an INT slot to the plain codec', () => {
    const h = makeHarness();
    h.view.setUint32(CEL_VALUE_KIND_OFFSET, CelKind.INT, true);
    h.view.setBigInt64(CEL_VALUE_PAYLOAD_OFFSET, 42n, true);
    expect(resolveCelValue(h.env, 0)).toBe(42n);
  });
});

// ── encodeCelValue (write) ──────────────────────────────────────────

describe('encodeCelValue — scalars round-trip', () => {
  it.each<[string, CelValue]>([
    ['null', null],
    ['bool', true],
    ['int', -7n],
    ['double', 2.5],
    ['string', 'héllo'],
    ['bytes', Uint8Array.from([9, 8, 7])],
  ])('round-trips %s', (_name, value) => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, value);
    const out = resolveCelValue(h.env, 0);
    if (value instanceof Uint8Array) {
      expect([...(out as Uint8Array)]).toEqual([...value]);
    } else {
      expect(out).toEqual(value);
    }
  });

  it('encodes a timestamp record', () => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, { kind: 'timestamp', epochSeconds: 5n, nanos: 6 });
    expect(resolveCelValue(h.env, 0)).toEqual({
      kind: 'timestamp',
      epochSeconds: 5n,
      nanos: 6,
    });
  });

  it('encodes a duration record', () => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, { kind: 'duration', seconds: 9n, nanos: 1 });
    expect(resolveCelValue(h.env, 0)).toEqual({
      kind: 'duration',
      seconds: 9n,
      nanos: 1,
    });
  });

  it('encodes a type record as a CEL_TYPE span (not a host map)', () => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, { kind: 'type', name: 'int' });
    expect(h.view.getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(CelKind.TYPE);
    expect(resolveCelValue(h.env, 0)).toEqual({ kind: 'type', name: 'int' });
  });

  it('encodes a message-FQN type record', () => {
    const h = makeHarness();
    const fqn = 'google.protobuf.Duration';
    encodeCelValue(h.env, 0, { kind: 'type', name: fqn });
    expect(resolveCelValue(h.env, 0)).toEqual({ kind: 'type', name: fqn });
  });

  it('encodes an error record', () => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, {
      kind: 'error',
      code: 11,
      message: 'divide by zero',
    });
    const out = resolveCelValue(h.env, 0) as { kind: string; code: number };
    expect(out.kind).toBe('error');
    expect(out.code).toBe(11);
  });
});

describe('encodeCelValue — aggregates intern + round-trip', () => {
  it('encodes an array as a host list and reads it back', () => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, [1n, 2n, 3n]);
    expect(h.view.getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.LIST_HOST,
    );
    expect(resolveCelValue(h.env, 0)).toEqual([1n, 2n, 3n]);
  });

  it('encodes a Map as a host map and reads it back', () => {
    const h = makeHarness();
    const map = new Map<CelValue, CelValue>([['k', 1n]]);
    encodeCelValue(h.env, 0, map);
    expect(h.view.getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(
      CelKind.MAP_HOST,
    );
    const out = resolveCelValue(h.env, 0) as Map<CelValue, CelValue>;
    expect(out.get('k')).toBe(1n);
  });

  it('encodes a plain (message-shaped) object as a host map', () => {
    const h = makeHarness();
    encodeCelValue(h.env, 0, { a: 1n, b: 'x' });
    const out = resolveCelValue(h.env, 0) as Map<CelValue, CelValue>;
    expect(out.get('a')).toBe(1n);
    expect(out.get('b')).toBe('x');
  });
});

// ── internMessageBacking (the CEL_MESSAGE write seam) ───────────────

describe('internMessageBacking — message intern + stamp', () => {
  it('interns the backing and stamps a CEL_MESSAGE handle', () => {
    // Mirrors HostCallContext::ReturnProto (eval/host_call_context.cc:549):
    // kind 10, ref_slot payload pointing at the externref message table.
    const h = makeHarness();
    const root = protobuf.Root.fromJSON({
      nested: {
        Pt: {
          fields: { x: { type: 'int32', id: 1 }, y: { type: 'int32', id: 2 } },
        },
      },
    });
    const Pt = root.lookupType('Pt');
    const backing = new ProtoMessageBacking(Pt, Pt.create({ x: 3, y: 4 }));
    internMessageBacking(h.env, 0, backing);
    expect(h.view.getUint32(CEL_VALUE_KIND_OFFSET, true)).toBe(CelKind.MESSAGE);
    // First intern lands at slot 1 (slot 0 is the null sentinel) and the
    // table resolves back to the SAME backing object.
    expect(h.view.getUint32(CEL_VALUE_PAYLOAD_OFFSET, true)).toBe(1);
    expect(h.refs.message.lookup(1)).toBe(backing);
    expect(resolveCelValue(h.env, 0)).toEqual({ x: 3n, y: 4n });
  });
});

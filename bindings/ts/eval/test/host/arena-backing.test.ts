/**
 * `arena-backing.ts` — the recursive `decodeValueAt` + the lazy
 * `ArenaListBacking` / `ArenaMapBacking` that read the runtime's
 * `ArenaListHeader` / `ArenaMapHeader` out of linear memory. Hermetic: we
 * hand-stamp the exact byte layout (cel_data.h §4.1/§4.2) into a real
 * `WebAssembly.Memory` — no wasm module — and assert the decode, so every
 * arm/branch is covered without the runtime.
 */
import { describe, it, expect } from 'vitest';
import { CelKind, type CelValue } from '../../src/celvalue.js';
import { ExternrefTable } from '../../src/externref.js';
import {
  ArenaDecodeError,
  ArenaListBacking,
  ArenaMapBacking,
  decodeValueAt,
  type DecodeContext,
} from '../../src/host/arena-backing.js';
import type {
  ListBacking,
  MapBacking,
  MessageBacking,
} from '../../src/host/backing.js';
import { Value } from '../../src/value.js';

const CV = 24; // sizeof(CelValue)

// A little memory-stamper: lays CelValues / headers at chosen offsets and
// bump-allocates span bytes. Mirrors what the runtime would write.
class Mem {
  public readonly memory = new WebAssembly.Memory({ initial: 1 });
  public readonly refs = new ExternrefTable<
    MessageBacking,
    MapBacking,
    ListBacking
  >();
  private readonly dv = new DataView(this.memory.buffer);
  private spanCursor = 4096;

  public ctx(): DecodeContext {
    return { memory: this.memory, refs: this.refs };
  }

  public scalar(off: number, v: CelValue): void {
    this.dv.setUint32(off, v.kind, true);
    this.dv.setUint32(off + 4, 0, true);
    switch (v.kind) {
      case CelKind.Null:
        break;
      case CelKind.Bool:
        this.dv.setInt32(off + 8, v.bool ? 1 : 0, true);
        break;
      case CelKind.Int:
        this.dv.setBigInt64(off + 8, v.int, true);
        break;
      case CelKind.Uint:
        this.dv.setBigUint64(off + 8, v.uint, true);
        break;
      case CelKind.Double:
        this.dv.setFloat64(off + 8, v.double, true);
        break;
      case CelKind.String: {
        const bytes = new TextEncoder().encode(v.value);
        const ptr = this.spanCursor;
        this.spanCursor += bytes.length;
        new Uint8Array(this.memory.buffer).set(bytes, ptr);
        this.dv.setUint32(off + 8, ptr, true);
        this.dv.setUint32(off + 12, bytes.length, true);
        break;
      }
      default:
        throw new Error(`scalar() does not stamp kind ${v.kind}`);
    }
  }

  public ref(off: number, kind: number, slot: number): void {
    this.dv.setUint32(off, kind, true);
    this.dv.setUint32(off + 4, 0, true);
    this.dv.setUint32(off + 8, slot, true);
  }

  public unknown(off: number): void {
    this.dv.setUint32(off, CelKind.Unknown, true);
  }

  public listHeader(hdrPtr: number, count: number, elementsOff: number): void {
    this.dv.setUint32(hdrPtr, count, true);
    this.dv.setUint32(hdrPtr + 4, count, true); // capacity
    this.dv.setUint32(hdrPtr + 8, elementsOff, true);
    this.dv.setUint32(hdrPtr + 12, 0, true);
  }

  public mapHeader(hdrPtr: number, count: number, entriesOff: number): void {
    this.dv.setUint32(hdrPtr, count, true);
    this.dv.setUint32(hdrPtr + 4, count, true);
    this.dv.setUint32(hdrPtr + 8, entriesOff, true);
    this.dv.setUint32(hdrPtr + 12, 0, true);
  }

  // Stamp a CEL_LIST_ARENA CelValue at `off` pointing at a freshly-laid
  // header+elements; returns nothing (caller fills elements).
  public listValueAt(off: number, hdrPtr: number): void {
    this.ref(off, CelKind.ListArena, hdrPtr);
  }
  public mapValueAt(off: number, hdrPtr: number): void {
    this.ref(off, CelKind.MapArena, hdrPtr);
  }
}

// A trivial MessageBacking so an interned slot resolves.
const FAKE_MSG: MessageBacking = {
  getField: () => ({
    host: 'scalar',
    value: { kind: CelKind.String, value: 'x' },
  }),
  hasField: () => true,
};

describe('decodeValueAt — per-kind dispatch', () => {
  it('decodes scalars + error via the codec', () => {
    const m = new Mem();
    m.scalar(0, Value.int(42n));
    expect(decodeValueAt(m.ctx(), 0)).toEqual(Value.int(42n));
    m.scalar(64, { kind: CelKind.Null });
    expect(decodeValueAt(m.ctx(), 64)).toEqual(Value.null());
  });

  it('resolves a CEL_MESSAGE element via the externref table', () => {
    const m = new Mem();
    const slot = m.refs.internMessage(FAKE_MSG);
    m.ref(0, CelKind.Message, slot);
    const v = decodeValueAt(m.ctx(), 0);
    expect(v.kind).toBe(CelKind.Message);
  });

  it('resolves CEL_LIST_HOST and CEL_MAP_HOST elements', () => {
    const m = new Mem();
    const lslot = m.refs.internList({
      size: 0,
      at: () => undefined,
      forEach: () => undefined,
    });
    const mslot = m.refs.internMap({
      size: 0,
      get: () => undefined,
      has: () => false,
      forEach: () => undefined,
    });
    m.ref(0, CelKind.ListHost, lslot);
    m.ref(64, CelKind.MapHost, mslot);
    expect(decodeValueAt(m.ctx(), 0).kind).toBe(CelKind.ListHost);
    expect(decodeValueAt(m.ctx(), 64).kind).toBe(CelKind.MapHost);
  });

  it('decodes UNKNOWN as the sentinel', () => {
    const m = new Mem();
    m.unknown(0);
    expect(decodeValueAt(m.ctx(), 0)).toEqual(Value.unknown());
  });

  it('throws on a dangling message / list / map slot', () => {
    const m = new Mem();
    m.ref(0, CelKind.Message, 99);
    m.ref(64, CelKind.ListHost, 99);
    m.ref(128, CelKind.MapHost, 99);
    expect(() => decodeValueAt(m.ctx(), 0)).toThrow(ArenaDecodeError);
    expect(() => decodeValueAt(m.ctx(), 64)).toThrow(/list slot 99/);
    expect(() => decodeValueAt(m.ctx(), 128)).toThrow(/map slot 99/);
  });
});

describe('ArenaListBacking', () => {
  // Build [int 1, "hi", null] at header 256, elements at 512.
  function listOfThree(): { m: Mem; hdr: number } {
    const m = new Mem();
    const hdr = 256;
    const elems = 512;
    m.listHeader(hdr, 3, elems);
    m.scalar(elems + 0 * CV, Value.int(1n));
    m.scalar(elems + 1 * CV, { kind: CelKind.String, value: 'hi' });
    m.scalar(elems + 2 * CV, { kind: CelKind.Null });
    return { m, hdr };
  }

  it('size + in-range element reads (scalar / string / null)', () => {
    const { m, hdr } = listOfThree();
    const b = new ArenaListBacking(m.ctx(), hdr);
    expect(b.size).toBe(3);
    expect(b.at(0)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 1n },
    });
    expect(b.at(1)).toEqual({
      host: 'scalar',
      value: { kind: CelKind.String, value: 'hi' },
    });
    expect(b.at(2)).toEqual({ host: 'scalar', value: { kind: CelKind.Null } });
  });

  it('out-of-range / negative index → undefined', () => {
    const { m, hdr } = listOfThree();
    const b = new ArenaListBacking(m.ctx(), hdr);
    expect(b.at(3)).toBeUndefined();
    expect(b.at(-1)).toBeUndefined();
  });

  it('forEach visits every element in order', () => {
    const { m, hdr } = listOfThree();
    const kinds: number[] = [];
    new ArenaListBacking(m.ctx(), hdr).forEach((e) => {
      kinds.push(e.host === 'scalar' ? e.value.kind : -1);
    });
    expect(kinds).toEqual([CelKind.Int, CelKind.String, CelKind.Null]);
  });

  it('decodes a list element that is a protobuf MESSAGE', () => {
    const m = new Mem();
    const slot = m.refs.internMessage(FAKE_MSG);
    m.listHeader(256, 1, 512);
    m.ref(512, CelKind.Message, slot);
    const b = new ArenaListBacking(m.ctx(), 256);
    const e = b.at(0);
    expect(e?.host).toBe('message');
  });

  it('recurses into a NESTED arena list element', () => {
    const m = new Mem();
    // outer list [ [int 7] ] : outer hdr 256 / elems 512; inner hdr 800 / elems 900
    m.listHeader(256, 1, 512);
    m.listValueAt(512, 800);
    m.listHeader(800, 1, 900);
    m.scalar(900, Value.int(7n));
    const outer = new ArenaListBacking(m.ctx(), 256);
    const inner = outer.at(0);
    expect(inner?.host).toBe('list');
    if (inner?.host === 'list') {
      expect(inner.backing.at(0)).toEqual({
        host: 'scalar',
        value: { kind: CelKind.Int, int: 7n },
      });
    }
  });
});

describe('ArenaMapBacking', () => {
  // Build { "k": int 7, 2: int 8 } at header 256, entries at 512 (48 B each).
  function mapOfTwo(): { m: Mem; hdr: number } {
    const m = new Mem();
    const hdr = 256;
    const entries = 512;
    m.mapHeader(hdr, 2, entries);
    m.scalar(entries + 0 * 48, { kind: CelKind.String, value: 'k' });
    m.scalar(entries + 0 * 48 + CV, Value.int(7n));
    m.scalar(entries + 1 * 48, Value.int(2n));
    m.scalar(entries + 1 * 48 + CV, Value.int(8n));
    return { m, hdr };
  }

  it('size + get by string and (cross-type) numeric key', () => {
    const { m, hdr } = mapOfTwo();
    const b = new ArenaMapBacking(m.ctx(), hdr);
    expect(b.size).toBe(2);
    expect(b.get({ kind: CelKind.String, value: 'k' })).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 7n },
    });
    // int key 2 matches via a uint 2 lookup (cross-type numeric equality).
    expect(b.get({ kind: CelKind.Uint, uint: 2n })).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 8n },
    });
  });

  it('get miss → undefined; has reflects presence', () => {
    const { m, hdr } = mapOfTwo();
    const b = new ArenaMapBacking(m.ctx(), hdr);
    expect(b.get({ kind: CelKind.String, value: 'absent' })).toBeUndefined();
    expect(b.has({ kind: CelKind.String, value: 'k' })).toBe(true);
    expect(b.has({ kind: CelKind.String, value: 'absent' })).toBe(false);
  });

  it('forEach visits key/value pairs', () => {
    const { m, hdr } = mapOfTwo();
    const keys: unknown[] = [];
    new ArenaMapBacking(m.ctx(), hdr).forEach((k) => {
      if (k.kind === CelKind.String) keys.push(`s:${k.value}`);
      else if (k.kind === CelKind.Int) keys.push(`i:${k.int}`);
    });
    expect(keys).toEqual(['s:k', 'i:2']);
  });

  it('map value may itself be a message (value recurses)', () => {
    const m = new Mem();
    const slot = m.refs.internMessage(FAKE_MSG);
    m.mapHeader(256, 1, 512);
    m.scalar(512, { kind: CelKind.String, value: 'k' });
    m.ref(512 + CV, CelKind.Message, slot);
    const b = new ArenaMapBacking(m.ctx(), 256);
    expect(b.get({ kind: CelKind.String, value: 'k' })?.host).toBe('message');
  });

  it('decodeValueAt resolves a top-level arena map/list value', () => {
    const { m, hdr } = mapOfTwo();
    m.mapValueAt(0, hdr);
    const v = decodeValueAt(m.ctx(), 0);
    expect(v.kind).toBe(CelKind.MapHost);
  });
});

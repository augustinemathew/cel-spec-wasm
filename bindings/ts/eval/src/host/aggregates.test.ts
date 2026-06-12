// Unit tests for the cel_host list/map aggregate trampolines.
//
// Each test hand-builds linear memory (a `DataView` over an
// `ArrayBuffer`), an `ExternrefTable` with hand-built host backings, and
// a fake `arenaAlloc` bump allocator, then drives a single trampoline
// and asserts the CelValue written to the out slot.  Spec errors
// (NO_SUCH_KEY / INDEX_OUT_OF_BOUNDS / TYPE_MISMATCH) are CEL_ERROR
// VALUES, never thrown; 3VL absorption copies a poisoned operand to out.

import { describe, expect, it } from 'vitest';

import { ExternrefTable } from '../externref.js';
import {
  ARENA_HEADER_COUNT_OFFSET,
  ARENA_HEADER_DATA_OFFSET,
  ARENA_HEADER_SIZE,
  ARENA_LIST_ELEMENT_STRIDE,
  ARENA_MAP_ENTRY_STRIDE,
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CEL_VALUE_SIZE,
  CelErrorCode,
  CelKind,
} from '../types.js';
import type { CelValue } from '../types.js';

import {
  celValueEquals,
  makeAggregateTrampolines,
  type AggregateContext,
  type HostListBacking,
  type HostMapBacking,
} from './aggregates.js';

// ───────────────────────────────────────────────────────────────────
// Test harness — a fake AggregateContext over a fixed linear memory.
//
// Slot layout: the harness reserves a fixed grid of 24-byte CelValue
// slots at the bottom of memory (slot 0 at offset 0, slot 1 at 24, …)
// for operands + the out slot; the arena bump allocator hands out bytes
// from `ARENA_BASE` upward for iter_open / concat snapshots.
//
// `readValue` / `writeValue` are a minimal codec the assembly WI owns in
// production: scalars + null + time records round-trip through linear
// memory; host list / map / message values round-trip through the
// externref slot stamped in the payload (so a snapshot the trampoline
// writes via `writeValue` decodes back to the same backing).  Strings /
// bytes round-trip through the arena.
// ───────────────────────────────────────────────────────────────────

const MEM_BYTES = 1 << 16; // 64 KiB
const ARENA_BASE = 4096; // operands live below; arena above.

interface Harness {
  readonly ctx: AggregateContext;
  /** The absolute byte offset of CelValue slot `n` (24-byte stride). */
  slot(n: number): number;
  /** Write a CelValue into slot `n`. */
  put(n: number, value: CelValue): void;
  /** Read the CelValue at slot `n`. */
  get(n: number): CelValue;
  /** Read the raw `kind` u32 at slot `n`. */
  kindAt(n: number): number;
  /** Read the raw payload u32 at slot `n`. */
  payloadU32At(n: number): number;
  /** Intern a host list, returning its externref slot. */
  internList(backing: HostListBacking): number;
  /** Intern a host map, returning its externref slot. */
  internMap(backing: HostMapBacking): number;
  /** Read the 16-byte MapIterState at `offset` → its four u32 fields. */
  readIterState(offset: number): {
    kind: number;
    cursor: number;
    payload: number;
    count: number;
  };
  /** Decode an arena LIST_ARENA CelValue at slot `n` → its elements. */
  readArenaList(n: number): CelValue[];
}

function makeHarness(): Harness {
  const buffer = new ArrayBuffer(MEM_BYTES);
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  const refs = new ExternrefTable();
  let arenaTop = ARENA_BASE;

  const UTF8 = new TextEncoder();
  const UTF8D = new TextDecoder();

  const arenaAlloc = (n: number): number => {
    const off = arenaTop;
    arenaTop += n;
    // 8-byte align the next allocation.
    arenaTop = (arenaTop + 7) & ~7;
    return off;
  };

  // Minimal codec: write any CelValue into linear memory; host
  // aggregates intern into refs and stamp the slot in the payload.
  const writeValue = (off: number, value: CelValue): void => {
    // Zero the 24 bytes first.
    for (let i = 0; i < CEL_VALUE_SIZE; i += 4) {
      view.setUint32(off + i, 0, true);
    }
    if (value === null) {
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.NULL, true);
      return;
    }
    if (typeof value === 'boolean') {
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.BOOL, true);
      view.setInt32(off + CEL_VALUE_PAYLOAD_OFFSET, value ? 1 : 0, true);
      return;
    }
    if (typeof value === 'bigint') {
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.INT, true);
      view.setBigInt64(off + CEL_VALUE_PAYLOAD_OFFSET, value, true);
      return;
    }
    if (typeof value === 'number') {
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.DOUBLE, true);
      view.setFloat64(off + CEL_VALUE_PAYLOAD_OFFSET, value, true);
      return;
    }
    if (typeof value === 'string') {
      const enc = UTF8.encode(value);
      const ptr = arenaAlloc(enc.length === 0 ? 1 : enc.length);
      bytes.set(enc, ptr);
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.STRING, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET, ptr, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET + 4, enc.length, true);
      return;
    }
    if (value instanceof Uint8Array) {
      const ptr = arenaAlloc(value.length === 0 ? 1 : value.length);
      bytes.set(value, ptr);
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.BYTES, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET, ptr, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET + 4, value.length, true);
      return;
    }
    if (Array.isArray(value)) {
      const slot = refs.list.intern({ elements: value });
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.LIST_HOST, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET, slot, true);
      return;
    }
    if (value instanceof Map) {
      const entries = [...value.entries()].map(([key, v]) => ({
        key,
        value: v,
      }));
      const slot = refs.map.intern({ entries });
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.MAP_HOST, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET, slot, true);
      return;
    }
    if (value.kind === 'error') {
      // CelValue's message member ({[k]:CelValue}) defeats discriminant
      // narrowing, so bind the tagged member explicitly.
      const e = value as Extract<CelValue, { kind: 'error' }>;
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.ERROR, true);
      view.setUint32(off + CEL_VALUE_PAYLOAD_OFFSET, e.code, true);
      return;
    }
    if (value.kind === 'timestamp') {
      const ts = value as Extract<CelValue, { kind: 'timestamp' }>;
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.TIMESTAMP, true);
      view.setBigInt64(off + CEL_VALUE_PAYLOAD_OFFSET, ts.epochSeconds, true);
      view.setInt32(off + CEL_VALUE_PAYLOAD_OFFSET + 8, ts.nanos, true);
      return;
    }
    if (value.kind === 'duration') {
      const d = value as Extract<CelValue, { kind: 'duration' }>;
      view.setUint32(off + CEL_VALUE_KIND_OFFSET, CelKind.DURATION, true);
      view.setBigInt64(off + CEL_VALUE_PAYLOAD_OFFSET, d.seconds, true);
      view.setInt32(off + CEL_VALUE_PAYLOAD_OFFSET + 8, d.nanos, true);
      return;
    }
    throw new Error('test codec: unsupported CelValue shape');
  };

  const readValue = (off: number): CelValue => {
    const kind = view.getUint32(off + CEL_VALUE_KIND_OFFSET, true) as CelKind;
    const p = off + CEL_VALUE_PAYLOAD_OFFSET;
    switch (kind) {
      case CelKind.NULL:
        return null;
      case CelKind.BOOL:
        return view.getInt32(p, true) !== 0;
      case CelKind.INT:
        return view.getBigInt64(p, true);
      case CelKind.UINT:
        return view.getBigUint64(p, true);
      case CelKind.DOUBLE:
        return view.getFloat64(p, true);
      case CelKind.STRING: {
        const ptr = view.getUint32(p, true);
        const len = view.getUint32(p + 4, true);
        return UTF8D.decode(bytes.subarray(ptr, ptr + len));
      }
      case CelKind.BYTES: {
        const ptr = view.getUint32(p, true);
        const len = view.getUint32(p + 4, true);
        return bytes.slice(ptr, ptr + len);
      }
      case CelKind.LIST_HOST: {
        const slot = view.getUint32(p, true);
        const b = refs.list.lookup(slot) as HostListBacking | undefined;
        return b === undefined ? [] : [...b.elements];
      }
      case CelKind.MAP_HOST: {
        const slot = view.getUint32(p, true);
        const b = refs.map.lookup(slot) as HostMapBacking | undefined;
        const m = new Map<CelValue, CelValue>();
        if (b !== undefined) {
          for (const e of b.entries) m.set(e.key, e.value);
        }
        return m;
      }
      case CelKind.LIST_ARENA: {
        const headerPtr = view.getUint32(p, true);
        const count = view.getUint32(
          headerPtr + ARENA_HEADER_COUNT_OFFSET,
          true,
        );
        const elems = view.getUint32(
          headerPtr + ARENA_HEADER_DATA_OFFSET,
          true,
        );
        const out: CelValue[] = [];
        for (let i = 0; i < count; i++) {
          out.push(readValue(elems + i * ARENA_LIST_ELEMENT_STRIDE));
        }
        return out;
      }
      case CelKind.MAP_ARENA: {
        const headerPtr = view.getUint32(p, true);
        const count = view.getUint32(
          headerPtr + ARENA_HEADER_COUNT_OFFSET,
          true,
        );
        const entries = view.getUint32(
          headerPtr + ARENA_HEADER_DATA_OFFSET,
          true,
        );
        const m = new Map<CelValue, CelValue>();
        for (let i = 0; i < count; i++) {
          const off = entries + i * ARENA_MAP_ENTRY_STRIDE;
          m.set(readValue(off), readValue(off + CEL_VALUE_SIZE));
        }
        return m;
      }
      case CelKind.ERROR:
        return { kind: 'error', code: view.getUint32(p, true), message: '' };
      case CelKind.TIMESTAMP:
        return {
          kind: 'timestamp',
          epochSeconds: view.getBigInt64(p, true),
          nanos: view.getInt32(p + 8, true),
        };
      case CelKind.DURATION:
        return {
          kind: 'duration',
          seconds: view.getBigInt64(p, true),
          nanos: view.getInt32(p + 8, true),
        };
      default:
        throw new Error(`test codec: cannot decode kind ${String(kind)}`);
    }
  };

  const ctx: AggregateContext = {
    view: () => view,
    bytes: () => bytes,
    refs,
    readValue,
    writeValue,
    arenaAlloc,
  };

  const slot = (n: number): number => n * CEL_VALUE_SIZE;

  return {
    ctx,
    slot,
    put: (n, value) => {
      writeValue(slot(n), value);
    },
    get: (n) => readValue(slot(n)),
    kindAt: (n) => view.getUint32(slot(n) + CEL_VALUE_KIND_OFFSET, true),
    payloadU32At: (n) =>
      view.getUint32(slot(n) + CEL_VALUE_PAYLOAD_OFFSET, true),
    internList: (backing) => refs.list.intern(backing),
    internMap: (backing) => refs.map.intern(backing),
    readIterState: (offset) => ({
      kind: view.getUint32(offset + 0, true),
      cursor: view.getUint32(offset + 4, true),
      payload: view.getUint32(offset + 8, true),
      count: view.getUint32(offset + 12, true),
    }),
    readArenaList: (n) => {
      const headerPtr = view.getUint32(
        slot(n) + CEL_VALUE_PAYLOAD_OFFSET,
        true,
      );
      const count = view.getUint32(headerPtr + 0, true);
      const elementsOff = view.getUint32(headerPtr + 8, true);
      const out: CelValue[] = [];
      for (let i = 0; i < count; i++) {
        out.push(readValue(elementsOff + i * CEL_VALUE_SIZE));
      }
      return out;
    },
  };
}

/**
 * Stamp a LIST_HOST CelValue (referencing externref `refSlot`) into
 * harness slot `n` directly — the trampolines only read the kind +
 * ref_slot off the slot, so we don't need the codec's intern path.
 */
function putListHost(h: Harness, n: number, refSlot: number): void {
  const view = h.ctx.view();
  view.setUint32(h.slot(n) + CEL_VALUE_KIND_OFFSET, CelKind.LIST_HOST, true);
  view.setUint32(h.slot(n) + CEL_VALUE_PAYLOAD_OFFSET, refSlot, true);
}

/** Stamp a MAP_HOST CelValue (referencing externref `refSlot`) into slot `n`. */
function putMapHost(h: Harness, n: number, refSlot: number): void {
  const view = h.ctx.view();
  view.setUint32(h.slot(n) + CEL_VALUE_KIND_OFFSET, CelKind.MAP_HOST, true);
  view.setUint32(h.slot(n) + CEL_VALUE_PAYLOAD_OFFSET, refSlot, true);
}

/**
 * Materialise `elements` as an arena LIST_ARENA (16-byte header + a 24-byte
 * CelValue run, `cel_data.h:90-102`) and stamp the CelValue into slot `n` —
 * the literal-list operand shape the mixed arena/host equality bridge reads.
 */
function putArenaList(
  h: Harness,
  n: number,
  elements: readonly CelValue[],
): void {
  const view = h.ctx.view();
  const headerPtr = h.ctx.arenaAlloc(ARENA_HEADER_SIZE);
  const elemsPtr = h.ctx.arenaAlloc(
    Math.max(elements.length * ARENA_LIST_ELEMENT_STRIDE, 1),
  );
  view.setUint32(headerPtr + ARENA_HEADER_COUNT_OFFSET, elements.length, true);
  view.setUint32(headerPtr + ARENA_HEADER_DATA_OFFSET, elemsPtr, true);
  elements.forEach((e, i) => {
    h.ctx.writeValue(elemsPtr + i * ARENA_LIST_ELEMENT_STRIDE, e);
  });
  view.setUint32(h.slot(n) + CEL_VALUE_KIND_OFFSET, CelKind.LIST_ARENA, true);
  view.setUint32(h.slot(n) + CEL_VALUE_PAYLOAD_OFFSET, headerPtr, true);
}

/**
 * Materialise `entries` as an arena MAP_ARENA (16-byte header + 48-byte
 * key/value entry run, `cel_data.h:64-83`) and stamp the CelValue into
 * slot `n`.
 */
function putArenaMap(
  h: Harness,
  n: number,
  entries: readonly [CelValue, CelValue][],
): void {
  const view = h.ctx.view();
  const headerPtr = h.ctx.arenaAlloc(ARENA_HEADER_SIZE);
  const entriesPtr = h.ctx.arenaAlloc(
    Math.max(entries.length * ARENA_MAP_ENTRY_STRIDE, 1),
  );
  view.setUint32(headerPtr + ARENA_HEADER_COUNT_OFFSET, entries.length, true);
  view.setUint32(headerPtr + ARENA_HEADER_DATA_OFFSET, entriesPtr, true);
  entries.forEach(([k, v], i) => {
    const off = entriesPtr + i * ARENA_MAP_ENTRY_STRIDE;
    h.ctx.writeValue(off, k);
    h.ctx.writeValue(off + CEL_VALUE_SIZE, v);
  });
  view.setUint32(h.slot(n) + CEL_VALUE_KIND_OFFSET, CelKind.MAP_ARENA, true);
  view.setUint32(h.slot(n) + CEL_VALUE_PAYLOAD_OFFSET, headerPtr, true);
}

/** Stamp a poison CelValue (ERROR with `code`, or UNKNOWN) into slot `n`. */
function putError(h: Harness, n: number, code: CelErrorCode): void {
  const view = h.ctx.view();
  view.setUint32(h.slot(n) + CEL_VALUE_KIND_OFFSET, CelKind.ERROR, true);
  view.setUint32(h.slot(n) + CEL_VALUE_PAYLOAD_OFFSET, code, true);
}

function putUnknown(h: Harness, n: number): void {
  const view = h.ctx.view();
  view.setUint32(h.slot(n) + CEL_VALUE_KIND_OFFSET, CelKind.UNKNOWN, true);
  view.setUint32(h.slot(n) + CEL_VALUE_PAYLOAD_OFFSET, 0, true);
}

/** Assert the CelValue at slot `n` is a CEL_ERROR with `code`. */
function expectError(h: Harness, n: number, code: CelErrorCode): void {
  expect(h.kindAt(n)).toBe(CelKind.ERROR);
  expect(h.payloadU32At(n)).toBe(code);
}

// Convenience: trampoline table over a fresh harness.
function trampolines(h: Harness): Record<string, (...args: number[]) => void> {
  return makeAggregateTrampolines(h.ctx);
}

// ───────────────────────────────────────────────────────────────────
// celValueEquals — the equality primitive the in/eq paths build on.
// ───────────────────────────────────────────────────────────────────
describe('celValueEquals', () => {
  it('compares same-kind scalars structurally', () => {
    expect(celValueEquals(1n, 1n)).toBe(true);
    expect(celValueEquals(1n, 2n)).toBe(false);
    expect(celValueEquals(true, true)).toBe(true);
    expect(celValueEquals(true, false)).toBe(false);
    expect(celValueEquals('a', 'a')).toBe(true);
    expect(celValueEquals('a', 'b')).toBe(false);
    expect(celValueEquals(null, null)).toBe(true);
  });

  it('compares int/uint/double by mathematical value (langdef Equality)', () => {
    // bigint (int/uint) vs number (double): 1 == 1.0.
    expect(celValueEquals(1n, 1)).toBe(true);
    expect(celValueEquals(2n, 2.0)).toBe(true);
    expect(celValueEquals(1n, 1.5)).toBe(false);
    expect(celValueEquals(1, 1)).toBe(true);
  });

  it('compares bytes byte-wise', () => {
    expect(celValueEquals(new Uint8Array([1, 2]), new Uint8Array([1, 2]))).toBe(
      true,
    );
    expect(celValueEquals(new Uint8Array([1, 2]), new Uint8Array([1, 3]))).toBe(
      false,
    );
  });

  it('treats mismatched non-numeric kinds as unequal', () => {
    expect(celValueEquals('1', 1n)).toBe(false);
    expect(celValueEquals(true, 1n)).toBe(false);
    expect(celValueEquals(null, 0n)).toBe(false);
  });

  it('compares timestamp / duration by (seconds, nanos)', () => {
    expect(
      celValueEquals(
        { kind: 'timestamp', epochSeconds: 5n, nanos: 7 },
        { kind: 'timestamp', epochSeconds: 5n, nanos: 7 },
      ),
    ).toBe(true);
    expect(
      celValueEquals(
        { kind: 'duration', seconds: 5n, nanos: 0 },
        { kind: 'duration', seconds: 5n, nanos: 1 },
      ),
    ).toBe(false);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_map_lookup — (out, map, key)
// ───────────────────────────────────────────────────────────────────
describe('cel_map_lookup', () => {
  const OUT = 0;
  const MAP = 1;
  const KEY = 2;

  it('returns the value for a hit (string-keyed map)', () => {
    const h = makeHarness();
    const ref = h.internMap({
      entries: [
        { key: 'a', value: 10n },
        { key: 'b', value: 20n },
      ],
    });
    putMapHost(h, MAP, ref);
    h.put(KEY, 'b');
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expect(h.get(OUT)).toBe(20n);
  });

  it('returns NO_SUCH_KEY for a miss', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 'a', value: 10n }] });
    putMapHost(h, MAP, ref);
    h.put(KEY, 'z');
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expectError(h, OUT, CelErrorCode.NO_SUCH_KEY);
  });

  it('looks up an int key across the numeric ladder (1 finds 1u)', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 1n, value: 'one' }] });
    putMapHost(h, MAP, ref);
    h.put(KEY, 1n);
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expect(h.get(OUT)).toBe('one');
  });

  it('misses on an empty map', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [] });
    putMapHost(h, MAP, ref);
    h.put(KEY, 'a');
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expectError(h, OUT, CelErrorCode.NO_SUCH_KEY);
  });

  it('absorbs an ERROR key (3VL)', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 'a', value: 1n }] });
    putMapHost(h, MAP, ref);
    putError(h, KEY, CelErrorCode.DIVIDE_BY_ZERO);
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expectError(h, OUT, CelErrorCode.DIVIDE_BY_ZERO);
  });

  it('absorbs an UNKNOWN map (3VL)', () => {
    const h = makeHarness();
    putUnknown(h, MAP);
    h.put(KEY, 'a');
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expect(h.kindAt(OUT)).toBe(CelKind.UNKNOWN);
  });

  it('poisons TYPE_MISMATCH for a non-key-kind query (double)', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 'a', value: 1n }] });
    putMapHost(h, MAP, ref);
    h.put(KEY, 1.5);
    trampolines(h).cel_map_lookup?.(h.slot(OUT), h.slot(MAP), h.slot(KEY));
    expectError(h, OUT, CelErrorCode.TYPE_MISMATCH);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_map_in — (out, key, map)
// ───────────────────────────────────────────────────────────────────
describe('cel_map_in', () => {
  const OUT = 0;
  const KEY = 1;
  const MAP = 2;

  it('returns true when the key is present', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 'x', value: 1n }] });
    h.put(KEY, 'x');
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_in?.(h.slot(OUT), h.slot(KEY), h.slot(MAP));
    expect(h.get(OUT)).toBe(true);
  });

  it('returns false when the key is absent', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 'x', value: 1n }] });
    h.put(KEY, 'y');
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_in?.(h.slot(OUT), h.slot(KEY), h.slot(MAP));
    expect(h.get(OUT)).toBe(false);
  });

  it('returns false on an empty map', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [] });
    h.put(KEY, 'x');
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_in?.(h.slot(OUT), h.slot(KEY), h.slot(MAP));
    expect(h.get(OUT)).toBe(false);
  });

  it('absorbs an ERROR operand (3VL)', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [{ key: 'x', value: 1n }] });
    putError(h, KEY, CelErrorCode.OVERFLOW);
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_in?.(h.slot(OUT), h.slot(KEY), h.slot(MAP));
    expectError(h, OUT, CelErrorCode.OVERFLOW);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_map_size — (out, map)
// ───────────────────────────────────────────────────────────────────
describe('cel_map_size', () => {
  const OUT = 0;
  const MAP = 1;

  it('returns the entry count as an INT', () => {
    const h = makeHarness();
    const ref = h.internMap({
      entries: [
        { key: 'a', value: 1n },
        { key: 'b', value: 2n },
        { key: 'c', value: 3n },
      ],
    });
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_size?.(h.slot(OUT), h.slot(MAP));
    expect(h.get(OUT)).toBe(3n);
  });

  it('returns 0 for an empty map', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [] });
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_size?.(h.slot(OUT), h.slot(MAP));
    expect(h.get(OUT)).toBe(0n);
  });

  it('absorbs an UNKNOWN operand (3VL)', () => {
    const h = makeHarness();
    putUnknown(h, MAP);
    trampolines(h).cel_map_size?.(h.slot(OUT), h.slot(MAP));
    expect(h.kindAt(OUT)).toBe(CelKind.UNKNOWN);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_map_eq — (out, a, b)
// ───────────────────────────────────────────────────────────────────
describe('cel_map_eq', () => {
  const OUT = 0;
  const A = 1;
  const B = 2;

  it('is true for set-equal maps regardless of order', () => {
    const h = makeHarness();
    const ra = h.internMap({
      entries: [
        { key: 'a', value: 1n },
        { key: 'b', value: 2n },
      ],
    });
    const rb = h.internMap({
      entries: [
        { key: 'b', value: 2n },
        { key: 'a', value: 1n },
      ],
    });
    putMapHost(h, A, ra);
    putMapHost(h, B, rb);
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('is false when a value differs', () => {
    const h = makeHarness();
    const ra = h.internMap({ entries: [{ key: 'a', value: 1n }] });
    const rb = h.internMap({ entries: [{ key: 'a', value: 9n }] });
    putMapHost(h, A, ra);
    putMapHost(h, B, rb);
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(false);
  });

  it('is false when sizes differ', () => {
    const h = makeHarness();
    const ra = h.internMap({ entries: [{ key: 'a', value: 1n }] });
    const rb = h.internMap({
      entries: [
        { key: 'a', value: 1n },
        { key: 'b', value: 2n },
      ],
    });
    putMapHost(h, A, ra);
    putMapHost(h, B, rb);
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(false);
  });

  it('is true for two empty maps', () => {
    const h = makeHarness();
    putMapHost(h, A, h.internMap({ entries: [] }));
    putMapHost(h, B, h.internMap({ entries: [] }));
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('absorbs an ERROR operand (3VL)', () => {
    const h = makeHarness();
    putError(h, A, CelErrorCode.NO_SUCH_KEY);
    putMapHost(h, B, h.internMap({ entries: [] }));
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expectError(h, OUT, CelErrorCode.NO_SUCH_KEY);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_map_iter_open — (state, map)
// ───────────────────────────────────────────────────────────────────
describe('cel_map_iter_open', () => {
  const STATE = 0; // a 16-byte MapIterState fits in one 24-byte slot.
  const MAP = 1;

  it('snapshots entries into a flat 48-byte-per-entry arena run', () => {
    const h = makeHarness();
    const ref = h.internMap({
      entries: [
        { key: 'k1', value: 100n },
        { key: 'k2', value: 200n },
      ],
    });
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_iter_open?.(h.slot(STATE), h.slot(MAP));
    const st = h.readIterState(h.slot(STATE));
    expect(st.kind).toBe(1); // MAP_ITER_KIND_HOST
    expect(st.cursor).toBe(0);
    expect(st.count).toBe(2);
    expect(st.payload).toBeGreaterThan(0);
    // Decode the two (key, value) pairs out of the snapshot.
    const view = h.ctx.view();
    const readAt = (off: number): CelValue => h.ctx.readValue(off);
    expect(readAt(st.payload + 0)).toBe('k1');
    expect(readAt(st.payload + CEL_VALUE_SIZE)).toBe(100n);
    expect(readAt(st.payload + 48)).toBe('k2');
    expect(readAt(st.payload + 48 + CEL_VALUE_SIZE)).toBe(200n);
    expect(view.byteLength).toBeGreaterThan(0);
  });

  it('writes count=0 for an empty map', () => {
    const h = makeHarness();
    const ref = h.internMap({ entries: [] });
    putMapHost(h, MAP, ref);
    trampolines(h).cel_map_iter_open?.(h.slot(STATE), h.slot(MAP));
    const st = h.readIterState(h.slot(STATE));
    expect(st.kind).toBe(1);
    expect(st.count).toBe(0);
    expect(st.payload).toBe(0);
  });

  it('throws on a poisoned (ERROR) source — codegen-regression tripwire', () => {
    const h = makeHarness();
    putError(h, MAP, CelErrorCode.DIVIDE_BY_ZERO);
    expect(() =>
      trampolines(h).cel_map_iter_open?.(h.slot(STATE), h.slot(MAP)),
    ).toThrow(/range/);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_list_at — (out, list, idx)
// ───────────────────────────────────────────────────────────────────
describe('cel_list_at', () => {
  const OUT = 0;
  const LIST = 1;
  const IDX = 2;

  it('returns the in-range element', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [10n, 20n, 30n] });
    putListHost(h, LIST, ref);
    h.put(IDX, 1n);
    trampolines(h).cel_list_at?.(h.slot(OUT), h.slot(LIST), h.slot(IDX));
    expect(h.get(OUT)).toBe(20n);
  });

  it('returns INDEX_OUT_OF_BOUNDS past the end', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [10n, 20n] });
    putListHost(h, LIST, ref);
    h.put(IDX, 5n);
    trampolines(h).cel_list_at?.(h.slot(OUT), h.slot(LIST), h.slot(IDX));
    expectError(h, OUT, CelErrorCode.INDEX_OUT_OF_BOUNDS);
  });

  it('returns INDEX_OUT_OF_BOUNDS for a negative index', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [10n] });
    putListHost(h, LIST, ref);
    h.put(IDX, -1n);
    trampolines(h).cel_list_at?.(h.slot(OUT), h.slot(LIST), h.slot(IDX));
    expectError(h, OUT, CelErrorCode.INDEX_OUT_OF_BOUNDS);
  });

  it('returns INDEX_OUT_OF_BOUNDS on an empty list', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [] });
    putListHost(h, LIST, ref);
    h.put(IDX, 0n);
    trampolines(h).cel_list_at?.(h.slot(OUT), h.slot(LIST), h.slot(IDX));
    expectError(h, OUT, CelErrorCode.INDEX_OUT_OF_BOUNDS);
  });

  it('admits an integral DOUBLE index (dyn) but rejects a fractional one', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [10n, 20n, 30n] });
    putListHost(h, LIST, ref);
    h.put(IDX, 2.0);
    trampolines(h).cel_list_at?.(h.slot(OUT), h.slot(LIST), h.slot(IDX));
    expect(h.get(OUT)).toBe(30n);

    const h2 = makeHarness();
    const ref2 = h2.internList({ elements: [10n, 20n, 30n] });
    putListHost(h2, LIST, ref2);
    h2.put(IDX, 1.5);
    trampolines(h2).cel_list_at?.(h2.slot(OUT), h2.slot(LIST), h2.slot(IDX));
    expectError(h2, OUT, CelErrorCode.INVALID_ARGUMENT);
  });

  it('absorbs an ERROR index before touching the list (3VL)', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [10n] });
    putListHost(h, LIST, ref);
    putError(h, IDX, CelErrorCode.OVERFLOW);
    trampolines(h).cel_list_at?.(h.slot(OUT), h.slot(LIST), h.slot(IDX));
    expectError(h, OUT, CelErrorCode.OVERFLOW);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_list_in — (out, val, list)
// ───────────────────────────────────────────────────────────────────
describe('cel_list_in', () => {
  const OUT = 0;
  const VAL = 1;
  const LIST = 2;

  it('returns true when the value is a member', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [1n, 2n, 3n] });
    h.put(VAL, 2n);
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_in?.(h.slot(OUT), h.slot(VAL), h.slot(LIST));
    expect(h.get(OUT)).toBe(true);
  });

  it('returns false when the value is absent', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [1n, 2n, 3n] });
    h.put(VAL, 9n);
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_in?.(h.slot(OUT), h.slot(VAL), h.slot(LIST));
    expect(h.get(OUT)).toBe(false);
  });

  it('matches across the numeric ladder (1 in [1u, 2u])', () => {
    const h = makeHarness();
    // Elements are doubles; query is an int. 1 == 1.0.
    const ref = h.internList({ elements: [1, 2] });
    h.put(VAL, 1n);
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_in?.(h.slot(OUT), h.slot(VAL), h.slot(LIST));
    expect(h.get(OUT)).toBe(true);
  });

  it('returns false on an empty list', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [] });
    h.put(VAL, 1n);
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_in?.(h.slot(OUT), h.slot(VAL), h.slot(LIST));
    expect(h.get(OUT)).toBe(false);
  });

  it('absorbs an ERROR operand (3VL)', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [1n] });
    putError(h, VAL, CelErrorCode.OVERFLOW);
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_in?.(h.slot(OUT), h.slot(VAL), h.slot(LIST));
    expectError(h, OUT, CelErrorCode.OVERFLOW);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_list_size — (out, list)
// ───────────────────────────────────────────────────────────────────
describe('cel_list_size', () => {
  const OUT = 0;
  const LIST = 1;

  it('returns the element count as an INT', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [1n, 2n, 3n, 4n] });
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_size?.(h.slot(OUT), h.slot(LIST));
    expect(h.get(OUT)).toBe(4n);
  });

  it('returns 0 for an empty list', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [] });
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_size?.(h.slot(OUT), h.slot(LIST));
    expect(h.get(OUT)).toBe(0n);
  });

  it('absorbs an UNKNOWN operand (3VL)', () => {
    const h = makeHarness();
    putUnknown(h, LIST);
    trampolines(h).cel_list_size?.(h.slot(OUT), h.slot(LIST));
    expect(h.kindAt(OUT)).toBe(CelKind.UNKNOWN);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_list_eq — (out, a, b)
// ───────────────────────────────────────────────────────────────────
describe('cel_list_eq', () => {
  const OUT = 0;
  const A = 1;
  const B = 2;

  it('is true for equal lists (positional)', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [1n, 2n, 3n] }));
    putListHost(h, B, h.internList({ elements: [1n, 2n, 3n] }));
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('is false when an element differs', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [1n, 2n, 3n] }));
    putListHost(h, B, h.internList({ elements: [1n, 9n, 3n] }));
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(false);
  });

  it('is false when lengths differ', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [1n, 2n] }));
    putListHost(h, B, h.internList({ elements: [1n, 2n, 3n] }));
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(false);
  });

  it('is true for two empty lists', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [] }));
    putListHost(h, B, h.internList({ elements: [] }));
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('absorbs an ERROR operand (3VL)', () => {
    const h = makeHarness();
    putError(h, A, CelErrorCode.OVERFLOW);
    putListHost(h, B, h.internList({ elements: [] }));
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expectError(h, OUT, CelErrorCode.OVERFLOW);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_list_concat — (out, a, b)
// ───────────────────────────────────────────────────────────────────
describe('cel_list_concat', () => {
  const OUT = 0;
  const A = 1;
  const B = 2;

  it('materialises a+b into a fresh arena LIST_ARENA', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [1n, 2n] }));
    putListHost(h, B, h.internList({ elements: [3n, 4n, 5n] }));
    trampolines(h).cel_list_concat?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.kindAt(OUT)).toBe(CelKind.LIST_ARENA);
    expect(h.readArenaList(OUT)).toEqual([1n, 2n, 3n, 4n, 5n]);
  });

  it('concatenates with an empty operand', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [] }));
    putListHost(h, B, h.internList({ elements: [7n] }));
    trampolines(h).cel_list_concat?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.readArenaList(OUT)).toEqual([7n]);
  });

  it('absorbs an ERROR operand (3VL)', () => {
    const h = makeHarness();
    putError(h, A, CelErrorCode.OVERFLOW);
    putListHost(h, B, h.internList({ elements: [1n] }));
    trampolines(h).cel_list_concat?.(h.slot(OUT), h.slot(A), h.slot(B));
    expectError(h, OUT, CelErrorCode.OVERFLOW);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_list_iter_open — (out, list)
// ───────────────────────────────────────────────────────────────────
describe('cel_list_iter_open', () => {
  const OUT = 0;
  const LIST = 1;

  it('snapshots a host list into an arena LIST_ARENA', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [11n, 22n, 33n] });
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_iter_open?.(h.slot(OUT), h.slot(LIST));
    expect(h.kindAt(OUT)).toBe(CelKind.LIST_ARENA);
    expect(h.readArenaList(OUT)).toEqual([11n, 22n, 33n]);
  });

  it('writes an empty (count=0) arena list for an empty source', () => {
    const h = makeHarness();
    const ref = h.internList({ elements: [] });
    putListHost(h, LIST, ref);
    trampolines(h).cel_list_iter_open?.(h.slot(OUT), h.slot(LIST));
    expect(h.kindAt(OUT)).toBe(CelKind.LIST_ARENA);
    expect(h.readArenaList(OUT)).toEqual([]);
    // header_ptr must be non-zero so the prologue's 2-load walk is safe.
    expect(h.payloadU32At(OUT)).toBeGreaterThan(0);
  });

  it('throws on a poisoned (UNKNOWN) source — codegen-regression tripwire', () => {
    const h = makeHarness();
    putUnknown(h, LIST);
    expect(() =>
      trampolines(h).cel_list_iter_open?.(h.slot(OUT), h.slot(LIST)),
    ).toThrow(/range/);
  });
});

// ───────────────────────────────────────────────────────────────────
// Mixed arena/host equality — the runtime routes a pair to the host
// trampoline whenever EITHER operand is host-backed
// (`cel_runtime.c::cel_list_eq`), so the bridge must compare across
// origins (cel-cpp `CelListEqImpl` / `NormalizedMapEq` admit any pair).
// Pinned to the corpus comparisons/bound rows (`[1, 2] == x`) and the
// proto field read-back vs literal rows (set_null/*_null_pruned).
// ───────────────────────────────────────────────────────────────────
describe('cel_list_eq — mixed arena/host operands', () => {
  const OUT = 0;
  const A = 1;
  const B = 2;

  it('compares a host list against an arena list', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [1n, 2n] }));
    putArenaList(h, B, [1n, 2n]);
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('compares an arena list against a host list (order swapped)', () => {
    const h = makeHarness();
    putArenaList(h, A, [1n, 2n]);
    putListHost(h, B, h.internList({ elements: [3n, 4n] }));
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(false);
  });

  it('compares timestamp elements across origins', () => {
    const h = makeHarness();
    const ts: CelValue = { kind: 'timestamp', epochSeconds: 1n, nanos: 0 };
    putListHost(h, A, h.internList({ elements: [ts] }));
    putArenaList(h, B, [ts]);
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('errors TYPE_MISMATCH for a non-list operand', () => {
    const h = makeHarness();
    putListHost(h, A, h.internList({ elements: [] }));
    h.put(B, 7n);
    trampolines(h).cel_list_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expectError(h, OUT, CelErrorCode.TYPE_MISMATCH);
  });
});

describe('cel_map_eq — mixed arena/host operands', () => {
  const OUT = 0;
  const A = 1;
  const B = 2;

  it('compares a host map against an arena map (set equality)', () => {
    const h = makeHarness();
    putMapHost(
      h,
      A,
      h.internMap({
        entries: [
          { key: 'a', value: 1n },
          { key: 'b', value: 2n },
        ],
      }),
    );
    putArenaMap(h, B, [
      ['b', 2n],
      ['a', 1n],
    ]);
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('compares timestamp values across origins', () => {
    const h = makeHarness();
    const ts: CelValue = { kind: 'timestamp', epochSeconds: 1n, nanos: 0 };
    putArenaMap(h, A, [[false, ts]]);
    putMapHost(h, B, h.internMap({ entries: [{ key: false, value: ts }] }));
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(true);
  });

  it('is false when an arena-side value differs', () => {
    const h = makeHarness();
    putMapHost(h, A, h.internMap({ entries: [{ key: 'a', value: 1n }] }));
    putArenaMap(h, B, [['a', 9n]]);
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expect(h.get(OUT)).toBe(false);
  });

  it('errors TYPE_MISMATCH for a non-map operand', () => {
    const h = makeHarness();
    putMapHost(h, A, h.internMap({ entries: [] }));
    h.put(B, 'not-a-map');
    trampolines(h).cel_map_eq?.(h.slot(OUT), h.slot(A), h.slot(B));
    expectError(h, OUT, CelErrorCode.TYPE_MISMATCH);
  });
});

/**
 * Arena-backed list / map decode — the read side of the runtime's
 * `ArenaListHeader` / `ArenaMapHeader` (compiler_v2/runtime/cel_data.h
 * §4.1/§4.2). A list/map *built inside wasm* (a `[...]` / `{...}` literal,
 * a comprehension result) lives in the bump arena, not the externref
 * table: the result `CelValue` carries `CEL_LIST_ARENA(7)` / `CEL_MAP_ARENA(8)`
 * with a `header_ptr` (u32 @ payload offset 8) into linear memory.
 *
 * Layout (all little-endian, offsets verified against cel_data.h):
 *   ArenaListHeader @ header_ptr: { count u32@0, capacity u32@4,
 *     elements_offset u32@8, _pad@12 }; elements are a contiguous run of
 *     `count` × 24-byte CelValues at `elements_offset`.
 *   ArenaMapHeader  @ header_ptr: { count u32@0, capacity u32@4,
 *     entries_offset u32@8, _pad@12 }; entries are `count` × 48-byte
 *     { key CelValue@+0, value CelValue@+24 } pairs at `entries_offset`.
 *
 * Elements / keys / values are INLINE 24-byte CelValues, so an element may
 * itself be a scalar, a `CEL_MESSAGE` (externref slot → backing), a host
 * list/map (externref slot), a nested arena list/map (recurse), `null`, or
 * `unknown`. `decodeValueAt` is the one recursive per-CelValue decoder;
 * `instance.ts` uses it for the top-level result too, so every code path
 * decodes a CelValue identically.
 *
 * Lifetime: these backings read LIVE linear memory lazily — valid until the
 * next `eval` (whose `arena_reset` reclaims the arena and whose
 * `refs.reset()` clears the externref table), exactly like the externref
 * backings. Materialize (asList/asMap) within the same turn.
 */
import { CelKind, decodeCelValue, type CelValue } from '../celvalue.js';
import type { ExternrefTable } from '../externref.js';
import { Value } from '../value.js';
import { valueToHost } from './value-backing.js';
import { celKeyTag } from './map-key.js';
import type {
  HostValue,
  ListBacking,
  MapBacking,
  MessageBacking,
} from './backing.js';

const PAYLOAD = 8;
const CELVALUE_SIZE = 24;
const MAP_ENTRY_STRIDE = 48; // kCelMapEntryStride (key + value)
// Header field offsets (shared by list + map headers).
const HDR_COUNT = 0;
const HDR_DATA_OFFSET = 8; // elements_offset / entries_offset

/** Thrown when a decoded CelValue references a slot that isn't interned. */
export class ArenaDecodeError extends Error {
  public override readonly name = 'ArenaDecodeError';
}

/** The state the recursive decoder needs: shared memory + the externref
 *  table (for `CEL_MESSAGE` / host list/map elements). Structurally a
 *  subset of `TrampolineContext`, so the Instance passes its `ctx`. */
export interface DecodeContext {
  readonly memory: WebAssembly.Memory;
  readonly refs: ExternrefTable<MessageBacking, MapBacking, ListBacking>;
}

function view(ctx: DecodeContext): DataView {
  return new DataView(ctx.memory.buffer);
}

/** Decode the 24-byte CelValue at `offset` into a public `Value`,
 *  resolving host slots and recursing into nested arena aggregates. */
export function decodeValueAt(ctx: DecodeContext, offset: number): Value {
  const dv = view(ctx);
  const kind = dv.getUint32(offset, true);
  const payload = (): number => dv.getUint32(offset + PAYLOAD, true);
  switch (kind) {
    case CelKind.Message: {
      const backing = ctx.refs.lookupMessage(payload());
      if (backing === undefined) {
        throw new ArenaDecodeError(`message slot ${payload()} is not interned`);
      }
      return Value.message(backing);
    }
    case CelKind.ListHost: {
      const backing = ctx.refs.lookupList(payload());
      if (backing === undefined) {
        throw new ArenaDecodeError(`list slot ${payload()} is not interned`);
      }
      return Value.listOf(backing);
    }
    case CelKind.MapHost: {
      const backing = ctx.refs.lookupMap(payload());
      if (backing === undefined) {
        throw new ArenaDecodeError(`map slot ${payload()} is not interned`);
      }
      return Value.mapOf(backing);
    }
    case CelKind.ListArena:
      return Value.listOf(new ArenaListBacking(ctx, payload()));
    case CelKind.MapArena:
      return Value.mapOf(new ArenaMapBacking(ctx, payload()));
    case CelKind.Unknown:
      return Value.unknown();
    default:
      // Scalars + error are self-contained; any other kind (type / time)
      // is out of the current subset and `decodeCelValue` throws.
      return decodeCelValue(new Uint8Array(ctx.memory.buffer), offset);
  }
}

/** `ListBacking` reading an `ArenaListHeader` + its inline element run. */
export class ArenaListBacking implements ListBacking {
  public constructor(
    private readonly ctx: DecodeContext,
    private readonly headerPtr: number,
  ) {}

  private header(field: number): number {
    return view(this.ctx).getUint32(this.headerPtr + field, true);
  }

  public get size(): number {
    return this.header(HDR_COUNT);
  }

  private elementOffset(index: number): number {
    return this.header(HDR_DATA_OFFSET) + index * CELVALUE_SIZE;
  }

  public at(index: number): HostValue | undefined {
    if (index < 0 || index >= this.size) {
      return undefined;
    }
    return valueToHost(decodeValueAt(this.ctx, this.elementOffset(index)));
  }

  public forEach(visit: (element: HostValue) => void): void {
    const n = this.size;
    for (let i = 0; i < n; i++) {
      visit(valueToHost(decodeValueAt(this.ctx, this.elementOffset(i))));
    }
  }
}

/** `MapBacking` reading an `ArenaMapHeader` + its inline {key,value} run.
 *  Map keys are always scalars, so they decode with the scalar codec
 *  directly (never a message/aggregate); values recurse. */
export class ArenaMapBacking implements MapBacking {
  public constructor(
    private readonly ctx: DecodeContext,
    private readonly headerPtr: number,
  ) {}

  private header(field: number): number {
    return view(this.ctx).getUint32(this.headerPtr + field, true);
  }

  public get size(): number {
    return this.header(HDR_COUNT);
  }

  private keyOffset(index: number): number {
    return this.header(HDR_DATA_OFFSET) + index * MAP_ENTRY_STRIDE;
  }

  private keyAt(index: number): CelValue {
    return decodeCelValue(
      new Uint8Array(this.ctx.memory.buffer),
      this.keyOffset(index),
    );
  }

  private valueAt(index: number): HostValue {
    return valueToHost(
      decodeValueAt(this.ctx, this.keyOffset(index) + CELVALUE_SIZE),
    );
  }

  public get(key: CelValue): HostValue | undefined {
    const tag = celKeyTag(key);
    const n = this.size;
    for (let i = 0; i < n; i++) {
      if (celKeyTag(this.keyAt(i)) === tag) {
        return this.valueAt(i);
      }
    }
    return undefined;
  }

  public has(key: CelValue): boolean {
    return this.get(key) !== undefined;
  }

  public forEach(visit: (key: CelValue, value: HostValue) => void): void {
    const n = this.size;
    for (let i = 0; i < n; i++) {
      visit(this.keyAt(i), this.valueAt(i));
    }
  }
}

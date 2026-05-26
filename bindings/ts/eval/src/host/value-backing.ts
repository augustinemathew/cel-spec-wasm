/**
 * `Value`-backed aggregates + the `Value` ⇆ `HostValue` bridge.
 *
 * The public `Value` (value.ts) is a superset of the wire `CelValue`: it
 * adds the host-backed arms (message / list / map carry a backing) so the
 * SAME type a caller binds in an `Activation` is what `Instance.eval`
 * hands back. The `cel_host.*` trampolines, though, speak `HostValue`
 * (host/backing.ts). This module is the one place those two models meet:
 *
 *   - `valueToHost` / `hostToValue` — total, lossless converters.
 *   - `ValueListBacking` / `ValueMapBacking` — `ListBacking` / `MapBacking`
 *     over an in-memory `Value[]` (what `Value.list` / `Value.map`
 *     construct), so a caller-built aggregate reads through the trampolines
 *     identically to a proto- or object-backed one.
 *
 * Layering: this imports the `Value` *type* from value.ts (erased at
 * runtime), while value.ts imports these classes — no runtime cycle.
 */
import { CelKind, type CelValue } from '../celvalue.js';
import {
  HostBackingError,
  listValue,
  mapValue,
  messageValue,
  scalarValue,
  type HostValue,
  type ListBacking,
  type MapBacking,
} from './backing.js';
import { celKeyTag } from './map-key.js';
import type { Value } from '../value.js';

/** Convert a public `Value` to the `HostValue` the trampolines consume.
 *  Throws on `UNKNOWN` (not a bindable/host-readable value). */
export function valueToHost(v: Value): HostValue {
  switch (v.kind) {
    case CelKind.Message:
      return messageValue(v.backing);
    case CelKind.ListHost:
      return listValue(v.backing);
    case CelKind.MapHost:
      return mapValue(v.backing);
    case CelKind.Unknown:
      throw new HostBackingError(
        'cannot convert an UNKNOWN value to a host value',
      );
    default:
      // null / bool / int / uint / double / string / bytes / error — all
      // self-contained `CelValue` scalars.
      return scalarValue(v);
  }
}

/** Convert a `HostValue` (trampoline output) back to a public `Value`. */
export function hostToValue(hv: HostValue): Value {
  switch (hv.host) {
    case 'scalar':
      return hv.value;
    case 'message':
      return { kind: CelKind.Message, backing: hv.backing };
    case 'list':
      return { kind: CelKind.ListHost, backing: hv.backing };
    case 'map':
      return { kind: CelKind.MapHost, backing: hv.backing };
  }
}

/** Narrow a `Value` to a valid CEL map key (string / int / uint / bool),
 *  or throw — mirrors the closed key-kind set in `celKeyTag`. */
function asKey(k: Value): CelValue {
  switch (k.kind) {
    case CelKind.String:
    case CelKind.Int:
    case CelKind.Uint:
    case CelKind.Bool:
      return k;
    default:
      throw new HostBackingError(`invalid map key kind ${k.kind}`);
  }
}

/** `ListBacking` over a caller-built `Value[]` (what `Value.list` wraps). */
export class ValueListBacking implements ListBacking {
  public constructor(private readonly items: readonly Value[]) {}

  public get size(): number {
    return this.items.length;
  }

  public at(index: number): HostValue | undefined {
    // `noUncheckedIndexedAccess`: an out-of-range index (incl. negative)
    // reads as `undefined`, which is exactly the OOB signal — a `Value`
    // is never itself `undefined`.
    const it = this.items[index];
    return it === undefined ? undefined : valueToHost(it);
  }

  public forEach(visit: (element: HostValue) => void): void {
    for (const it of this.items) {
      visit(valueToHost(it));
    }
  }
}

/** `MapBacking` over caller-built `[key, value]` `Value` pairs (what
 *  `Value.map` wraps). Keys reduce to the cross-type `celKeyTag`. */
export class ValueMapBacking implements MapBacking {
  // tag → [CEL key, value], materialized once for O(1) lookup.
  private readonly byTag = new Map<string, [CelValue, HostValue]>();

  public constructor(entries: readonly (readonly [Value, Value])[]) {
    for (const [k, v] of entries) {
      const key = asKey(k);
      this.byTag.set(celKeyTag(key), [key, valueToHost(v)]);
    }
  }

  public get size(): number {
    return this.byTag.size;
  }

  public get(key: CelValue): HostValue | undefined {
    return this.byTag.get(celKeyTag(key))?.[1];
  }

  public has(key: CelValue): boolean {
    return this.byTag.has(celKeyTag(key));
  }

  public forEach(visit: (key: CelValue, value: HostValue) => void): void {
    for (const [celKey, value] of this.byTag.values()) {
      visit(celKey, value);
    }
  }
}

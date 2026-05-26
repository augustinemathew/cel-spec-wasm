/**
 * Public value surface — the TS counterpart to `compiler_v2/api/value.h`
 * (`cel::Value`). `Value` is **one** discriminated union (keyed on `kind`)
 * that is a *superset* of the wire `CelValue` from `celvalue.ts`: the
 * scalar + error arms are exactly `CelValue`, plus the host-backed
 * aggregate arms (`Message` / `ListHost` / `MapHost`, each carrying a
 * backing) and `Unknown`. One value model, both directions — the same
 * `Value` a caller binds into an `Activation` is what `Instance.eval`
 * returns.
 *
 * `Value.*` are the named factories (`cel::Value::Int(42)`,
 * `cel::Value::Message(...)`); the `asX` helpers are the typed accessors
 * (`cel::Value::AsInt()` — they throw `ValueError` on a kind mismatch, the
 * TS analog of a non-OK `StatusOr`). Aggregate accessors *materialize*:
 * `asList` walks the backing into a `Value[]`, `asMap` into key/value
 * pairs. `asMessage` returns the read interface (`MessageBacking`);
 * turning that into a *typed* protobuf-es message is a `TypeRegistry`
 * concern (it owns the descriptors), kept out of this registry-free module.
 *
 * Idiomatic TS note: accessors are free functions over the union (e.g.
 * `asInt(v)`), not methods on a wrapper class — narrowing a discriminated
 * union is the idiomatic shape and keeps `Value`s plain data.
 */
import { CelKind, type CelValue } from './celvalue.js';
import type {
  ListBacking,
  MapBacking,
  MessageBacking,
} from './host/backing.js';
import { ObjectMessageBacking } from './host/object-backing.js';
import {
  hostToValue,
  ValueListBacking,
  ValueMapBacking,
} from './host/value-backing.js';

/**
 * A CEL value at the public boundary: every `CelValue` wire arm, plus the
 * host-backed aggregates and `Unknown`. `CelValue` ⊂ `Value`, so a wire
 * decode result is already a `Value`.
 */
export type Value =
  | CelValue
  | { readonly kind: typeof CelKind.Message; readonly backing: MessageBacking }
  | { readonly kind: typeof CelKind.ListHost; readonly backing: ListBacking }
  | { readonly kind: typeof CelKind.MapHost; readonly backing: MapBacking }
  | { readonly kind: typeof CelKind.Unknown };

/** Thrown by an `asX` accessor when the value's kind doesn't match. */
export class ValueError extends Error {
  public override readonly name = 'ValueError';
}

/** Named constructors mirroring `cel::Value::Int(...)` etc. The scalar
 *  factories return `CelValue` (a `Value` subtype); the aggregate ones
 *  build a backing and return the host-backed `Value` arm. */
export const Value = {
  null(): CelValue {
    return { kind: CelKind.Null };
  },
  bool(b: boolean): CelValue {
    return { kind: CelKind.Bool, bool: b };
  },
  int(i: bigint): CelValue {
    return { kind: CelKind.Int, int: i };
  },
  uint(u: bigint): CelValue {
    return { kind: CelKind.Uint, uint: u };
  },
  double(d: number): CelValue {
    return { kind: CelKind.Double, double: d };
  },
  string(s: string): CelValue {
    return { kind: CelKind.String, value: s };
  },
  bytes(b: Uint8Array): CelValue {
    return { kind: CelKind.Bytes, bytes: b };
  },
  /** A CEL message from any `MessageBacking` (proto- or object-backed).
   *  For a real protobuf-es message use `TypeRegistry.message(msg)` to
   *  build the backing; for plain JS data use {@link Value.object}. */
  message(backing: MessageBacking): Value {
    return { kind: CelKind.Message, backing };
  },
  /** A CEL message mirrored by a plain JS object (the JSObject-as-proto
   *  path) — sugar for `Value.message(new ObjectMessageBacking(obj))`. */
  object(obj: Record<string, unknown>): Value {
    return { kind: CelKind.Message, backing: new ObjectMessageBacking(obj) };
  },
  /** A CEL list from caller-built elements. */
  list(items: readonly Value[]): Value {
    return { kind: CelKind.ListHost, backing: new ValueListBacking(items) };
  },
  /** A CEL list from any `ListBacking` (e.g. a proto repeated field). */
  listOf(backing: ListBacking): Value {
    return { kind: CelKind.ListHost, backing };
  },
  /** A CEL map from caller-built `[key, value]` pairs. Keys must be
   *  scalar (string / int / uint / bool). */
  map(entries: readonly (readonly [Value, Value])[]): Value {
    return { kind: CelKind.MapHost, backing: new ValueMapBacking(entries) };
  },
  /** A CEL map from any `MapBacking` (e.g. a proto map field). */
  mapOf(backing: MapBacking): Value {
    return { kind: CelKind.MapHost, backing };
  },
  /** The UNKNOWN sentinel (partial-eval inputs / results). */
  unknown(): Value {
    return { kind: CelKind.Unknown };
  },
} as const;

export function asBool(v: Value): boolean {
  if (v.kind !== CelKind.Bool) {
    throw new ValueError(`expected bool, got kind ${v.kind}`);
  }
  return v.bool;
}

export function asInt(v: Value): bigint {
  if (v.kind !== CelKind.Int) {
    throw new ValueError(`expected int, got kind ${v.kind}`);
  }
  return v.int;
}

export function asUint(v: Value): bigint {
  if (v.kind !== CelKind.Uint) {
    throw new ValueError(`expected uint, got kind ${v.kind}`);
  }
  return v.uint;
}

export function asDouble(v: Value): number {
  if (v.kind !== CelKind.Double) {
    throw new ValueError(`expected double, got kind ${v.kind}`);
  }
  return v.double;
}

export function asString(v: Value): string {
  if (v.kind !== CelKind.String) {
    throw new ValueError(`expected string, got kind ${v.kind}`);
  }
  return v.value;
}

export function asBytes(v: Value): Uint8Array {
  if (v.kind !== CelKind.Bytes) {
    throw new ValueError(`expected bytes, got kind ${v.kind}`);
  }
  return v.bytes;
}

/** The message's read interface. Typed protobuf-es materialization is
 *  `TypeRegistry.toMessage(backing, schema)` (it owns the descriptors). */
export function asMessage(v: Value): MessageBacking {
  if (v.kind !== CelKind.Message) {
    throw new ValueError(`expected message, got kind ${v.kind}`);
  }
  return v.backing;
}

/** Materialize a CEL list into a `Value[]` (elements recurse). */
export function asList(v: Value): Value[] {
  if (v.kind !== CelKind.ListHost) {
    throw new ValueError(`expected list, got kind ${v.kind}`);
  }
  const out: Value[] = [];
  v.backing.forEach((e) => out.push(hostToValue(e)));
  return out;
}

/** Materialize a CEL map into `[key, value]` `Value` pairs. */
export function asMap(v: Value): [Value, Value][] {
  if (v.kind !== CelKind.MapHost) {
    throw new ValueError(`expected map, got kind ${v.kind}`);
  }
  const out: [Value, Value][] = [];
  v.backing.forEach((k, val) => out.push([k, hostToValue(val)]));
  return out;
}

export function isNull(v: Value): boolean {
  return v.kind === CelKind.Null;
}

export function isError(v: Value): boolean {
  return v.kind === CelKind.Error;
}

export function isUnknown(v: Value): boolean {
  return v.kind === CelKind.Unknown;
}

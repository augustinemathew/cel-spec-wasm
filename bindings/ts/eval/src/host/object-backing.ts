/**
 * Plain-JS host backings — the "JSON / struct-of-structs" path
 * (`compiler_v2/api/internal/cel_host.h` custom-backing subclasses; probe
 * P-9). A JS object mirrors a CEL message, an array a list, a `Map` a
 * map. Values map to CEL by JS type:
 *
 *   bigint → int   number → double   boolean → bool   string → string
 *   Uint8Array → bytes   null/undefined → null
 *   array → list   Map → map   other object → message (recurses)
 *
 * `uint` is reachable only via an explicit `protobuf`/descriptor backing
 * (a plain JS `bigint` is `int`); int/uint key equality is still
 * cross-type at lookup time, matching langdef.
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
  type MessageBacking,
} from './backing.js';
import { celKeyTag } from './map-key.js';

/** Map an arbitrary JS value (mirroring proto/JSON data) to a HostValue. */
export function jsToHost(v: unknown): HostValue {
  if (v === null || v === undefined) {
    return scalarValue({ kind: CelKind.Null });
  }
  switch (typeof v) {
    case 'boolean':
      return scalarValue({ kind: CelKind.Bool, bool: v });
    case 'bigint':
      return scalarValue({ kind: CelKind.Int, int: v });
    case 'number':
      return scalarValue({ kind: CelKind.Double, double: v });
    case 'string':
      return scalarValue({ kind: CelKind.String, value: v });
    case 'object':
      if (v instanceof Uint8Array) {
        return scalarValue({ kind: CelKind.Bytes, bytes: v });
      }
      if (Array.isArray(v)) {
        return listValue(new ObjectListBacking(v));
      }
      if (v instanceof Map) {
        return mapValue(new ObjectMapBacking(v as Map<unknown, unknown>));
      }
      return messageValue(
        new ObjectMessageBacking(v as Record<string, unknown>),
      );
    default:
      throw new HostBackingError(
        `cannot represent JS ${typeof v} as a CEL value`,
      );
  }
}

export class ObjectMessageBacking implements MessageBacking {
  public constructor(private readonly obj: Record<string, unknown>) {}

  public getField(name: string): HostValue | undefined {
    if (!Object.hasOwn(this.obj, name)) {
      return undefined;
    }
    return jsToHost(this.obj[name]);
  }

  public hasField(name: string): boolean {
    // Presence: the key exists with a non-null value (proto3-ish).
    return (
      Object.hasOwn(this.obj, name) &&
      this.obj[name] !== null &&
      this.obj[name] !== undefined
    );
  }
}

export class ObjectListBacking implements ListBacking {
  public constructor(private readonly arr: readonly unknown[]) {}

  public get size(): number {
    return this.arr.length;
  }

  public at(index: number): HostValue | undefined {
    if (index < 0 || index >= this.arr.length) {
      return undefined;
    }
    return jsToHost(this.arr[index]);
  }

  public forEach(visit: (element: HostValue) => void): void {
    for (const e of this.arr) {
      visit(jsToHost(e));
    }
  }
}

// A JS map key → its comparison tag + the CEL key it represents. One
// switch (one `default`) so there's no unreachable duplicate arm.
function jsKey(key: unknown): { tag: string; cel: CelValue } {
  switch (typeof key) {
    case 'string':
      return { tag: `s:${key}`, cel: { kind: CelKind.String, value: key } };
    case 'bigint':
      return { tag: `i:${key}`, cel: { kind: CelKind.Int, int: key } };
    case 'number':
      if (!Number.isInteger(key)) {
        throw new HostBackingError(`non-integer map key ${key}`);
      }
      return {
        tag: `i:${BigInt(key)}`,
        cel: { kind: CelKind.Int, int: BigInt(key) },
      };
    case 'boolean':
      return {
        tag: `b:${key ? 1 : 0}`,
        cel: { kind: CelKind.Bool, bool: key },
      };
    default:
      throw new HostBackingError(`invalid JS map key type ${typeof key}`);
  }
}

export class ObjectMapBacking implements MapBacking {
  // tag → [original CEL key, value], built once for O(1) lookup.
  private readonly byTag = new Map<string, [CelValue, HostValue]>();

  public constructor(entries: Map<unknown, unknown>) {
    for (const [k, v] of entries) {
      const { tag, cel } = jsKey(k);
      this.byTag.set(tag, [cel, jsToHost(v)]);
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

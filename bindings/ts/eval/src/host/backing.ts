/**
 * Host-backing model — the TS counterpart to the Layer-1 interfaces in
 * `compiler_v2/api/internal/cel_host.h` (`HostMessageBacking` /
 * `HostListBacking` / `HostMapBacking`).
 *
 * A `CEL_MESSAGE` / `CEL_LIST_HOST` / `CEL_MAP_HOST` value isn't stored
 * inline — it's a host object reached through the externref table. A
 * *backing* is the read interface the `cel_host.*` trampolines call:
 * given a field name / index / key it yields a `HostValue`, which is
 * either a scalar (a `CelValue`) or another backing (nested aggregates
 * recurse). Concrete backings wrap a protobuf-es `Message`
 * (`proto-backing.ts`) or a plain JS object/array/Map
 * (`object-backing.ts`); the trampolines are backing-agnostic (P-6/P-9).
 */
import type { CelValue } from '../celvalue.js';

/** A value produced by reading a backing: a scalar or a nested aggregate. */
export type HostValue =
  | { readonly host: 'scalar'; readonly value: CelValue }
  | { readonly host: 'message'; readonly backing: MessageBacking }
  | { readonly host: 'list'; readonly backing: ListBacking }
  | { readonly host: 'map'; readonly backing: MapBacking };

/** Field-read semantics for a CEL message (proto or struct-like). */
export interface MessageBacking {
  /** The field's value, or `undefined` if the message has no such field
   *  (the trampoline maps that to `CEL_ERROR(kFieldNotFound)`). */
  getField(name: string): HostValue | undefined;
  /** `has(msg.field)` per langdef presence rules. */
  hasField(name: string): boolean;
}

/** Indexed access for a CEL list. */
export interface ListBacking {
  readonly size: number;
  /** Element at `index`, or `undefined` if out of `[0, size)`
   *  (→ `CEL_ERROR(kIndexOutOfBounds)`). */
  at(index: number): HostValue | undefined;
  /** In-order traversal (for `in` / `==` / output materialization). */
  forEach(visit: (element: HostValue) => void): void;
}

/** Keyed access for a CEL map. */
export interface MapBacking {
  readonly size: number;
  /** Value for `key` per langdef map-key equality (cross-type numeric
   *  for int/uint), or `undefined` if absent (→ `CEL_ERROR(kNoSuchKey)`). */
  get(key: CelValue): HostValue | undefined;
  has(key: CelValue): boolean;
  forEach(visit: (key: CelValue, value: HostValue) => void): void;
}

/** Thrown when a host value can't be represented as a CEL value. */
export class HostBackingError extends Error {
  public override readonly name = 'HostBackingError';
}

// ── HostValue constructors ──────────────────────────────────────────
export function scalarValue(value: CelValue): HostValue {
  return { host: 'scalar', value };
}
export function messageValue(backing: MessageBacking): HostValue {
  return { host: 'message', backing };
}
export function listValue(backing: ListBacking): HostValue {
  return { host: 'list', backing };
}
export function mapValue(backing: MapBacking): HostValue {
  return { host: 'map', backing };
}

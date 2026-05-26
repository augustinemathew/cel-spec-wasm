/**
 * 24-byte CelValue codec — the TS mirror of
 * `compiler_v2/runtime/cel_data.h` (the wire struct) +
 * `compiler_v2/api/instance.cc::DecodeCelValueAt` (the scalar arms).
 *
 * Scope: the kinds that are self-contained in `(memory, offset)` —
 * scalars + string/bytes spans + error. Aggregates (arena list/map) and
 * host-backed kinds (message / list_host / map_host) need the externref
 * table + arena and are decoded by `instance.ts`, not here.
 *
 * Correctness rules (ts/CLAUDE.md): CEL `int`/`uint` are 64-bit →
 * `bigint`, never `number`; every `DataView` access is little-endian;
 * bytes are `Uint8Array`.
 *
 * `CelKind` is a `const`-object tag set (not a TS `enum`): the wire tag
 * is a raw `number` read from memory, and switching a `number` over enum
 * cases trips typescript-eslint's `no-unsafe-enum-comparison` while
 * asserting it to an enum trips `no-unnecessary-type-assertion`. A
 * number-literal `const` object sidesteps both and is the idiomatic
 * shape for a wire decoder.
 */

/** Total size of a wire CelValue (cel_data.h `_Static_assert == 24`). */
export const CELVALUE_SIZE = 24;

/** Byte offset of the payload union (kind u32 @0, `_pad` u32 @4). */
const PAYLOAD = 8;

/** Stable wire kind tags — mirror `cel_data.h::CelKind` 1:1. */
export const CelKind = {
  Null: 0,
  Bool: 1,
  Int: 2,
  Uint: 3,
  Double: 4,
  String: 5,
  Bytes: 6,
  ListArena: 7,
  MapArena: 8,
  MapHost: 9,
  Message: 10,
  Type: 11,
  Duration: 12,
  Timestamp: 13,
  Optional: 14,
  Unknown: 15,
  Error: 16,
  ListHost: 17,
} as const;

export type CelKind = (typeof CelKind)[keyof typeof CelKind];

/** Host-side decoded value for the codec-handleable kinds. */
export type CelValue =
  | { readonly kind: typeof CelKind.Null }
  | { readonly kind: typeof CelKind.Bool; readonly bool: boolean }
  | { readonly kind: typeof CelKind.Int; readonly int: bigint }
  | { readonly kind: typeof CelKind.Uint; readonly uint: bigint }
  | { readonly kind: typeof CelKind.Double; readonly double: number }
  | { readonly kind: typeof CelKind.String; readonly value: string }
  | { readonly kind: typeof CelKind.Bytes; readonly bytes: Uint8Array }
  | { readonly kind: typeof CelKind.Error; readonly errorCode: number };

/** Thrown on malformed wire bytes or a kind this codec can't handle
 *  without instance context. */
export class CelDecodeError extends Error {
  public override readonly name = 'CelDecodeError';
}

const utf8 = new TextDecoder('utf-8', { fatal: false });

function viewOf(mem: Uint8Array): DataView {
  return new DataView(mem.buffer, mem.byteOffset, mem.byteLength);
}

/** Read a `{ptr, len}` CelSpan (at payload +0 / +4) as a memory subarray. */
function readSpan(mem: Uint8Array, dv: DataView, offset: number): Uint8Array {
  const ptr = dv.getUint32(offset + PAYLOAD, true);
  const len = dv.getUint32(offset + PAYLOAD + 4, true);
  if (ptr + len > mem.byteLength) {
    throw new CelDecodeError(
      `span [${ptr}, ${ptr + len}) exceeds memory size ${mem.byteLength}`,
    );
  }
  return mem.subarray(ptr, ptr + len);
}

/**
 * Decode the 24-byte CelValue at `offset` in `mem` (little-endian).
 * Throws `CelDecodeError` on an unknown kind byte, on a span that
 * overruns memory, or on a kind that needs the externref table / arena
 * (aggregate / host / message / type / time / unknown) — those are
 * `instance.ts`'s responsibility.
 */
export function decodeCelValue(mem: Uint8Array, offset: number): CelValue {
  const dv = viewOf(mem);
  const kind = dv.getUint32(offset, true);
  switch (kind) {
    case CelKind.Null:
      return { kind: CelKind.Null };
    case CelKind.Bool:
      return {
        kind: CelKind.Bool,
        bool: dv.getInt32(offset + PAYLOAD, true) !== 0,
      };
    case CelKind.Int:
      return { kind: CelKind.Int, int: dv.getBigInt64(offset + PAYLOAD, true) };
    case CelKind.Uint:
      return {
        kind: CelKind.Uint,
        uint: dv.getBigUint64(offset + PAYLOAD, true),
      };
    case CelKind.Double:
      return {
        kind: CelKind.Double,
        double: dv.getFloat64(offset + PAYLOAD, true),
      };
    case CelKind.String:
      return {
        kind: CelKind.String,
        value: utf8.decode(readSpan(mem, dv, offset)),
      };
    case CelKind.Bytes:
      return { kind: CelKind.Bytes, bytes: readSpan(mem, dv, offset).slice() };
    case CelKind.Error:
      return {
        kind: CelKind.Error,
        errorCode: dv.getUint32(offset + PAYLOAD, true),
      };
    case CelKind.ListArena:
    case CelKind.MapArena:
    case CelKind.MapHost:
    case CelKind.Message:
    case CelKind.Type:
    case CelKind.Duration:
    case CelKind.Timestamp:
    case CelKind.Optional:
    case CelKind.Unknown:
    case CelKind.ListHost:
      throw new CelDecodeError(
        `decodeCelValue: kind ${kind} needs instance context ` +
          `(externref table / arena); decode it in instance.ts`,
      );
    default:
      throw new CelDecodeError(`decodeCelValue: unknown CelKind byte ${kind}`);
  }
}

/**
 * Encode an inline-scalar CelValue (null / bool / int / uint / double)
 * into the 24 bytes at `offset` (little-endian; `_pad` zeroed).
 * String/bytes need an arena allocation and are encoded by `instance.ts`;
 * passing one here throws `CelDecodeError`.
 */
export function encodeInlineScalar(
  mem: Uint8Array,
  offset: number,
  value: CelValue,
): void {
  const dv = viewOf(mem);
  dv.setUint32(offset, value.kind, true);
  dv.setUint32(offset + 4, 0, true);
  switch (value.kind) {
    case CelKind.Null:
      return;
    case CelKind.Bool:
      dv.setInt32(offset + PAYLOAD, value.bool ? 1 : 0, true);
      return;
    case CelKind.Int:
      dv.setBigInt64(offset + PAYLOAD, value.int, true);
      return;
    case CelKind.Uint:
      dv.setBigUint64(offset + PAYLOAD, value.uint, true);
      return;
    case CelKind.Double:
      dv.setFloat64(offset + PAYLOAD, value.double, true);
      return;
    // Not inline-encodable: string/bytes need an arena allocation,
    // error is produced by the runtime, never the activation marshaller.
    // Listed explicitly so `switch-exhaustiveness-check` proves a new
    // CelValue member can't silently fall through.
    case CelKind.String:
    case CelKind.Bytes:
    case CelKind.Error:
      throw new CelDecodeError(
        `encodeInlineScalar: kind ${value.kind} is not inline-encodable ` +
          `(string/bytes need an arena — use instance.ts)`,
      );
  }
}

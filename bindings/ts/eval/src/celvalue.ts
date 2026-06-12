// CelValue codec — read/write the 24-byte CelValue wire layout over a
// `DataView` of wasm linear memory.
//
// The layout is FROZEN by the C++/runtime side; every offset below is
// cited to `runtime/cel_data.h`.  All multi-byte reads are little-endian
// (`cel_data.h:202-210` — "Wasm memory is always little-endian"); i64/u64
// decode to `bigint`.
//
// Scope (WI-1.2, §A.4.1): the in-linear-memory kinds — NULL, BOOL, INT,
// UINT, DOUBLE, STRING, BYTES, TYPE, TIMESTAMP, DURATION, ERROR,
// LIST_ARENA, MAP_ARENA.  The externref-slot kinds (LIST_HOST, MAP_HOST,
// MESSAGE) need the externref table the assembly WI wires up; reading one
// here throws {@link CelExternrefBoundaryError}.  OPTIONAL / UNKNOWN are
// out of scope by design (§A.3) and throw {@link CelUnsupportedKindError},
// as do IP / CIDR (not in this binding's value surface).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.1.

import {
  ARENA_HEADER_COUNT_OFFSET,
  ARENA_HEADER_DATA_OFFSET,
  ARENA_LIST_ELEMENT_STRIDE,
  ARENA_MAP_ENTRY_STRIDE,
  CEL_DURTS_NANOS_OFFSET,
  CEL_DURTS_SECONDS_OFFSET,
  CEL_SPAN_LEN_OFFSET,
  CEL_SPAN_PTR_OFFSET,
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CelErrorCode,
  CelKind,
} from './types.js';
import type { CelError, CelValue } from './types.js';

/**
 * Thrown when {@link readCelValue} encounters an externref-slot kind
 * (LIST_HOST, MAP_HOST, MESSAGE).  Those payloads are a `ref_slot` /
 * `msg_slot` index into the host externref table, which the codec does
 * not own — the assembly WI resolves them against that table before
 * (or instead of) calling the codec.  The boundary is intentional: this
 * codec only decodes values whose bytes live in linear memory.
 */
export class CelExternrefBoundaryError extends Error {
  /** The externref slot index carried in the payload (for the resolver). */
  readonly slot: number;
  /** The CelKind that triggered the boundary. */
  readonly celKind: CelKind;

  constructor(celKind: CelKind, slot: number) {
    super(
      `CelValue kind ${String(celKind)} is an externref slot (index ` +
        `${String(slot)}) — needs the host externref table; the codec ` +
        `does not resolve it`,
    );
    this.name = 'CelExternrefBoundaryError';
    this.celKind = celKind;
    this.slot = slot;
  }
}

/**
 * Thrown for a CelKind outside this binding's static value surface —
 * OPTIONAL / UNKNOWN (out of scope per §A.3) or IP / CIDR (not a
 * decoded value in these bindings).
 */
export class CelUnsupportedKindError extends Error {
  /** The unsupported CelKind. */
  readonly celKind: number;

  constructor(celKind: number) {
    super(`CelValue kind ${String(celKind)} is out of scope for the codec`);
    this.name = 'CelUnsupportedKindError';
    this.celKind = celKind;
  }
}

const UTF8_DECODER = new TextDecoder('utf-8', { fatal: false });
const UTF8_ENCODER = new TextEncoder();

/**
 * Decode the span `{ptr:u32@8, len:u32@12}` at `offset` into a byte
 * slice of `memory` (`CelSpan`, `cel_data.h:54-57`).  Returns a *copy*
 * so the result is stable across wasm memory growth.
 */
function readSpanBytes(
  view: DataView,
  offset: number,
  memory: Uint8Array,
): Uint8Array {
  const ptr = view.getUint32(
    offset + CEL_SPAN_PTR_OFFSET,
    /* littleEndian */ true,
  );
  const len = view.getUint32(
    offset + CEL_SPAN_LEN_OFFSET,
    /* littleEndian */ true,
  );
  return memory.slice(ptr, ptr + len);
}

/**
 * Read an arena list (`CEL_LIST_ARENA`, `cel_data.h:90-102`).  The
 * payload holds a `header_ptr:u32@8` into linear memory; the 16-byte
 * header is `{count:u32, capacity:u32, elements_offset:u32, _pad:u32}`
 * (`cel_data.h:90-95`), and the elements are `count` × 24-byte CelValues
 * at `elements_offset`.
 */
function readArenaList(
  view: DataView,
  payloadOffset: number,
  memory: Uint8Array,
): CelValue[] {
  const headerPtr = view.getUint32(payloadOffset, /* littleEndian */ true);
  const count = view.getUint32(headerPtr + ARENA_HEADER_COUNT_OFFSET, true);
  const elementsOffset = view.getUint32(
    headerPtr + ARENA_HEADER_DATA_OFFSET,
    true,
  );
  const out: CelValue[] = [];
  for (let i = 0; i < count; i++) {
    const elementOffset = elementsOffset + i * ARENA_LIST_ELEMENT_STRIDE;
    out.push(readCelValue(view, elementOffset, memory));
  }
  return out;
}

/**
 * Read an arena map (`CEL_MAP_ARENA`, `cel_data.h:64-83`).  The payload
 * holds a `header_ptr:u32@8`; the 16-byte header is `{count, capacity,
 * entries_offset, _pad}`, and the entries are `count` × 48 bytes (a
 * 24-byte key CelValue immediately followed by a 24-byte value CelValue)
 * at `entries_offset`.
 */
function readArenaMap(
  view: DataView,
  payloadOffset: number,
  memory: Uint8Array,
): Map<CelValue, CelValue> {
  const headerPtr = view.getUint32(payloadOffset, /* littleEndian */ true);
  const count = view.getUint32(headerPtr + ARENA_HEADER_COUNT_OFFSET, true);
  const entriesOffset = view.getUint32(
    headerPtr + ARENA_HEADER_DATA_OFFSET,
    true,
  );
  const out = new Map<CelValue, CelValue>();
  for (let i = 0; i < count; i++) {
    const entryOffset = entriesOffset + i * ARENA_MAP_ENTRY_STRIDE;
    const key = readCelValue(view, entryOffset, memory);
    // The value CelValue starts one CelValue (24 B) after the key.
    const value = readCelValue(
      view,
      entryOffset + ARENA_LIST_ELEMENT_STRIDE,
      memory,
    );
    out.set(key, value);
  }
  return out;
}

/** Decode a TIMESTAMP / DURATION payload (`CelDurTs`, `cel_data.h:104-108`). */
function readDurTs(
  view: DataView,
  payloadOffset: number,
): { seconds: bigint; nanos: number } {
  const seconds = view.getBigInt64(
    payloadOffset + (CEL_DURTS_SECONDS_OFFSET - CEL_VALUE_PAYLOAD_OFFSET),
    true,
  );
  const nanos = view.getInt32(
    payloadOffset + (CEL_DURTS_NANOS_OFFSET - CEL_VALUE_PAYLOAD_OFFSET),
    true,
  );
  return { seconds, nanos };
}

/**
 * Read a CelValue at `(view, offset)`.  `memory` is a `Uint8Array` over
 * the SAME linear memory `view` covers — span / arena payloads are
 * pointers into it.  Switches on the `kind` u32 (`cel_data.h:143`) and
 * decodes each in-scope kind to its JS-natural shape.
 *
 * @throws {CelExternrefBoundaryError} for LIST_HOST / MAP_HOST / MESSAGE.
 * @throws {CelUnsupportedKindError} for OPTIONAL / UNKNOWN / IP / CIDR.
 */
export function readCelValue(
  view: DataView,
  offset: number,
  memory: Uint8Array,
): CelValue {
  const kind = view.getUint32(
    offset + CEL_VALUE_KIND_OFFSET,
    /* littleEndian */ true,
  ) as CelKind;
  const payload = offset + CEL_VALUE_PAYLOAD_OFFSET;
  switch (kind) {
    case CelKind.NULL:
      return null;
    case CelKind.BOOL:
      // `b` is an i32; any non-zero is true (`cel_data.h:146`).
      return view.getInt32(payload, true) !== 0;
    case CelKind.INT:
      return view.getBigInt64(payload, true);
    case CelKind.UINT:
      return view.getBigUint64(payload, true);
    case CelKind.DOUBLE:
      return view.getFloat64(payload, true);
    case CelKind.STRING:
      return UTF8_DECODER.decode(readSpanBytes(view, offset, memory));
    case CelKind.BYTES:
      return readSpanBytes(view, offset, memory);
    case CelKind.TYPE:
      // CEL_TYPE reuses the span arm: the payload points at the spec
      // type-name bytes ("int", a message FQN, …) — `cel_data.h:164-174`.
      return {
        kind: 'type',
        name: UTF8_DECODER.decode(readSpanBytes(view, offset, memory)),
      };
    case CelKind.TIMESTAMP: {
      const { seconds, nanos } = readDurTs(view, payload);
      return { kind: 'timestamp', epochSeconds: seconds, nanos };
    }
    case CelKind.DURATION: {
      const { seconds, nanos } = readDurTs(view, payload);
      return { kind: 'duration', seconds, nanos };
    }
    case CelKind.ERROR: {
      const code = view.getUint32(payload, true);
      return makeCelError(code);
    }
    case CelKind.LIST_ARENA:
      return readArenaList(view, payload, memory);
    case CelKind.MAP_ARENA:
      return readArenaMap(view, payload, memory);
    case CelKind.LIST_HOST:
    case CelKind.MAP_HOST:
    case CelKind.MESSAGE:
      throw new CelExternrefBoundaryError(kind, view.getUint32(payload, true));
    case CelKind.OPTIONAL:
    case CelKind.UNKNOWN:
    case CelKind.IP:
    case CelKind.CIDR:
      throw new CelUnsupportedKindError(kind);
    default:
      // Closed enum: an unknown discriminant is a wire-corruption /
      // version-skew bug, not a legitimate code path.
      throw new CelUnsupportedKindError(kind);
  }
}

/** Build a {@link CelError} from a wire error code, synthesizing the message. */
function makeCelError(code: number): CelError {
  return { kind: 'error', code, message: synthesizeErrorMessage(code) };
}

/**
 * Map a wire error code to a human-readable message.  The wire carries
 * only the numeric code (`cleanup-backlog #31`); the message is
 * synthesized host-side.  Codes mirror `runtime/cel_data.h:218-271`.
 */
export function synthesizeErrorMessage(code: CelErrorCode | number): string {
  // The wire carries a raw numeric code; treat it as the CelErrorCode it
  // encodes for the switch (the default arm handles an unknown code).
  switch (code as CelErrorCode) {
    case CelErrorCode.OVERFLOW:
      return 'integer overflow';
    case CelErrorCode.DIVIDE_BY_ZERO:
      return 'divide by zero';
    case CelErrorCode.MODULUS_BY_ZERO:
      return 'modulus by zero';
    case CelErrorCode.TYPE_MISMATCH:
      return 'no matching overload';
    case CelErrorCode.TYPE_UNSUPPORTED:
      return 'unsupported type';
    case CelErrorCode.NO_SUCH_KEY:
      return 'no such key';
    case CelErrorCode.DUPLICATE_KEY:
      return 'duplicate key in map literal';
    case CelErrorCode.INDEX_OUT_OF_BOUNDS:
      return 'index out of bounds';
    case CelErrorCode.INVALID_ARGUMENT:
      return 'invalid argument';
    case CelErrorCode.FIELD_NOT_FOUND:
      return 'no such field';
    case CelErrorCode.UNKNOWN_TYPE:
      return 'unknown type';
    case CelErrorCode.CUSTOM_FN_FAILED:
      return 'custom function failed';
    case CelErrorCode.HOST_ADAPTER_ERROR:
      return 'host adapter error';
    case CelErrorCode.TIMEOUT:
      return 'evaluation timed out';
    default:
      return `cel error (code ${String(code)})`;
  }
}

// ───────────────────────────────────────────────────────────────────
// Write helpers — encode a scalar CelValue at `(view, offset)`.  These
// feed the marshal (a variable's CelValue is written at its
// `slot_offset`).  Each writer stamps the kind u32 at offset 0 and the
// payload at offset 8; the 4-byte `_pad` at offset 4 is left untouched
// (the slot is zero-initialised by the runtime).
// ───────────────────────────────────────────────────────────────────

/** Write the `kind` u32 at offset 0 (`cel_data.h:143`). */
function writeKind(view: DataView, offset: number, kind: CelKind): void {
  view.setUint32(offset + CEL_VALUE_KIND_OFFSET, kind, /* littleEndian */ true);
}

/** Write a NULL CelValue (`CEL_NULL`, no payload). */
export function writeScalarNull(view: DataView, offset: number): void {
  writeKind(view, offset, CelKind.NULL);
}

/** Write a BOOL CelValue (`b` is an i32: 1 / 0). */
export function writeScalarBool(
  view: DataView,
  offset: number,
  value: boolean,
): void {
  writeKind(view, offset, CelKind.BOOL);
  view.setInt32(offset + CEL_VALUE_PAYLOAD_OFFSET, value ? 1 : 0, true);
}

/** Write an INT CelValue (`i` is an i64; `value` is a `bigint`). */
export function writeScalarInt(
  view: DataView,
  offset: number,
  value: bigint,
): void {
  writeKind(view, offset, CelKind.INT);
  view.setBigInt64(offset + CEL_VALUE_PAYLOAD_OFFSET, value, true);
}

/** Write a UINT CelValue (`u` is a u64; `value` is a `bigint`). */
export function writeScalarUint(
  view: DataView,
  offset: number,
  value: bigint,
): void {
  writeKind(view, offset, CelKind.UINT);
  view.setBigUint64(offset + CEL_VALUE_PAYLOAD_OFFSET, value, true);
}

/** Write a DOUBLE CelValue (`d` is an f64). */
export function writeScalarDouble(
  view: DataView,
  offset: number,
  value: number,
): void {
  writeKind(view, offset, CelKind.DOUBLE);
  view.setFloat64(offset + CEL_VALUE_PAYLOAD_OFFSET, value, true);
}

/**
 * Write a STRING / BYTES / TYPE span CelValue.  The codec does NOT
 * allocate — the caller passes a `ptr` into linear memory where the `len`
 * payload bytes already live (allocated via the runtime's `arena_alloc`);
 * this stamps the kind + the `{ptr, len}` span (`CelSpan`,
 * `cel_data.h:54-57`; CEL_TYPE reuses the span arm, `cel_data.h:164-174`).
 */
export function writeSpan(
  view: DataView,
  offset: number,
  kind: CelKind.STRING | CelKind.BYTES | CelKind.TYPE,
  ptr: number,
  len: number,
): void {
  writeKind(view, offset, kind);
  view.setUint32(offset + CEL_SPAN_PTR_OFFSET, ptr, true);
  view.setUint32(offset + CEL_SPAN_LEN_OFFSET, len, true);
}

/** UTF-8 encode `value` (the bytes the caller must place at `ptr`). */
export function encodeUtf8(value: string): Uint8Array {
  return UTF8_ENCODER.encode(value);
}

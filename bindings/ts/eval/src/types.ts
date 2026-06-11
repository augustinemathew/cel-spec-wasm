// The shared type contracts for the cel-wasm TypeScript bindings.
//
// This module is the single source of truth: every other package
// (compiler, conformance, web) and every downstream eval module imports
// its CelValue / CelInput / CelAbi / wire-constant definitions from here
// rather than re-declaring them.  The wire layout it encodes is FROZEN
// by the C++/runtime side; the constants below cite the authoritative
// headers and must be kept byte-for-byte in sync with them.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md
//       §A.4 (wire formats), §A.5 (canonical API).

import type * as protobuf from 'protobufjs';

// ───────────────────────────────────────────────────────────────────
// CelKind — the discriminant of a 24-byte CelValue.
//
// Stable, append-only on the wire; numeric values mirror the C enum in
// `runtime/cel_data.h:31-52` exactly.  A `const enum` so references
// inline to their literal at compile time and carry no runtime object.
// ───────────────────────────────────────────────────────────────────
export const enum CelKind {
  NULL = 0,
  BOOL = 1,
  INT = 2,
  UINT = 3,
  DOUBLE = 4,
  STRING = 5,
  BYTES = 6,
  LIST_ARENA = 7,
  MAP_ARENA = 8,
  MAP_HOST = 9,
  MESSAGE = 10,
  TYPE = 11,
  DURATION = 12,
  TIMESTAMP = 13,
  OPTIONAL = 14, // out of scope (§A.3)
  UNKNOWN = 15, // out of scope (§A.3) — pass through as error-ish
  ERROR = 16,
  LIST_HOST = 17,
  IP = 18,
  CIDR = 19,
}

// ───────────────────────────────────────────────────────────────────
// CelErrorCode — the numeric code carried in a CEL_ERROR CelValue.
//
// Mirrors the `enum` in `runtime/cel_data.h:218-271`.  Stable on the
// wire; append only, never renumber.  The human-readable message is NOT
// on the wire (cleanup-backlog #31) — the binding synthesizes it.
// ───────────────────────────────────────────────────────────────────
export const enum CelErrorCode {
  OVERFLOW = 10,
  DIVIDE_BY_ZERO = 11,
  MODULUS_BY_ZERO = 12,
  TYPE_MISMATCH = 13,
  TYPE_UNSUPPORTED = 14,
  NO_SUCH_KEY = 15,
  DUPLICATE_KEY = 16,
  INDEX_OUT_OF_BOUNDS = 17,
  INVALID_ARGUMENT = 18,
  FIELD_NOT_FOUND = 20,
  UNKNOWN_TYPE = 30,
  CUSTOM_FN_FAILED = 40,
  HOST_ADAPTER_ERROR = 41,
  TIMEOUT = 50,
}

// ───────────────────────────────────────────────────────────────────
// Byte-layout constants — the wire format is law (§A.6).
//
// Every offset / size / stride below is cited to `runtime/cel_data.h`
// and must round-trip against a real Program produced by the C++
// compiler (the codec's golden-fixture tests, WI-1.2).
// ───────────────────────────────────────────────────────────────────

/** Total size of a CelValue, in bytes (`cel_data.h:183`). */
export const CEL_VALUE_SIZE = 24;

/** Offset of the `kind` u32 within a CelValue (`cel_data.h:143`). */
export const CEL_VALUE_KIND_OFFSET = 0;

/**
 * Offset of the payload union within a CelValue (`cel_data.h:145`).
 * Bytes 4..8 are `_pad`; the union begins at 8 so 8-byte payloads
 * (i64 / u64 / f64) are naturally aligned.
 */
export const CEL_VALUE_PAYLOAD_OFFSET = 8;

/**
 * STRING / BYTES / TYPE span layout (`CelSpan`, `cel_data.h:54-57`):
 * a `{ ptr: u32, len: u32 }` pair starting at the payload offset.
 */
export const CEL_SPAN_PTR_OFFSET = CEL_VALUE_PAYLOAD_OFFSET; // 8
export const CEL_SPAN_LEN_OFFSET = CEL_VALUE_PAYLOAD_OFFSET + 4; // 12

/**
 * DURATION / TIMESTAMP layout (`CelDurTs`, `cel_data.h:104-108`):
 * `{ seconds: i64, nanos: i32, _pad: i32 }` starting at the payload
 * offset — seconds at 8, nanos at 16.
 */
export const CEL_DURTS_SECONDS_OFFSET = CEL_VALUE_PAYLOAD_OFFSET; // 8
export const CEL_DURTS_NANOS_OFFSET = CEL_VALUE_PAYLOAD_OFFSET + 8; // 16

/** Arena list/map header size (`cel_data.h:71-98`). */
export const ARENA_HEADER_SIZE = 16;
export const ARENA_HEADER_COUNT_OFFSET = 0;
export const ARENA_HEADER_CAPACITY_OFFSET = 4;
/** `elements_offset` (list) / `entries_offset` (map) sit at byte 8. */
export const ARENA_HEADER_DATA_OFFSET = 8;

/** Arena list element stride — one CelValue (`kCelListEntryStride`, `cel_data.h:195`). */
export const ARENA_LIST_ELEMENT_STRIDE = 24;

/** Arena map entry stride — key CelValue + value CelValue (`kCelMapEntryStride`, `cel_data.h:188`). */
export const ARENA_MAP_ENTRY_STRIDE = 48;

// ───────────────────────────────────────────────────────────────────
// CelValue — what eval gives you OUT (§A.5).
//
// A discriminated union over JS-natural shapes.  Scalars decode to
// their natural JS type; int64/uint64 decode to `bigint`; aggregates to
// arrays / `Map`s; messages to plain objects; the time / error kinds to
// tagged records.  OPTIONAL / UNKNOWN are out of scope and never appear.
// ───────────────────────────────────────────────────────────────────
export type CelValue =
  | null
  | boolean
  | bigint
  | number
  | string
  | Uint8Array
  | CelValue[]
  | Map<CelValue, CelValue>
  | { [field: string]: CelValue } // a message (protobufjs toObject)
  | CelTimestamp
  | CelDuration
  | CelError;

/** A decoded TIMESTAMP CelValue (§A.5). */
export interface CelTimestamp {
  readonly kind: 'timestamp';
  readonly epochSeconds: bigint;
  readonly nanos: number;
}

/** A decoded DURATION CelValue (§A.5). */
export interface CelDuration {
  readonly kind: 'duration';
  readonly seconds: bigint;
  readonly nanos: number;
}

/**
 * A decoded ERROR CelValue (§A.5).  `code` is a {@link CelErrorCode};
 * `message` is synthesized host-side (not on the wire — §A.4.1).
 */
export interface CelError {
  readonly kind: 'error';
  readonly code: number;
  readonly message: string;
}

// ───────────────────────────────────────────────────────────────────
// CelInput — what you bind IN (§A.5).
//
// JS-natural activation values.  A plain object bound to a message-typed
// variable is coerced to that proto via protobufjs `fromObject`
// (§A.4.6); an array/`Map`/object bound to a list/map-typed variable is
// interned as a host aggregate.
// ───────────────────────────────────────────────────────────────────
export type CelInput =
  | null
  | boolean
  | bigint
  | number
  | string
  | Uint8Array
  | CelInput[]
  | Map<CelInput, CelInput>
  | { [field: string]: CelInput }
  | protobuf.Message;

// ───────────────────────────────────────────────────────────────────
// The `cel.abi` descriptor (§A.4.3) — mirrors `abi/cel_abi.proto`.
//
// AttributeEntry is intentionally omitted: unknowns / partial eval are
// out of scope (§A.3), so the eval binding never reads that table.
// ───────────────────────────────────────────────────────────────────

/** Link mode a Program was compiled with (`cel_abi.proto` `LinkMode`). */
export const enum LinkMode {
  DYNAMIC = 0,
  STATIC = 1,
}

/**
 * One declared free variable — the marshal table row.  Bind `name`,
 * write its CelValue at `slotOffset`; `repr` selects the encoder.
 */
export interface VariableEntry {
  readonly name: string;
  readonly localIndex: number;
  readonly slotOffset: number;
  readonly repr: number;
}

/** One row of the field intern table (proto field reads, §A.4.5). */
export interface FieldEntry {
  readonly id: number;
  readonly fieldNumber: number;
  readonly name: string;
  readonly ownerFqn: string;
}

/** One row of the message-type intern table (proto literals). */
export interface TypeEntry {
  readonly id: number;
  readonly fullyQualifiedName: string;
}

/** The decoded `cel.abi` custom section (§A.4.3). */
export interface CelAbi {
  readonly version: number;
  readonly variables: readonly VariableEntry[];
  readonly fields: readonly FieldEntry[];
  readonly types: readonly TypeEntry[];
  readonly runtimeAbiVersion: number;
  readonly linkMode: LinkMode;
}

// ───────────────────────────────────────────────────────────────────
// A compiled Program — the portable artifact (§A.5).
// ───────────────────────────────────────────────────────────────────
export interface Program {
  /** The portable wasm artifact (downloadable, runnable in any engine). */
  readonly wasm: Uint8Array;
  /** The decoded `cel.abi` descriptor. */
  readonly abi: CelAbi;
}

// ───────────────────────────────────────────────────────────────────
// Host functions (§A.5) — `@host` custom functions as JS callbacks.
// Registered via `Engine.defineFunction`; invoked with already-decoded
// CelValue arguments, returning a CelValue (errors are returned as a
// CelError value, never thrown — the wire-format law of §A.4.5).
// ───────────────────────────────────────────────────────────────────
export type HostFunction = (...args: CelValue[]) => CelValue;

// ───────────────────────────────────────────────────────────────────
// MessageBacking — the thing an externref message slot points at
// (§A.4.6): a protobufjs `Message` + its `Type` (descriptor).  The
// `cel_host` proto/WKT trampolines operate over this interface; the
// concrete impl lands in WI-1.4b (`proto/backing.ts`).
// ───────────────────────────────────────────────────────────────────
export interface MessageBacking {
  /** The fully-qualified message type name (matches `TypeEntry.fullyQualifiedName`). */
  readonly typeName: string;

  /** Read a field by wire number or by name → a decoded CelValue. */
  readField(field: number | string): CelValue;

  /** proto2/proto3 presence of a field by wire number or by name. */
  hasField(field: number | string): boolean;

  /** Set a field (proto-literal construction) by wire number or by name. */
  setField(field: number | string, value: CelValue): void;
}

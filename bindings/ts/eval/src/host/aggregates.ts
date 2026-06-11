// cel_host — host-side list/map aggregate trampolines.
//
// The wasm expr module calls these via its `cel_host.*` imports when an
// operand is a HOST-backed list or map (a JS array / Map / object the
// marshal interned into the {@link ExternrefTable}, not bytes in linear
// memory).  Each import takes i32 *slot offsets* into linear memory,
// reads its operand CelValues, **absorbs UNKNOWN / ERROR on the inputs**
// (3VL — copy the poisoned operand straight to the out slot and return),
// runs the spec-level operation, and writes a CelValue result.  Spec
// errors (NO_SUCH_KEY, INDEX_OUT_OF_BOUNDS, TYPE_MISMATCH) are CEL_ERROR
// *values* written to the out slot — never thrown.
//
// The C++ originals are the authoritative contract:
//   - `eval/internal/cel_host.h:542-684` (the trampoline declarations).
//   - `eval/internal/cel_host.cc` (CelMapLookupImpl, CelListAtImpl,
//     CelListInImpl, CelMapInImpl, CelListSizeImpl, CelMapSizeImpl,
//     CelListEqImpl, CelMapEqImpl, CelListConcatImpl,
//     CelListIterOpenImpl, CelMapIterOpenImpl) — semantics mirrored
//     here line-for-line.
//   - `eval/internal/cel_host_error.cc:140-170` (the 3VL absorbers:
//     ERROR dominates UNKNOWN, the poisoned operand is copied through
//     verbatim).
//
// Value / map-key / element equality follows langdef §"Equality": same
// kind compares structurally; int/uint/double compare by mathematical
// value across the numeric ladder (mirrors `HostScalarValueEq` /
// `HostNumericCrossEq` in cel_host.cc).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.5.

import type { ExternrefTable } from '../externref.js';
import {
  ARENA_HEADER_CAPACITY_OFFSET,
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

// ───────────────────────────────────────────────────────────────────
// Host backings — what a list / map externref slot points at.
//
// The marshal (the assembly WI) interns a bound JS array / Map / object
// as one of these, holding the elements / entries as already-decoded
// CelValues.  The trampolines read them positionally (lists) or scan
// them (maps); equality uses CEL value equality, never JS `===`, so a
// numeric key matches across the int/uint/double ladder.
// ───────────────────────────────────────────────────────────────────

/**
 * A host-backed CEL list: its elements as decoded CelValues, in order.
 * Mirrors C++ `HostList` (`cel_host.h:287`) — vector-backed, insertion
 * order preserved, linear scan on membership.
 */
export interface HostListBacking {
  /** The list elements, in order. */
  readonly elements: readonly CelValue[];
}

/**
 * A host-backed CEL map: its entries as decoded `(key, value)` CelValue
 * pairs.  Mirrors C++ `HostMap` (`cel_host.h:211`) — vector-backed,
 * insertion order preserved, linear scan with CEL key equality on
 * lookup / membership.  Keys are the langdef-legal map-key kinds
 * (bool / int / uint / string); a string key is the decoded JS string.
 */
export interface HostMapBacking {
  /** The map entries, in insertion order. */
  readonly entries: readonly HostMapEntry[];
}

/** One `(key, value)` entry of a {@link HostMapBacking}. */
export interface HostMapEntry {
  /** The entry key (a bool / int / uint / string CelValue). */
  readonly key: CelValue;
  /** The entry value (any CelValue). */
  readonly value: CelValue;
}

// ───────────────────────────────────────────────────────────────────
// AggregateContext — what the assembly WI (WI-1.5) supplies so these
// trampolines can read operands, resolve host backings, and write
// results.  The context owns the parts that need the runtime (arena
// allocation, externref interning) so this module stays a pure
// operation layer over hand-buildable inputs.
// ───────────────────────────────────────────────────────────────────

/**
 * The dependencies the list/map trampolines need, bundled so the
 * assembly WI can satisfy them once and close every trampoline over the
 * same context.
 */
export interface AggregateContext {
  /**
   * A `DataView` over the Program's current linear memory.  A getter (not
   * the view itself) because wasm memory growth detaches the backing
   * `ArrayBuffer`; every trampoline re-reads it per call.
   */
  view(): DataView;

  /**
   * A `Uint8Array` over the SAME linear memory `view()` covers.  Used for
   * span (string / bytes) byte comparisons.  Same growth caveat — a
   * getter, re-read per call.
   */
  bytes(): Uint8Array;

  /** The three host-handle namespaces (message / map / list). */
  readonly refs: ExternrefTable;

  /**
   * Decode the CelValue at `slot` into its JS-natural shape, resolving
   * host (LIST_HOST / MAP_HOST / MESSAGE) slots against {@link refs}.
   * Supplied by the assembly WI because the plain codec
   * ({@link readCelValue}) throws on externref kinds — the resolving
   * read belongs to the layer that owns the externref table.  Used to
   * decode a map-lookup result element and the operands of equality.
   */
  readValue(slot: number): CelValue;

  /**
   * Encode `value` into the 24-byte CelValue at `slot`, handling every
   * kind: scalars inline, string / bytes arena-allocated, nested
   * list / map / message interned into {@link refs} as a host handle.
   * Supplied by the assembly WI because string / bytes / aggregate
   * writes need {@link arenaAlloc} + {@link refs}; the matching C++
   * surface is `EncodeFieldResult` / `EncodeValueToSlot`
   * (`cel_host.h:803`), which likewise lives above this layer.
   */
  writeValue(slot: number, value: CelValue): void;

  /**
   * Reserve `nBytes` of the runtime's per-Eval arena and return the
   * linear-memory offset of the reservation (the same allocator the
   * expr module uses via `arena_alloc`).  Returns 0 on OOM.  Used by
   * the iter_open trampolines to lay out the snapshot the comprehension
   * loop walks.
   */
  arenaAlloc(nBytes: number): number;
}

// ───────────────────────────────────────────────────────────────────
// Raw CelValue field access — read the discriminant / payload u32
// directly off the DataView, WITHOUT going through the resolving codec.
//
// The trampolines need the raw `kind` (to absorb poisoned operands and
// to dispatch on LIST_HOST / MAP_HOST) and the raw `ref_slot` (the first
// payload u32, `cel_data.h:155`) before they decide whether to decode.
// Reading these directly avoids invoking `readValue` on an operand that
// might be an ERROR/UNKNOWN we must pass through untouched.
// ───────────────────────────────────────────────────────────────────

/** Read the `kind` u32 of the CelValue at `slot` (`cel_data.h:143`). */
function readKind(view: DataView, slot: number): CelKind {
  return view.getUint32(
    slot + CEL_VALUE_KIND_OFFSET,
    /* littleEndian */ true,
  ) as CelKind;
}

/**
 * Read the first payload u32 of the CelValue at `slot` — the externref
 * `ref_slot` for a LIST_HOST / MAP_HOST value (`cel_data.h:155`).
 */
function readRefSlot(view: DataView, slot: number): number {
  return view.getUint32(
    slot + CEL_VALUE_PAYLOAD_OFFSET,
    /* littleEndian */ true,
  );
}

/** True iff `kind` is a poisoned operand the 3VL absorbers propagate. */
function isPoison(kind: CelKind): boolean {
  return kind === CelKind.ERROR || kind === CelKind.UNKNOWN;
}

/**
 * Copy the 24-byte CelValue at `srcSlot` to `dstSlot` verbatim — the
 * absorbers propagate a poisoned operand byte-for-byte (`AbsorbUnary` /
 * `AbsorbBinary`, `cel_host_error.cc:140`).
 */
function copyCelValue(view: DataView, srcSlot: number, dstSlot: number): void {
  for (let i = 0; i < CEL_VALUE_SIZE; i += 4) {
    view.setUint32(dstSlot + i, view.getUint32(srcSlot + i, true), true);
  }
}

/**
 * One-operand 3VL absorb (`AbsorbUnary`, `cel_host_error.cc:140`): if the
 * operand at `slot` is ERROR / UNKNOWN, copy it to `out` and return true.
 */
function absorbUnary(view: DataView, out: number, slot: number): boolean {
  if (isPoison(readKind(view, slot))) {
    copyCelValue(view, slot, out);
    return true;
  }
  return false;
}

/**
 * Two-operand 3VL absorb (`AbsorbBinary`, `cel_host_error.cc:148`):
 * ERROR dominates UNKNOWN, left operand dominates within a class.  If
 * either operand is poisoned, write the dominating one to `out` and
 * return true.
 */
function absorbBinary(
  view: DataView,
  out: number,
  a: number,
  b: number,
): boolean {
  const ka = readKind(view, a);
  const kb = readKind(view, b);
  if (ka === CelKind.ERROR) return copyCelValue(view, a, out), true;
  if (kb === CelKind.ERROR) return copyCelValue(view, b, out), true;
  if (ka === CelKind.UNKNOWN) return copyCelValue(view, a, out), true;
  if (kb === CelKind.UNKNOWN) return copyCelValue(view, b, out), true;
  return false;
}

// ───────────────────────────────────────────────────────────────────
// Result writers — the scalar shapes the trampolines stamp directly.
// Aggregate / string / message results route through `ctx.writeValue`
// (the assembly WI's encoder); these three cover BOOL / INT / ERROR,
// which is everything the size / in / eq / lookup-miss paths produce.
// ───────────────────────────────────────────────────────────────────

/** Write a BOOL CelValue at `slot` (`WriteWireBool`, `cel_host_error.cc:110`). */
function writeBool(view: DataView, slot: number, value: boolean): void {
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.BOOL, true);
  view.setInt32(slot + CEL_VALUE_PAYLOAD_OFFSET, value ? 1 : 0, true);
}

/** Write an INT CelValue at `slot` (`WriteWireInt`, `cel_host_error.cc:117`). */
function writeInt(view: DataView, slot: number, value: bigint): void {
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.INT, true);
  view.setBigInt64(slot + CEL_VALUE_PAYLOAD_OFFSET, value, true);
}

/** Write a CEL_ERROR CelValue at `slot` (`WriteWireError`, `cel_host_error.cc:103`). */
function writeError(view: DataView, slot: number, code: CelErrorCode): void {
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.ERROR, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, code, true);
}

// ───────────────────────────────────────────────────────────────────
// CEL value equality — the langdef §"Equality" rule the host backings
// use for `in` (element / key membership), map `eq`, and list `eq`.
//
// Mirrors `HostScalarValueEq` (`cel_host.cc:1839`): same kind compares
// structurally; int / uint / double compare by mathematical value
// (`HostNumericCrossEq`, `cel_host.cc:1814`).  Aggregates / messages /
// poison are not membership-matchable against a scalar query in this
// contract and compare unequal (the default arm of
// `BackingValueEqualsQuery`, `cel_host.cc:1988`).
// ───────────────────────────────────────────────────────────────────

/** Coerce an INT / UINT / DOUBLE CelValue to a JS number, or `undefined`. */
function asNumber(v: CelValue): number | undefined {
  if (typeof v === 'bigint') return Number(v);
  if (typeof v === 'number') return v;
  return undefined;
}

/** True iff `a` and `b` are the same numeric kind family (int/uint/double). */
function isNumeric(v: CelValue): boolean {
  return typeof v === 'bigint' || typeof v === 'number';
}

/**
 * Cross-numeric equality across the int/uint/double ladder by
 * mathematical value (`HostNumericCrossEq`, `cel_host.cc:1814`).  Both
 * operands must be numeric; compared as doubles, matching the C++
 * `da == db`.
 */
function numericCrossEq(a: CelValue, b: CelValue): boolean {
  const da = asNumber(a);
  const db = asNumber(b);
  if (da === undefined || db === undefined) return false;
  return da === db;
}

/** Byte-compare two `Uint8Array`s for BYTES equality. */
function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

/**
 * CEL value equality between two decoded CelValues (langdef §"Equality").
 * Scalar same-kind structural + numeric cross-ladder; bytes byte-wise;
 * timestamp / duration by (seconds, nanos); everything else (aggregate,
 * message, error, mismatched non-numeric kinds) compares unequal — the
 * scalar-only membership contract these trampolines implement.
 */
export function celValueEquals(a: CelValue, b: CelValue): boolean {
  // null
  if (a === null || b === null) return a === null && b === null;
  // Same JS primitive families.
  if (typeof a === 'boolean' || typeof b === 'boolean') {
    return a === b;
  }
  if (typeof a === 'string' || typeof b === 'string') {
    return a === b;
  }
  if (a instanceof Uint8Array || b instanceof Uint8Array) {
    return (
      a instanceof Uint8Array && b instanceof Uint8Array && bytesEqual(a, b)
    );
  }
  // int / uint / double — bigint and number both reach here; compare by
  // mathematical value across the ladder.
  if (isNumeric(a) && isNumeric(b)) {
    // Same-kind bigint↔bigint stays exact; any number operand falls to
    // the double comparison, matching HostNumericCrossEq.
    if (typeof a === 'bigint' && typeof b === 'bigint') return a === b;
    return numericCrossEq(a, b);
  }
  // timestamp / duration tagged records.
  if (isTimeRecord(a) && isTimeRecord(b)) {
    return timeRecordEquals(a, b);
  }
  // Aggregates, messages, errors, mismatched kinds: not equal.
  return false;
}

/** Narrow to a timestamp / duration tagged record. */
function isTimeRecord(
  v: CelValue,
): v is Extract<CelValue, { kind: 'timestamp' | 'duration' }> {
  return (
    typeof v === 'object' &&
    v !== null &&
    'kind' in v &&
    (v.kind === 'timestamp' || v.kind === 'duration')
  );
}

/** Equality of two timestamp / duration records by (seconds, nanos). */
function timeRecordEquals(
  a: Extract<CelValue, { kind: 'timestamp' | 'duration' }>,
  b: Extract<CelValue, { kind: 'timestamp' | 'duration' }>,
): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === 'timestamp' && b.kind === 'timestamp') {
    return a.epochSeconds === b.epochSeconds && a.nanos === b.nanos;
  }
  if (a.kind === 'duration' && b.kind === 'duration') {
    return a.seconds === b.seconds && a.nanos === b.nanos;
  }
  return false;
}

// ───────────────────────────────────────────────────────────────────
// Backing resolution.
// ───────────────────────────────────────────────────────────────────

/** Resolve the LIST_HOST backing for the value at `slot`, or `undefined`. */
function lookupList(
  ctx: AggregateContext,
  slot: number,
): HostListBacking | undefined {
  const ref = readRefSlot(ctx.view(), slot);
  return ctx.refs.list.lookup(ref) as HostListBacking | undefined;
}

/** Resolve the MAP_HOST backing for the value at `slot`, or `undefined`. */
function lookupMap(
  ctx: AggregateContext,
  slot: number,
): HostMapBacking | undefined {
  const ref = readRefSlot(ctx.view(), slot);
  return ctx.refs.map.lookup(ref) as HostMapBacking | undefined;
}

// ───────────────────────────────────────────────────────────────────
// Index coercion for `cel_list_at` (langdef §"Indexing").  The index is
// an INT; cel-cpp also admits a UINT and an integral DOUBLE under dyn
// (`CelListAtImpl`, `cel_host.cc:987-1020`).  Returns the i64 index, or
// an error code to poison the out slot with.
// ───────────────────────────────────────────────────────────────────

/** The outcome of coercing an index operand to an i64. */
type IndexResult =
  | { readonly ok: true; readonly index: bigint }
  | { readonly ok: false; readonly code: CelErrorCode };

const INT64_MAX = 9223372036854775807n;

/**
 * Coerce a decoded index CelValue to an i64 index per `CelListAtImpl`:
 * INT direct; UINT in range; integral finite DOUBLE truncated; anything
 * else TYPE_MISMATCH; out-of-i64-range UINT / non-integral DOUBLE the
 * matching INDEX_OUT_OF_BOUNDS / INVALID_ARGUMENT poison.
 */
function coerceIndex(idx: CelValue): IndexResult {
  if (typeof idx === 'bigint') {
    // INT and UINT both decode to bigint; UINT past i64-max is OOB.
    if (idx > INT64_MAX) {
      return { ok: false, code: CelErrorCode.INDEX_OUT_OF_BOUNDS };
    }
    return { ok: true, index: idx };
  }
  if (typeof idx === 'number') {
    // A DOUBLE index: must be finite and integral.
    if (!Number.isFinite(idx) || !Number.isInteger(idx)) {
      return { ok: false, code: CelErrorCode.INVALID_ARGUMENT };
    }
    return { ok: true, index: BigInt(idx) };
  }
  return { ok: false, code: CelErrorCode.TYPE_MISMATCH };
}

// ───────────────────────────────────────────────────────────────────
// The trampoline factory.
// ───────────────────────────────────────────────────────────────────

/**
 * Build the `cel_host` list/map aggregate trampolines closed over `ctx`.
 * The returned record maps each import name to its
 * `(...slots: number[]) => void` implementation, ready for the assembly
 * WI to merge into the `cel_host` import group.  Import names + arg
 * orders are the empirically-confirmed wire contract (the C++
 * `import_name` strings in `runtime/cel_*.c` / `cel_host.h`).
 */
export function makeAggregateTrampolines(
  ctx: AggregateContext,
): Record<string, (...args: number[]) => void> {
  return {
    cel_map_lookup: (out: number, map: number, key: number): void => {
      mapLookup(ctx, out, map, key);
    },
    cel_map_in: (out: number, key: number, map: number): void => {
      mapIn(ctx, out, key, map);
    },
    cel_map_size: (out: number, map: number): void => {
      mapSize(ctx, out, map);
    },
    cel_map_eq: (out: number, a: number, b: number): void => {
      mapEq(ctx, out, a, b);
    },
    cel_map_iter_open: (state: number, map: number): void => {
      mapIterOpen(ctx, state, map);
    },
    cel_list_at: (out: number, list: number, idx: number): void => {
      listAt(ctx, out, list, idx);
    },
    cel_list_in: (out: number, val: number, list: number): void => {
      listIn(ctx, out, val, list);
    },
    cel_list_size: (out: number, list: number): void => {
      listSize(ctx, out, list);
    },
    cel_list_eq: (out: number, a: number, b: number): void => {
      listEq(ctx, out, a, b);
    },
    cel_list_concat: (out: number, a: number, b: number): void => {
      listConcat(ctx, out, a, b);
    },
    cel_list_iter_open: (out: number, list: number): void => {
      listIterOpen(ctx, out, list);
    },
  };
}

// ───────────────────────────────────────────────────────────────────
// cel_map_lookup — (out, map, key): host map `Get(key)` → value or
// NO_SUCH_KEY.  3VL on operands (key first, then map — `CelMapLookupImpl`,
// cel_host.cc:1148).  Non-MAP_HOST or non-key-kind → TYPE_MISMATCH.
// ───────────────────────────────────────────────────────────────────
function mapLookup(
  ctx: AggregateContext,
  out: number,
  map: number,
  key: number,
): void {
  const view = ctx.view();
  if (absorbUnary(view, out, key)) return;
  if (absorbUnary(view, out, map)) return;
  if (readKind(view, map) !== CelKind.MAP_HOST) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const backing = lookupMap(ctx, map);
  if (backing === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const keyValue = ctx.readValue(key);
  if (!isLegalMapKey(keyValue)) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const hit = backing.entries.find((e) => celValueEquals(e.key, keyValue));
  if (hit === undefined) {
    writeError(view, out, CelErrorCode.NO_SUCH_KEY);
    return;
  }
  ctx.writeValue(out, hit.value);
}

// ───────────────────────────────────────────────────────────────────
// cel_map_in — (out, key, map): key ∈ map.keys → BOOL.  3VL binary
// (`CelMapInImpl`, cel_host.cc:2251).  Non-MAP_HOST or non-key-kind →
// TYPE_MISMATCH.
// ───────────────────────────────────────────────────────────────────
function mapIn(
  ctx: AggregateContext,
  out: number,
  key: number,
  map: number,
): void {
  const view = ctx.view();
  if (absorbBinary(view, out, key, map)) return;
  if (readKind(view, map) !== CelKind.MAP_HOST) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const backing = lookupMap(ctx, map);
  if (backing === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const keyValue = ctx.readValue(key);
  if (!isLegalMapKey(keyValue)) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const found = backing.entries.some((e) => celValueEquals(e.key, keyValue));
  writeBool(view, out, found);
}

// ───────────────────────────────────────────────────────────────────
// cel_map_size — (out, map): entry count → INT (`CelMapSizeImpl`,
// cel_host.cc:2233).  3VL unary.  Non-MAP_HOST → TYPE_MISMATCH.
// ───────────────────────────────────────────────────────────────────
function mapSize(ctx: AggregateContext, out: number, map: number): void {
  const view = ctx.view();
  if (absorbUnary(view, out, map)) return;
  if (readKind(view, map) !== CelKind.MAP_HOST) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const backing = lookupMap(ctx, map);
  if (backing === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  writeInt(view, out, BigInt(backing.entries.length));
}

// ───────────────────────────────────────────────────────────────────
// cel_map_eq — (out, a, b): set-equality of two host maps → BOOL
// (`CelMapEqImpl`, cel_host.cc:2394).  Order-irrelevant; key equality
// across the numeric ladder.  3VL binary; non-map operands →
// TYPE_MISMATCH.
// ───────────────────────────────────────────────────────────────────
function mapEq(ctx: AggregateContext, out: number, a: number, b: number): void {
  const view = ctx.view();
  if (absorbBinary(view, out, a, b)) return;
  if (
    readKind(view, a) !== CelKind.MAP_HOST ||
    readKind(view, b) !== CelKind.MAP_HOST
  ) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const ba = lookupMap(ctx, a);
  const bb = lookupMap(ctx, b);
  if (ba === undefined || bb === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  writeBool(view, out, mapsEqual(ba, bb));
}

/**
 * Set-equality of two host maps (`NormalizedMapEq`, cel_host.cc:2375):
 * same size, and every entry of `a` has a key-equal entry in `b` whose
 * value is value-equal.  Key and value equality both use
 * {@link celValueEquals}.
 */
function mapsEqual(a: HostMapBacking, b: HostMapBacking): boolean {
  if (a.entries.length !== b.entries.length) return false;
  for (const ea of a.entries) {
    const match = b.entries.find((eb) => celValueEquals(ea.key, eb.key));
    if (match === undefined || !celValueEquals(ea.value, match.value)) {
      return false;
    }
  }
  return true;
}

// ───────────────────────────────────────────────────────────────────
// cel_map_iter_open — (state, map): snapshot a host map into a flat
// arena run + write the 16-byte MapIterState the runtime walks
// (`CelMapIterOpenImpl`, cel_host.cc:1197; `MapIterState`,
// cel_runtime.c:1300).  Empty / OOM → count=0 (the runtime collapses to
// an empty iter).  An UNKNOWN / ERROR source is a codegen regression —
// the comprehension prologue absorbs poisoned ranges first — so it
// throws (the loud tripwire the C++ impl uses, replacing a silent
// wrong-answer empty range).
// ───────────────────────────────────────────────────────────────────

/** MapIterState field offsets (`cel_runtime.c:1300-1305`). */
const MAP_ITER_KIND_OFFSET = 0;
const MAP_ITER_CURSOR_OFFSET = 4;
const MAP_ITER_PAYLOAD_OFFSET = 8;
const MAP_ITER_COUNT_OFFSET = 12;
/** `MAP_ITER_KIND_HOST` (`cel_runtime.c:1293`). */
const MAP_ITER_KIND_HOST = 1;
/** Per-entry snapshot stride: key CelValue (24B) + value CelValue (24B). */
const MAP_ITER_HOST_ENTRY_BYTES = ARENA_MAP_ENTRY_STRIDE;

function mapIterOpen(ctx: AggregateContext, state: number, map: number): void {
  const view = ctx.view();
  const writeEmpty = (): void => {
    view.setUint32(state + MAP_ITER_KIND_OFFSET, MAP_ITER_KIND_HOST, true);
    view.setUint32(state + MAP_ITER_CURSOR_OFFSET, 0, true);
    view.setUint32(state + MAP_ITER_PAYLOAD_OFFSET, 0, true);
    view.setUint32(state + MAP_ITER_COUNT_OFFSET, 0, true);
  };
  const kind = readKind(view, map);
  if (isPoison(kind)) {
    throw new Error(
      `cel_map_iter_open: iter_range CelValue is ${
        kind === CelKind.UNKNOWN ? 'CEL_UNKNOWN' : 'CEL_ERROR'
      } — codegen's comprehension range-absorption guard must propagate ` +
        `it before the iterate path runs`,
    );
  }
  if (kind !== CelKind.MAP_HOST) {
    writeEmpty();
    return;
  }
  const backing = lookupMap(ctx, map);
  if (backing === undefined) {
    writeEmpty();
    return;
  }
  const entries = backing.entries;
  if (entries.length === 0) {
    writeEmpty();
    return;
  }
  const snapshotBytes = entries.length * MAP_ITER_HOST_ENTRY_BYTES;
  const snapshotOff = ctx.arenaAlloc(snapshotBytes);
  if (snapshotOff === 0) {
    writeEmpty();
    return;
  }
  for (let i = 0; i < entries.length; i++) {
    const entry = entries[i];
    if (entry === undefined) continue;
    const keyOff = snapshotOff + i * MAP_ITER_HOST_ENTRY_BYTES;
    const valOff = keyOff + CEL_VALUE_SIZE;
    ctx.writeValue(keyOff, entry.key);
    ctx.writeValue(valOff, entry.value);
  }
  view.setUint32(state + MAP_ITER_KIND_OFFSET, MAP_ITER_KIND_HOST, true);
  view.setUint32(state + MAP_ITER_CURSOR_OFFSET, 0, true);
  view.setUint32(state + MAP_ITER_PAYLOAD_OFFSET, snapshotOff, true);
  view.setUint32(state + MAP_ITER_COUNT_OFFSET, entries.length, true);
}

// ───────────────────────────────────────────────────────────────────
// cel_list_at — (out, list, idx): host list element or INDEX_OOB
// (`CelListAtImpl`, cel_host.cc:964).  3VL: index first, then list.
// ───────────────────────────────────────────────────────────────────
function listAt(
  ctx: AggregateContext,
  out: number,
  list: number,
  idx: number,
): void {
  const view = ctx.view();
  // Index absorbs first (matches the runtime fast-path order).
  if (absorbUnary(view, out, idx)) return;
  if (absorbUnary(view, out, list)) return;
  if (readKind(view, list) !== CelKind.LIST_HOST) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const coerced = coerceIndex(ctx.readValue(idx));
  if (!coerced.ok) {
    writeError(view, out, coerced.code);
    return;
  }
  const backing = lookupList(ctx, list);
  if (backing === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const i = coerced.index;
  if (i < 0n || i >= BigInt(backing.elements.length)) {
    writeError(view, out, CelErrorCode.INDEX_OUT_OF_BOUNDS);
    return;
  }
  const element = backing.elements[Number(i)];
  if (element === undefined) {
    writeError(view, out, CelErrorCode.INDEX_OUT_OF_BOUNDS);
    return;
  }
  ctx.writeValue(out, element);
}

// ───────────────────────────────────────────────────────────────────
// cel_list_in — (out, val, list): val ∈ list → BOOL (scalar value
// equality; `CelListInImpl`, cel_host.cc:1995).  3VL binary.
// ───────────────────────────────────────────────────────────────────
function listIn(
  ctx: AggregateContext,
  out: number,
  val: number,
  list: number,
): void {
  const view = ctx.view();
  if (absorbBinary(view, out, val, list)) return;
  if (readKind(view, list) !== CelKind.LIST_HOST) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const backing = lookupList(ctx, list);
  if (backing === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const query = ctx.readValue(val);
  const found = backing.elements.some((e) => celValueEquals(e, query));
  writeBool(view, out, found);
}

// ───────────────────────────────────────────────────────────────────
// cel_list_size — (out, list): element count → INT (`CelListSizeImpl`,
// cel_host.cc:1890).  3VL unary.
// ───────────────────────────────────────────────────────────────────
function listSize(ctx: AggregateContext, out: number, list: number): void {
  const view = ctx.view();
  if (absorbUnary(view, out, list)) return;
  if (readKind(view, list) !== CelKind.LIST_HOST) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const backing = lookupList(ctx, list);
  if (backing === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  writeInt(view, out, BigInt(backing.elements.length));
}

// ───────────────────────────────────────────────────────────────────
// cel_list_eq — (out, a, b): element-wise equality of two host lists →
// BOOL (`CelListEqImpl`, cel_host.cc:2164).  Equal length + positional
// element equality.  3VL binary; non-list operands → TYPE_MISMATCH.
// ───────────────────────────────────────────────────────────────────
function listEq(
  ctx: AggregateContext,
  out: number,
  a: number,
  b: number,
): void {
  const view = ctx.view();
  if (absorbBinary(view, out, a, b)) return;
  if (
    readKind(view, a) !== CelKind.LIST_HOST ||
    readKind(view, b) !== CelKind.LIST_HOST
  ) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const ba = lookupList(ctx, a);
  const bb = lookupList(ctx, b);
  if (ba === undefined || bb === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  writeBool(view, out, listsEqual(ba, bb));
}

/** Positional element-wise equality of two host lists (`WalkListEq`). */
function listsEqual(a: HostListBacking, b: HostListBacking): boolean {
  if (a.elements.length !== b.elements.length) return false;
  for (let i = 0; i < a.elements.length; i++) {
    const ea = a.elements[i];
    const eb = b.elements[i];
    if (ea === undefined || eb === undefined) return false;
    if (!celValueEquals(ea, eb)) return false;
  }
  return true;
}

// ───────────────────────────────────────────────────────────────────
// cel_list_concat — (out, a, b): two host lists → fresh arena LIST_ARENA
// (`CelListConcatImpl`, cel_host.cc:2189).  Materialises a+b into a fresh
// arena header + elements run.  3VL binary; non-list operands →
// TYPE_MISMATCH.
//
// The C++ ship state POISONs both-host / mixed-origin concat with
// TYPE_MISMATCH pending the re-entrant arena materialisation; the TS
// binding HAS an arena hook (`ctx.arenaAlloc`) + a full-CelValue writer
// (`ctx.writeValue`), so it materialises directly — there is no
// re-entrancy hazard in JS.
// ───────────────────────────────────────────────────────────────────
function listConcat(
  ctx: AggregateContext,
  out: number,
  a: number,
  b: number,
): void {
  const view = ctx.view();
  if (absorbBinary(view, out, a, b)) return;
  if (
    readKind(view, a) !== CelKind.LIST_HOST ||
    readKind(view, b) !== CelKind.LIST_HOST
  ) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const ba = lookupList(ctx, a);
  const bb = lookupList(ctx, b);
  if (ba === undefined || bb === undefined) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  const elements = [...ba.elements, ...bb.elements];
  writeArenaList(ctx, out, elements);
}

/**
 * Materialise `elements` into a fresh arena LIST_ARENA at `out`: a
 * 16-byte ArenaListHeader (`cel_data.h:90-95`) pointing at an
 * `n × 24-byte` CelValue run.  An empty list writes a zero-count header
 * (header_ptr non-zero) so the comprehension prologue reads count=0
 * cleanly.  On arena OOM, poisons with TYPE_MISMATCH (the concat
 * fallback the C++ impl uses for an unmaterialisable result).
 */
function writeArenaList(
  ctx: AggregateContext,
  out: number,
  elements: readonly CelValue[],
): void {
  const view = ctx.view();
  const headerOff = ctx.arenaAlloc(ARENA_HEADER_SIZE);
  if (headerOff === 0) {
    writeError(view, out, CelErrorCode.TYPE_MISMATCH);
    return;
  }
  let elementsOff = 0;
  if (elements.length > 0) {
    elementsOff = ctx.arenaAlloc(elements.length * ARENA_LIST_ELEMENT_STRIDE);
    if (elementsOff === 0) {
      writeError(view, out, CelErrorCode.TYPE_MISMATCH);
      return;
    }
  }
  view.setUint32(headerOff + ARENA_HEADER_COUNT_OFFSET, elements.length, true);
  view.setUint32(
    headerOff + ARENA_HEADER_CAPACITY_OFFSET,
    elements.length,
    true,
  );
  view.setUint32(headerOff + ARENA_HEADER_DATA_OFFSET, elementsOff, true);
  view.setUint32(headerOff + 12, 0, true); // _pad
  for (let i = 0; i < elements.length; i++) {
    const e = elements[i];
    if (e === undefined) continue;
    ctx.writeValue(elementsOff + i * ARENA_LIST_ELEMENT_STRIDE, e);
  }
  // Stamp the synthetic CEL_LIST_ARENA at out: kind + header_ptr.
  view.setUint32(out + CEL_VALUE_KIND_OFFSET, CelKind.LIST_ARENA, true);
  view.setUint32(out + CEL_VALUE_PAYLOAD_OFFSET, headerOff, true);
}

// ───────────────────────────────────────────────────────────────────
// cel_list_iter_open — (out, list): snapshot a host list into a fresh
// arena LIST_ARENA the comprehension prologue walks unchanged
// (`CelListIterOpenImpl`, cel_host.cc:1042).  Empty / non-host source →
// empty arena list (count=0, header_ptr non-zero).  Poisoned source
// throws (the codegen-regression tripwire).
// ───────────────────────────────────────────────────────────────────
function listIterOpen(ctx: AggregateContext, out: number, list: number): void {
  const view = ctx.view();
  const kind = readKind(view, list);
  if (isPoison(kind)) {
    throw new Error(
      `cel_list_iter_open: iter_range CelValue is ${
        kind === CelKind.UNKNOWN ? 'CEL_UNKNOWN' : 'CEL_ERROR'
      } — codegen's comprehension range-absorption guard must propagate ` +
        `it before the iterate path runs`,
    );
  }
  if (kind !== CelKind.LIST_HOST) {
    writeArenaList(ctx, out, []);
    return;
  }
  const backing = lookupList(ctx, list);
  if (backing === undefined) {
    writeArenaList(ctx, out, []);
    return;
  }
  writeArenaList(ctx, out, backing.elements);
}

// ───────────────────────────────────────────────────────────────────
// Map-key legality (langdef §"Maps": keys are bool / int / uint /
// string).  A non-key-kind query poisons TYPE_MISMATCH, mirroring
// `DecodeKey` returning nullopt (`cel_host.cc:779-796`).
// ───────────────────────────────────────────────────────────────────
function isLegalMapKey(key: CelValue): boolean {
  return (
    typeof key === 'boolean' ||
    typeof key === 'bigint' ||
    typeof key === 'string'
  );
}

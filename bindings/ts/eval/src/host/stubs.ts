// The non-trampoline host imports a STATIC Program needs to instantiate.
//
// A compiled static Program bundles the runtime (`cel.*`) but still
// imports four host-provided groups (§A.4.4): the `cel_host.*`
// trampolines, the `cel_fn.*` registered functions, `cel_env.cel_log`,
// and the WASI preview1 surface.  This module owns the last two — the
// "make it instantiate" groups that are NOT the cel_host trampolines:
//
//   - `cel_env.cel_log`  — a diagnostic sink.  Defaults to a no-op;
//     an optional logger receives the formatted message.  The wire
//     contract is `runtime/cel_log.h:46-50`: nine i32 args
//     (file_ptr, file_len, fn_ptr, fn_len, line, fmt_ptr, fmt_len,
//     argv_ptr, argc), no result.  The message is reconstructed from
//     linear memory exactly as the C++ host does
//     (`eval/host/cel_log.cc`).
//   - `wasi_snapshot_preview1.*` — minimal stubs.  The C++ engine
//     wires an empty WASI context; the static Program never exercises
//     real I/O at `$eval` time, so each import is a stub that returns
//     the WASI "success" errno (0) — enough for instantiation.
//
// The `cel_host.cel_timestamp_tz_accessor` trampoline is implemented
// here too (per §A.4.4), but it belongs to the `cel_host` import group
// owned by the assembly WI, so it is exported as a standalone factory
// rather than folded into `makeStubImports`.  Its civil-time projection
// (`computeCivilField`) is pure and fully tested; the surrounding
// CelValue out-slot write is left to the codec/assembly WI.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.4.

import { CelErrorCode } from '../types.js';

// ───────────────────────────────────────────────────────────────────
// Import module names — the wire contract the Program declares.
// Discovered empirically from the golden fixtures
// (`WebAssembly.Module.imports`).
// ───────────────────────────────────────────────────────────────────

/** The `cel_env.cel_log` import module name (`runtime/cel_log.h:46`). */
export const CEL_ENV_MODULE = 'cel_env';

/** The WASI preview1 import module name. */
export const WASI_MODULE = 'wasi_snapshot_preview1';

/**
 * The WASI preview1 import names a static Program declares.  Discovered
 * empirically from the golden fixtures; every fixture imports exactly
 * this set.  Each is stubbed to return WASI `errno` 0 (success) — the
 * Program never drives real I/O during `$eval`.
 */
export const WASI_IMPORT_NAMES = [
  'clock_time_get',
  'environ_get',
  'environ_sizes_get',
  'fd_close',
  'fd_fdstat_get',
  'fd_prestat_dir_name',
  'fd_prestat_get',
  'fd_read',
  'fd_seek',
  'fd_write',
  'poll_oneoff',
  'proc_exit',
  'random_get',
  'sched_yield',
] as const;

// ───────────────────────────────────────────────────────────────────
// cel_env.cel_log — the diagnostic sink.
// ───────────────────────────────────────────────────────────────────

/**
 * A decoded `cel_log` record handed to an optional logger.  `file` /
 * `fn` / `message` are reconstructed from linear memory; `line` is the
 * raw source line number.
 */
export interface CelLogRecord {
  readonly file: string;
  readonly fn: string;
  readonly line: number;
  readonly message: string;
}

/** Options for {@link makeStubImports}. */
export interface StubImportOptions {
  /**
   * Optional `cel_log` sink.  Receives the fully formatted message for
   * each runtime log call; when omitted, `cel_log` is a no-op.
   */
  readonly log?: (record: CelLogRecord) => void;

  /**
   * Resolves the instantiated Program's exported linear memory.  Needed
   * to decode `cel_log`'s pointer/length arguments; supplied by the
   * assembly WI after instantiation (the imports are built first, so it
   * is a getter, not the memory itself).  When absent — or while it
   * still resolves `undefined` — `cel_log` cannot read its operands and
   * is a no-op even if a `log` sink is set.
   */
  readonly memory?: () => WebAssembly.Memory | undefined;
}

// One `argv` slot is two 8-byte words: word 0's low 32 bits carry the
// tag, word 1 is the per-tag payload (`eval/host/cel_log.cc:24-28`).
const ARGV_SLOT_BYTES = 16;

// `CEL_LOG_TAG_*` values (`runtime/cel_log.h:35-42`).
const enum CelLogTag {
  STR = 1,
  INT = 2,
  UINT = 3,
  DOUBLE = 4,
  BOOL = 5,
  VALUE = 6,
}

// Reads a UTF-8 string from `[ptr, ptr+len)`; empty (with an `<oob>`
// marker for the caller to surface) when the span exceeds memory.
function readSpan(mem: Uint8Array, ptr: number, len: number): string {
  if (ptr + len > mem.length) return '';
  return new TextDecoder().decode(mem.subarray(ptr, ptr + len));
}

// Formats one argv slot per its tag, mirroring `ApplyDirective`
// (`eval/host/cel_log.cc:254-280`).  Only the scalar tags are rendered
// faithfully; the `%v` CelValue tag is summarised (full CelValue
// pretty-printing is the codec's job) so the sink still gets a message.
function formatSlot(mem: Uint8Array, view: DataView, slotOff: number): string {
  if (slotOff + ARGV_SLOT_BYTES > mem.length) return '<oob-arg>';
  const tag = view.getUint32(slotOff, true);
  const payload = view.getBigUint64(slotOff + 8, true);
  switch (tag as CelLogTag) {
    case CelLogTag.STR: {
      const ptr = Number(payload & 0xffffffffn);
      const len = Number(payload >> 32n);
      return ptr + len > mem.length ? '<oob>' : readSpan(mem, ptr, len);
    }
    case CelLogTag.INT:
      return BigInt.asIntN(64, payload).toString();
    case CelLogTag.UINT:
      return payload.toString();
    case CelLogTag.DOUBLE: {
      const f = new DataView(new ArrayBuffer(8));
      f.setBigUint64(0, payload, true);
      return String(f.getFloat64(0, true));
    }
    case CelLogTag.BOOL:
      return payload === 0n ? 'false' : 'true';
    case CelLogTag.VALUE:
      return `value(@${String(Number(payload & 0xffffffffn))})`;
    default:
      return `<tag=${String(tag)}>`;
  }
}

// Walks the format string pulling one argv slot per non-`%%` directive,
// mirroring `FormatMessage` (`eval/host/cel_log.cc:284-319`).
function formatMessage(
  mem: Uint8Array,
  fmt: string,
  argvPtr: number,
  argc: number,
): string {
  const view = new DataView(mem.buffer, mem.byteOffset, mem.byteLength);
  let out = '';
  let argIdx = 0;
  for (let i = 0; i < fmt.length; i++) {
    const c = fmt[i] ?? '';
    if (c !== '%') {
      out += c;
      continue;
    }
    const next = fmt[i + 1];
    if (next === undefined) {
      out += '%';
      break;
    }
    i++;
    if (next === '%') {
      out += '%';
      continue;
    }
    if (argIdx >= argc) {
      out += `%${next}`;
      continue;
    }
    out += formatSlot(mem, view, argvPtr + argIdx * ARGV_SLOT_BYTES);
    argIdx++;
  }
  return out;
}

// Builds the nine-i32 `cel_log` import.  Reads memory through the
// supplied getter; no-op when there is no sink or no memory.
function makeCelLog(opts: StubImportOptions): (...args: number[]) => void {
  const sink = opts.log;
  const memoryOf = opts.memory;
  return (
    filePtr: number,
    fileLen: number,
    fnPtr: number,
    fnLen: number,
    line: number,
    fmtPtr: number,
    fmtLen: number,
    argvPtr: number,
    argc: number,
  ): void => {
    if (sink === undefined) return;
    const memory = memoryOf?.();
    if (memory === undefined) return;
    const mem = new Uint8Array(memory.buffer);
    sink({
      file: readSpan(mem, filePtr, fileLen),
      fn: readSpan(mem, fnPtr, fnLen),
      line: line >>> 0,
      message: formatMessage(mem, readSpan(mem, fmtPtr, fmtLen), argvPtr, argc),
    });
  };
}

// ───────────────────────────────────────────────────────────────────
// WASI preview1 minimal stubs.
// ───────────────────────────────────────────────────────────────────

// `proc_exit` is the one WASI import that, if actually invoked, signals
// the guest asked to terminate — make it loud rather than silently
// returning, so a Program that calls it (it should not, during $eval)
// surfaces instead of hanging on a swallowed exit.
function makeWasiStubs(): WebAssembly.ModuleImports {
  const ok = (): number => 0;
  const stubs: Record<string, WebAssembly.ImportValue> = {};
  for (const name of WASI_IMPORT_NAMES) {
    stubs[name] = ok;
  }
  stubs.proc_exit = (code: number): never => {
    throw new Error(`wasi proc_exit(${String(code >>> 0)}) during $eval`);
  };
  return stubs;
}

// ───────────────────────────────────────────────────────────────────
// makeStubImports — the cel_env + WASI groups, ready to merge with the
// cel_host trampolines and cel_fn functions the assembly WI supplies.
// ───────────────────────────────────────────────────────────────────

/**
 * Builds the host-import groups a static Program needs to instantiate
 * OTHER than the `cel_host.*` trampolines and `cel_fn.*` functions:
 * `cel_env.cel_log` (a sink) and the WASI preview1 stubs.  The assembly
 * WI deep-merges these with its own `cel_host` / `cel_fn` groups before
 * calling `WebAssembly.instantiate`.
 */
export function makeStubImports(
  opts: StubImportOptions = {},
): WebAssembly.Imports {
  return {
    [CEL_ENV_MODULE]: { cel_log: makeCelLog(opts) },
    [WASI_MODULE]: makeWasiStubs(),
  };
}

// ───────────────────────────────────────────────────────────────────
// cel_host.cel_timestamp_tz_accessor — civil-time projection.
//
// Imported under `cel_host` (`runtime/cel_time.c:556-561`) as
// `(out_slot, ts_slot, tz_slot, accessor_kind) -> void`.  The accessor
// kind selects which civil field of the timestamp, projected into the
// IANA zone named by the `tz` string, is written as an INT CelValue.
// ───────────────────────────────────────────────────────────────────

/**
 * The accessor-kind discriminant (`runtime/cel_time.h:211-222`).  Closed
 * and append-only; the C runtime's shims pass these constants to the
 * single dispatch trampoline.
 */
export const enum CelTzAccessorKind {
  YEAR = 0,
  MONTH = 1,
  DAY_OF_MONTH_1 = 2,
  DAY_OF_MONTH = 3,
  DAY_OF_YEAR = 4,
  DAY_OF_WEEK = 5,
  HOURS = 6,
  MINUTES = 7,
  SECONDS = 8,
  MILLISECONDS = 9,
}

/**
 * The result of a civil-field projection.  `ok` carries the integer
 * field value; an invalid IANA zone yields an INVALID_ARGUMENT code the
 * caller writes as a CEL_ERROR CelValue.
 */
export type CivilFieldResult =
  | { readonly ok: true; readonly value: number }
  | { readonly ok: false; readonly code: CelErrorCode };

// Parts cache: one `Intl.DateTimeFormat` per (zone) is reused.
const formatterCache = new Map<string, Intl.DateTimeFormat>();

function zoneFormatter(zone: string): Intl.DateTimeFormat | undefined {
  const cached = formatterCache.get(zone);
  if (cached !== undefined) return cached;
  let fmt: Intl.DateTimeFormat;
  try {
    fmt = new Intl.DateTimeFormat('en-US', {
      timeZone: zone,
      year: 'numeric',
      month: 'numeric',
      day: 'numeric',
      hour: 'numeric',
      minute: 'numeric',
      second: 'numeric',
      weekday: 'short',
      hourCycle: 'h23',
    });
  } catch {
    return undefined;
  }
  formatterCache.set(zone, fmt);
  return fmt;
}

// CEL's day-of-week is 0=Sunday..6=Saturday (`doc/langdef.md`); map the
// `Intl` short weekday names to that range.
const WEEKDAY_INDEX: Readonly<Record<string, number>> = {
  Sun: 0,
  Mon: 1,
  Tue: 2,
  Wed: 3,
  Thu: 4,
  Fri: 5,
  Sat: 6,
};

function partsToFields(
  parts: Intl.DateTimeFormatPart[],
): Map<Intl.DateTimeFormatPartTypes, string> {
  const m = new Map<Intl.DateTimeFormatPartTypes, string>();
  for (const p of parts) m.set(p.type, p.value);
  return m;
}

function dayOfYear(year: number, month: number, day: number): number {
  const startUtc = Date.UTC(year, 0, 1);
  const dayUtc = Date.UTC(year, month - 1, day);
  return Math.floor((dayUtc - startUtc) / 86_400_000) + 1;
}

/**
 * Projects a timestamp (`epochSeconds`/`nanos` since the Unix epoch)
 * into the IANA `zone` and returns the civil field selected by `kind`.
 * Backed by `Intl.DateTimeFormat` for the zone's UTC offset + DST.  An
 * unrecognised zone yields INVALID_ARGUMENT.
 */
export function computeCivilField(
  epochSeconds: bigint,
  nanos: number,
  zone: string,
  kind: CelTzAccessorKind,
): CivilFieldResult {
  const fmt = zoneFormatter(zone);
  if (fmt === undefined) {
    return { ok: false, code: CelErrorCode.INVALID_ARGUMENT };
  }
  // Milliseconds since epoch; `Intl` operates at ms resolution, so the
  // sub-millisecond nanos only matter for the MILLISECONDS accessor.
  const ms = Number(epochSeconds) * 1000 + Math.floor(nanos / 1_000_000);
  const f = partsToFields(fmt.formatToParts(new Date(ms)));
  const num = (t: Intl.DateTimeFormatPartTypes): number => Number(f.get(t));
  const year = num('year');
  const month = num('month');
  const day = num('day');
  switch (kind) {
    case CelTzAccessorKind.YEAR:
      return { ok: true, value: year };
    case CelTzAccessorKind.MONTH:
      return { ok: true, value: month - 1 };
    case CelTzAccessorKind.DAY_OF_MONTH_1:
      return { ok: true, value: day };
    case CelTzAccessorKind.DAY_OF_MONTH:
      return { ok: true, value: day - 1 };
    case CelTzAccessorKind.DAY_OF_YEAR:
      return { ok: true, value: dayOfYear(year, month, day) - 1 };
    case CelTzAccessorKind.DAY_OF_WEEK:
      return { ok: true, value: WEEKDAY_INDEX[f.get('weekday') ?? ''] ?? 0 };
    case CelTzAccessorKind.HOURS:
      return { ok: true, value: num('hour') % 24 };
    case CelTzAccessorKind.MINUTES:
      return { ok: true, value: num('minute') };
    case CelTzAccessorKind.SECONDS:
      return { ok: true, value: num('second') };
    case CelTzAccessorKind.MILLISECONDS:
      return { ok: true, value: Math.floor(nanos / 1_000_000) };
  }
}

/**
 * A `cel_host.cel_timestamp_tz_accessor` trampoline.  This factory takes
 * the codec hooks the assembly WI owns — reading the timestamp + tz
 * operands out of `out`/`ts`/`tz` slots and writing the resulting INT or
 * CEL_ERROR CelValue — and returns the four-i32 import the Program
 * declares.  `computeCivilField` does the civil-time math; the read/write
 * of CelValue bytes is the codec's responsibility.
 */
export interface TzAccessorCodec {
  /** Reads the TIMESTAMP CelValue at `slot` → {epochSeconds, nanos}. */
  readonly readTimestamp: (slot: number) => {
    epochSeconds: bigint;
    nanos: number;
  };
  /** Reads the STRING CelValue at `slot` → the IANA zone name. */
  readonly readZone: (slot: number) => string;
  /** Writes an INT CelValue with `value` at `slot`. */
  readonly writeInt: (slot: number, value: number) => void;
  /** Writes a CEL_ERROR CelValue with `code` at `slot`. */
  readonly writeError: (slot: number, code: CelErrorCode) => void;
}

/**
 * Builds the `cel_timestamp_tz_accessor` import for the `cel_host` group
 * (merged by the assembly WI, not by {@link makeStubImports}).
 */
export function makeTimestampTzAccessor(
  codec: TzAccessorCodec,
): (out: number, ts: number, tz: number, kind: number) => void {
  return (out: number, ts: number, tz: number, kind: number): void => {
    const { epochSeconds, nanos } = codec.readTimestamp(ts);
    const zone = codec.readZone(tz);
    const result = computeCivilField(
      epochSeconds,
      nanos,
      zone,
      kind as CelTzAccessorKind,
    );
    if (result.ok) {
      codec.writeInt(out, result.value);
    } else {
      codec.writeError(out, result.code);
    }
  };
}

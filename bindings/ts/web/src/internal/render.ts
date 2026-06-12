// Render a decoded {@link CelValue} into a readable string for the Run
// panel.
//
// The eval binding returns JS-natural shapes (§A.5): scalars as their JS
// type, int64/uint64 as `bigint`, bytes as `Uint8Array`, aggregates as
// arrays / `Map`s, messages as plain objects, and the time / error kinds
// as tagged records.  This module is the display projection — it never
// throws on a value shape, so a surprising result is shown, not hidden.

import type {
  CelDuration,
  CelError,
  CelTimestamp,
  CelType,
  CelValue,
} from '@cel-wasm/eval';

/** The classification a caller styles the result panel by. */
export type ResultClass = 'value' | 'error';

/** A rendered result: a display string plus how to style it. */
export interface RenderedResult {
  readonly text: string;
  readonly className: ResultClass;
  /** The CEL type name of the value (`bool`, `int`, `list`, `error`, …). */
  readonly typeName: string;
}

/** Render a top-level result value for the Run panel. */
export function renderResult(value: CelValue): RenderedResult {
  if (isCelError(value)) {
    return {
      text: `error: ${value.message} (code ${String(value.code)})`,
      className: 'error',
      typeName: 'error',
    };
  }
  return {
    text: renderValue(value),
    className: 'value',
    typeName: typeNameOf(value),
  };
}

/** Render any {@link CelValue} to a compact, readable string. */
export function renderValue(value: CelValue): string {
  if (value === null) {
    return 'null';
  }
  if (typeof value === 'boolean') {
    return value ? 'true' : 'false';
  }
  if (typeof value === 'bigint') {
    return value.toString();
  }
  if (typeof value === 'number') {
    return renderNumber(value);
  }
  if (typeof value === 'string') {
    return JSON.stringify(value);
  }
  if (value instanceof Uint8Array) {
    return renderBytes(value);
  }
  if (Array.isArray(value)) {
    return `[${value.map(renderValue).join(', ')}]`;
  }
  if (value instanceof Map) {
    return renderMap(value);
  }
  if (isCelTimestamp(value)) {
    return renderTimestamp(value);
  }
  if (isCelDuration(value)) {
    return renderDuration(value);
  }
  if (isCelTypeValue(value)) {
    return value.name;
  }
  if (isCelError(value)) {
    return `error(${String(value.code)}): ${value.message}`;
  }
  return renderMessage(value);
}

function renderNumber(value: number): string {
  if (Number.isInteger(value) && Number.isFinite(value)) {
    // Render an integral double with a trailing `.0` so it is visibly a
    // double, matching CEL's distinction between `1` (int) and `1.0`.
    return `${value.toString()}.0`;
  }
  return value.toString();
}

function renderBytes(value: Uint8Array): string {
  const hex = Array.from(value, (b) => b.toString(16).padStart(2, '0')).join(
    '',
  );
  return `b"${hex}" (${String(value.length)} bytes)`;
}

function renderMap(value: Map<CelValue, CelValue>): string {
  const entries = Array.from(value.entries(), ([k, v]) => {
    return `${renderValue(k)}: ${renderValue(v)}`;
  });
  return `{${entries.join(', ')}}`;
}

function renderMessage(value: Record<string, CelValue>): string {
  const entries = Object.entries(value).map(([field, fieldValue]) => {
    return `${field}: ${renderValue(fieldValue)}`;
  });
  return `{${entries.join(', ')}}`;
}

function renderTimestamp(value: CelTimestamp): string {
  const millis = Number(value.epochSeconds) * 1000 + value.nanos / 1_000_000;
  const iso = Number.isFinite(millis)
    ? new Date(millis).toISOString()
    : `${value.epochSeconds.toString()}s`;
  return `timestamp(${iso})`;
}

function renderDuration(value: CelDuration): string {
  const seconds = Number(value.seconds) + value.nanos / 1e9;
  return `duration(${seconds.toString()}s)`;
}

/** The CEL type-name label for a non-error value. */
export function typeNameOf(value: CelValue): string {
  if (value === null) {
    return 'null';
  }
  if (typeof value === 'boolean') {
    return 'bool';
  }
  if (typeof value === 'bigint') {
    return 'int';
  }
  if (typeof value === 'number') {
    return 'double';
  }
  if (typeof value === 'string') {
    return 'string';
  }
  if (value instanceof Uint8Array) {
    return 'bytes';
  }
  if (Array.isArray(value)) {
    return 'list';
  }
  if (value instanceof Map) {
    return 'map';
  }
  if (isCelTimestamp(value)) {
    return 'timestamp';
  }
  if (isCelDuration(value)) {
    return 'duration';
  }
  if (isCelTypeValue(value)) {
    return 'type';
  }
  if (isCelError(value)) {
    return 'error';
  }
  return 'message';
}

function isCelError(value: CelValue): value is CelError {
  return isTagged(value, 'error');
}

function isCelTimestamp(value: CelValue): value is CelTimestamp {
  return isTagged(value, 'timestamp');
}

function isCelDuration(value: CelValue): value is CelDuration {
  return isTagged(value, 'duration');
}

function isCelTypeValue(value: CelValue): value is CelType {
  return isTagged(value, 'type');
}

function isTagged(value: CelValue, kind: string): boolean {
  return (
    typeof value === 'object' &&
    value !== null &&
    !Array.isArray(value) &&
    !(value instanceof Uint8Array) &&
    !(value instanceof Map) &&
    (value as { kind?: unknown }).kind === kind
  );
}

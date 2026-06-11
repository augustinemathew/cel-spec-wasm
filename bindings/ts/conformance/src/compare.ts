// Compares a decoded {@link CelValue} (what the eval binding returns) to
// a row's {@link ExpectedValue} matcher, following CEL value-equality
// semantics (`doc/langdef.md`) — the same rules the C++ `CompareValue`
// implements (`conformance/runner.cc`):
//
//   - NaN matches NaN (the only reflexive-NaN site in CEL).
//   - An eval ERROR value matches any `evalError` matcher (kind-only —
//     message text is not part of conformance per cel-cpp's run.cc).
//   - int64 / uint64 both decode to `bigint`; the matcher's int vs uint
//     kind is compared against the same numeric bigint.
//   - list equality is order-aware; map equality is order-agnostic.
//
// Returns `undefined` on a match, or a human-readable mismatch reason.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7;
//       doc/langdef.md (§Equality); conformance/runner.cc.

import type { CelError, CelValue } from '@cel-wasm/eval';

import type { ExpectedValue } from './corpus.js';

/** True when `got` is a decoded CEL eval-error value. */
export function isCelError(got: CelValue): got is CelError {
  return (
    typeof got === 'object' &&
    got !== null &&
    'kind' in got &&
    (got as { kind: unknown }).kind === 'error'
  );
}

/**
 * Compare a decoded result to an expected-value matcher.  Returns
 * `undefined` on a match, else a mismatch description.
 */
export function compareValue(
  got: CelValue,
  want: ExpectedValue,
): string | undefined {
  switch (want.kind) {
    case 'null':
      return got === null ? undefined : mismatch('null', got);
    case 'bool':
      return got === want.value
        ? undefined
        : mismatch(`bool ${String(want.value)}`, got);
    case 'int':
    case 'uint':
    case 'enum':
      return compareInteger(got, want.value);
    case 'double':
      return compareDouble(got, want.value);
    case 'string':
      return got === want.value
        ? undefined
        : mismatch(`string '${want.value}'`, got);
    case 'bytes':
      return compareBytes(got, want.value);
    case 'list':
      return compareList(got, want.elements);
    case 'map':
      return compareMap(got, want.entries);
    case 'type':
      return 'type matcher is out of value surface';
    case 'object':
      return 'object_value matcher is out of scope';
    case 'unrecognized':
      return want.reason;
  }
}

/** Compare a decoded error result to the `evalError` matcher (kind-only). */
export function compareEvalError(got: CelValue): string | undefined {
  return isCelError(got) ? undefined : mismatch('error', got);
}

function compareInteger(got: CelValue, want: bigint): string | undefined {
  if (typeof got === 'bigint' && got === want) {
    return undefined;
  }
  return mismatch(`integer ${want.toString()}`, got);
}

function compareDouble(got: CelValue, want: number): string | undefined {
  if (typeof got !== 'number') {
    return mismatch(`double ${String(want)}`, got);
  }
  // CEL: NaN matches NaN (langdef §Equality); -0 == 0.
  if (Number.isNaN(got) && Number.isNaN(want)) {
    return undefined;
  }
  return got === want ? undefined : mismatch(`double ${String(want)}`, got);
}

function compareBytes(got: CelValue, want: Uint8Array): string | undefined {
  if (!(got instanceof Uint8Array)) {
    return mismatch('bytes', got);
  }
  if (got.length !== want.length) {
    return `bytes length want=${String(want.length)} got=${String(got.length)}`;
  }
  for (let i = 0; i < want.length; i += 1) {
    if (got[i] !== want[i]) {
      return `bytes differ at index ${String(i)}`;
    }
  }
  return undefined;
}

function compareList(
  got: CelValue,
  want: readonly ExpectedValue[],
): string | undefined {
  if (!Array.isArray(got)) {
    return mismatch('list', got);
  }
  if (got.length !== want.length) {
    return `list size want=${String(want.length)} got=${String(got.length)}`;
  }
  for (let i = 0; i < want.length; i += 1) {
    const elem = got[i];
    const wantElem = want[i];
    if (elem === undefined || wantElem === undefined) {
      return `list[${String(i)}] missing`;
    }
    const reason = compareValue(elem, wantElem);
    if (reason !== undefined) {
      return `list[${String(i)}]: ${reason}`;
    }
  }
  return undefined;
}

interface MapEntry {
  readonly key: ExpectedValue;
  readonly value: ExpectedValue;
}

function compareMap(
  got: CelValue,
  want: readonly MapEntry[],
): string | undefined {
  if (!(got instanceof Map)) {
    return mismatch('map', got);
  }
  if (got.size !== want.length) {
    return `map size want=${String(want.length)} got=${String(got.size)}`;
  }
  const gotEntries = [...got.entries()];
  for (const wantEntry of want) {
    const match = gotEntries.find(
      ([gk]) => compareValue(gk, wantEntry.key) === undefined,
    );
    if (match === undefined) {
      return 'map key missing in result';
    }
    const reason = compareValue(match[1], wantEntry.value);
    if (reason !== undefined) {
      return `map value mismatch: ${reason}`;
    }
  }
  return undefined;
}

function mismatch(want: string, got: CelValue): string {
  return `want ${want}, got ${describe(got)}`;
}

function describe(got: CelValue): string {
  if (got === null) {
    return 'null';
  }
  if (Array.isArray(got)) {
    return `list[${String(got.length)}]`;
  }
  if (got instanceof Map) {
    return `map[${String(got.size)}]`;
  }
  if (got instanceof Uint8Array) {
    return `bytes[${String(got.length)}]`;
  }
  if (isCelError(got)) {
    return `error(code=${String(got.code)})`;
  }
  if (typeof got === 'object') {
    // A timestamp / duration tagged record or a decoded message object.
    if ('kind' in got) {
      return String((got as { kind: unknown }).kind);
    }
    return 'message';
  }
  // The remaining scalars: bigint / boolean / number / string.
  return `${typeof got} ${stringifyScalar(got)}`;
}

function stringifyScalar(got: bigint | boolean | number | string): string {
  return typeof got === 'bigint' ? got.toString() : String(got);
}

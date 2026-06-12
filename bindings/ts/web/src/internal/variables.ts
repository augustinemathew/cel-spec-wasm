// Parse the demo's variables form into a compile-time declaration plus a
// run-time activation binding.
//
// The form lets a user write one `name:type=value` row per variable.  The
// `name:type` half becomes a {@link VariableDecl} the compiler declares;
// the `=value` half becomes a JS-natural {@link CelInput} the activation
// binds.  Each type's literal grammar mirrors how that CEL type is bound
// in the §A.5 API: `int`/`uint` → `bigint`, `double` → `number`, `bool`
// → `boolean`, `string` → the raw text, `bytes` → UTF-8 of the text.

import type { VariableDecl } from '@cel-wasm/compiler';
import type { CelInput } from '@cel-wasm/eval';

/** One parsed variable row: its declaration and its bound value. */
export interface ParsedVariable {
  readonly decl: VariableDecl;
  readonly value: CelInput;
}

/** Thrown when a variables-form row is malformed. */
export class VariableParseError extends Error {
  override readonly name = 'VariableParseError';
}

const SCALAR_TYPES = new Set([
  'int',
  'uint',
  'double',
  'bool',
  'string',
  'bytes',
]);

/**
 * Parse a single `name:type=value` row.  Whitespace around each part is
 * trimmed; the value (everything after the first `=`) is taken verbatim
 * so a string value may itself contain `=` or `:`.
 *
 * @throws {VariableParseError} on a missing name/type, an unknown type, or
 *   a value that does not parse for its type.
 */
export function parseVariableRow(row: string): ParsedVariable {
  const eq = row.indexOf('=');
  if (eq < 0) {
    throw new VariableParseError(`row '${row}' must be 'name:type=value'`);
  }
  const decl = row.slice(0, eq);
  const rawValue = row.slice(eq + 1);
  const colon = decl.indexOf(':');
  if (colon < 0) {
    throw new VariableParseError(
      `declaration '${decl.trim()}' must be 'name:type'`,
    );
  }
  const name = decl.slice(0, colon).trim();
  const type = decl.slice(colon + 1).trim();
  if (name.length === 0) {
    throw new VariableParseError(`row '${row}' has an empty variable name`);
  }
  if (!SCALAR_TYPES.has(type)) {
    throw new VariableParseError(
      `unsupported type '${type}' for '${name}' ` +
        `(supported: ${[...SCALAR_TYPES].join(', ')})`,
    );
  }
  return { decl: { name, type }, value: parseValue(name, type, rawValue) };
}

/**
 * Parse the whole variables form (one row per non-blank line) into a list
 * of {@link ParsedVariable}s.  Blank lines are ignored so the form can be
 * spaced out for readability.
 *
 * @throws {VariableParseError} on the first malformed row, or on a
 *   duplicate variable name.
 */
export function parseVariablesForm(text: string): ParsedVariable[] {
  const parsed: ParsedVariable[] = [];
  const seen = new Set<string>();
  for (const rawLine of text.split('\n')) {
    const line = rawLine.trim();
    if (line.length === 0) {
      continue;
    }
    const variable = parseVariableRow(line);
    if (seen.has(variable.decl.name)) {
      throw new VariableParseError(
        `duplicate variable name '${variable.decl.name}'`,
      );
    }
    seen.add(variable.decl.name);
    parsed.push(variable);
  }
  return parsed;
}

function parseValue(name: string, type: string, raw: string): CelInput {
  const value = raw.trim();
  switch (type) {
    case 'int':
    case 'uint':
      return parseIntValue(name, type, value);
    case 'double':
      return parseDoubleValue(name, value);
    case 'bool':
      return parseBoolValue(name, value);
    case 'string':
      // The string value is taken verbatim (not trimmed) so trailing /
      // leading spaces a user intends are preserved.
      return raw;
    case 'bytes':
      return new TextEncoder().encode(raw);
    default:
      throw new VariableParseError(`unsupported type '${type}' for '${name}'`);
  }
}

function parseIntValue(name: string, type: string, value: string): bigint {
  if (!/^[+-]?\d+$/.test(value)) {
    throw new VariableParseError(
      `'${name}' (${type}) expects an integer, got '${value}'`,
    );
  }
  const parsed = BigInt(value);
  if (type === 'uint' && parsed < 0n) {
    throw new VariableParseError(`'${name}' (uint) must be non-negative`);
  }
  return parsed;
}

function parseDoubleValue(name: string, value: string): number {
  const parsed = Number(value);
  if (value.length === 0 || Number.isNaN(parsed)) {
    throw new VariableParseError(
      `'${name}' (double) expects a number, got '${value}'`,
    );
  }
  return parsed;
}

function parseBoolValue(name: string, value: string): boolean {
  if (value === 'true') {
    return true;
  }
  if (value === 'false') {
    return false;
  }
  throw new VariableParseError(
    `'${name}' (bool) expects 'true' or 'false', got '${value}'`,
  );
}

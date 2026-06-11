// Interprets the parsed textproto AST as the typed conformance corpus —
// `cel.expr.conformance.test.SimpleTestFile` → an ordered list of
// {@link SimpleTest} rows, each carrying the compile inputs (`expr`,
// declared `typeEnv`, `container`), the activation `bindings`, the
// expected result matcher, and the out-of-scope flags (`disableCheck`,
// `checkOnly`) the classifier reads.
//
// The expected-result model ({@link ExpectedValue}) mirrors the
// `cel.expr.Value` oneof the harness can compare against (§A.7); kinds
// the binding does not support (object/struct construction) are still
// modelled so the classifier can SKIP them with a category rather than
// the loader dropping the row silently.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7;
//       proto/cel/expr/conformance/test/simple.proto; proto/cel/expr/value.proto.

import type { CelInput } from '@cel-wasm/eval';

import {
  fieldValue,
  fieldValues,
  type TextprotoMessage,
  type TextprotoScalar,
} from './textproto.js';

/** A declared free variable for a row's type environment. */
export interface TypeEnvDecl {
  readonly name: string;
  /** The declared CEL type rendered in the compiler's surface syntax. */
  readonly type: DeclaredType;
}

/**
 * A declared variable's CEL type, in a shape the harness can either
 * render to the compiler's surface syntax or reject as out-of-scope.
 * `unsupported` carries the reason a decl can't be lowered (a function
 * decl, an abstract type, an unrepresentable shape).
 */
export type DeclaredType =
  | { readonly kind: 'primitive'; readonly name: string }
  | { readonly kind: 'message'; readonly fqn: string }
  | { readonly kind: 'wellKnown'; readonly name: string }
  | { readonly kind: 'list'; readonly elem: DeclaredType }
  | {
      readonly kind: 'map';
      readonly key: DeclaredType;
      readonly value: DeclaredType;
    }
  | { readonly kind: 'unsupported'; readonly reason: string };

/** The expected result of evaluating a row — the `cel.expr.Value` oneof. */
export type ExpectedValue =
  | { readonly kind: 'null' }
  | { readonly kind: 'bool'; readonly value: boolean }
  | { readonly kind: 'int'; readonly value: bigint }
  | { readonly kind: 'uint'; readonly value: bigint }
  | { readonly kind: 'double'; readonly value: number }
  | { readonly kind: 'string'; readonly value: string }
  | { readonly kind: 'bytes'; readonly value: Uint8Array }
  | { readonly kind: 'type'; readonly name: string }
  | { readonly kind: 'enum'; readonly value: bigint }
  | { readonly kind: 'list'; readonly elements: readonly ExpectedValue[] }
  | { readonly kind: 'map'; readonly entries: readonly ExpectedMapEntry[] }
  // object_value (proto construction) is out of scope (§A.3); modelled so
  // the classifier can SKIP rather than the loader dropping the row.
  | { readonly kind: 'object' }
  // A value message with no recognized kind (e.g. an empty `value {}`),
  // or one this model doesn't represent — carried as an out-of-envelope
  // sentinel so the classifier SKIPs rather than the loader throwing.
  | { readonly kind: 'unrecognized'; readonly reason: string };

/** One entry of an expected `map_value` matcher. */
export interface ExpectedMapEntry {
  readonly key: ExpectedValue;
  readonly value: ExpectedValue;
}

/**
 * The result matcher a row asserts.  `value` is a concrete expected
 * value; `evalError` matches any CEL eval error (kind-only, per the C++
 * harness); `boolTrue` is the implicit-true matcher (no matcher set);
 * `unsupported` carries the reason a matcher is out of envelope (unknown
 * / typed_result / any_*).
 */
export type ResultMatcher =
  | { readonly kind: 'value'; readonly value: ExpectedValue }
  | { readonly kind: 'evalError' }
  | { readonly kind: 'boolTrue' }
  | { readonly kind: 'unsupported'; readonly reason: string };

/** One conformance row, fully interpreted from textproto. */
export interface SimpleTest {
  readonly file: string;
  readonly section: string;
  readonly name: string;
  readonly expr: string;
  readonly container: string;
  readonly disableCheck: boolean;
  readonly checkOnly: boolean;
  readonly typeEnv: readonly TypeEnvDecl[];
  readonly bindings: ReadonlyMap<string, CelInput>;
  /**
   * Set when a `bindings` value is out of the harness's marshal scope
   * (e.g. a proto-message binding) — the classifier SKIPs the row with
   * this reason rather than the loader aborting the run.
   */
  readonly unsupportedBindingReason: string | undefined;
  readonly matcher: ResultMatcher;
}

/** Thrown when a row cannot be interpreted into the typed model. */
export class CorpusError extends Error {
  override readonly name = 'CorpusError';
}

/**
 * Interpret a parsed `SimpleTestFile` message into its ordered rows.
 * `fileStem` labels each row's origin (the textproto file's base name).
 */
export function loadSimpleTestFile(
  fileStem: string,
  doc: TextprotoMessage,
): readonly SimpleTest[] {
  const rows: SimpleTest[] = [];
  for (const section of fieldValues(doc, 'section')) {
    if (section.kind !== 'message') {
      continue;
    }
    const sectionName = stringField(section, 'name') ?? '';
    for (const test of fieldValues(section, 'test')) {
      if (test.kind === 'message') {
        rows.push(interpretTest(fileStem, sectionName, test));
      }
    }
  }
  // A few files carry top-level `test` rows outside any section.
  for (const test of fieldValues(doc, 'test')) {
    if (test.kind === 'message') {
      rows.push(interpretTest(fileStem, '', test));
    }
  }
  return rows;
}

// ───────────────────────────────────────────────────────────────────
// Row interpretation.
// ───────────────────────────────────────────────────────────────────

function interpretTest(
  file: string,
  section: string,
  test: TextprotoMessage,
): SimpleTest {
  const { bindings, unsupportedReason } = interpretBindings(test);
  return {
    file,
    section,
    name: stringField(test, 'name') ?? '<unnamed>',
    expr: stringField(test, 'expr') ?? '',
    container: stringField(test, 'container') ?? '',
    disableCheck: boolField(test, 'disable_check'),
    checkOnly: boolField(test, 'check_only'),
    typeEnv: interpretTypeEnv(test),
    bindings,
    unsupportedBindingReason: unsupportedReason,
    matcher: interpretMatcher(test),
  };
}

function interpretTypeEnv(test: TextprotoMessage): TypeEnvDecl[] {
  const decls: TypeEnvDecl[] = [];
  for (const decl of fieldValues(test, 'type_env')) {
    if (decl.kind !== 'message') {
      continue;
    }
    const name = stringField(decl, 'name') ?? '';
    const ident = fieldValue(decl, 'ident');
    if (ident === undefined || ident.kind !== 'message') {
      // A `function` decl (not `ident`) — out of scope; mark unsupported
      // so the classifier SKIPs the row as a type_env it can't lower.
      decls.push({
        name,
        type: { kind: 'unsupported', reason: 'non-ident type_env decl' },
      });
      continue;
    }
    const typeMsg = fieldValue(ident, 'type');
    if (typeMsg === undefined || typeMsg.kind !== 'message') {
      decls.push({
        name,
        type: { kind: 'unsupported', reason: 'ident decl without a type' },
      });
      continue;
    }
    decls.push({ name, type: interpretType(typeMsg) });
  }
  return decls;
}

function interpretType(typeMsg: TextprotoMessage): DeclaredType {
  const primitive = enumField(typeMsg, 'primitive');
  if (primitive !== undefined) {
    return { kind: 'primitive', name: primitive };
  }
  if (fieldValue(typeMsg, 'null') !== undefined) {
    return { kind: 'primitive', name: 'null_type' };
  }
  const message = stringField(typeMsg, 'message_type');
  if (message !== undefined) {
    return classifyMessageType(message);
  }
  const wk = enumField(typeMsg, 'well_known');
  if (wk !== undefined) {
    return { kind: 'wellKnown', name: wk };
  }
  const list = fieldValue(typeMsg, 'list_type');
  if (list !== undefined && list.kind === 'message') {
    const elem = fieldValue(list, 'elem_type');
    return {
      kind: 'list',
      elem:
        elem !== undefined && elem.kind === 'message'
          ? interpretType(elem)
          : { kind: 'unsupported', reason: 'list without elem_type' },
    };
  }
  const map = fieldValue(typeMsg, 'map_type');
  if (map !== undefined && map.kind === 'message') {
    return interpretMapType(map);
  }
  return { kind: 'unsupported', reason: 'unrepresentable type_env type' };
}

function interpretMapType(map: TextprotoMessage): DeclaredType {
  const key = fieldValue(map, 'key_type');
  const value = fieldValue(map, 'value_type');
  return {
    kind: 'map',
    key:
      key !== undefined && key.kind === 'message'
        ? interpretType(key)
        : { kind: 'unsupported', reason: 'map without key_type' },
    value:
      value !== undefined && value.kind === 'message'
        ? interpretType(value)
        : { kind: 'unsupported', reason: 'map without value_type' },
  };
}

// `google.protobuf.*` message types are the well-known types the
// compiler resolves natively; other message FQNs need a descriptor set
// the harness does not load (proto rows are out of scope, §A.3).
function classifyMessageType(fqn: string): DeclaredType {
  if (fqn.startsWith('google.protobuf.')) {
    return { kind: 'wellKnown', name: fqn };
  }
  return { kind: 'message', fqn };
}

interface InterpretedBindings {
  readonly bindings: Map<string, CelInput>;
  /** Set when a binding value is out of marshal scope (proto / non-value). */
  readonly unsupportedReason: string | undefined;
}

function interpretBindings(test: TextprotoMessage): InterpretedBindings {
  const bindings = new Map<string, CelInput>();
  for (const entry of fieldValues(test, 'bindings')) {
    if (entry.kind !== 'message') {
      continue;
    }
    const key = stringField(entry, 'key');
    if (key === undefined) {
      continue;
    }
    // bindings.value is an `ExprValue` wrapping `value { <Value> }`.
    const exprValue = fieldValue(entry, 'value');
    if (exprValue === undefined || exprValue.kind !== 'message') {
      continue;
    }
    const valueMsg = fieldValue(exprValue, 'value');
    if (valueMsg === undefined || valueMsg.kind !== 'message') {
      // An unknown / error binding — out of scope (§A.3).
      return {
        bindings,
        unsupportedReason: `binding '${key}' is not a concrete value`,
      };
    }
    try {
      bindings.set(key, expectedToInput(interpretValue(valueMsg)));
    } catch (err) {
      // A proto-message / type binding the harness can't marshal — defer
      // to the classifier as a SKIP rather than aborting the run.
      const reason = err instanceof Error ? err.message : String(err);
      return { bindings, unsupportedReason: `binding '${key}': ${reason}` };
    }
  }
  return { bindings, unsupportedReason: undefined };
}

function interpretMatcher(test: TextprotoMessage): ResultMatcher {
  const value = fieldValue(test, 'value');
  if (value !== undefined && value.kind === 'message') {
    return { kind: 'value', value: interpretValue(value) };
  }
  if (fieldValue(test, 'eval_error') !== undefined) {
    return { kind: 'evalError' };
  }
  if (fieldValue(test, 'any_eval_errors') !== undefined) {
    return { kind: 'evalError' };
  }
  if (fieldValue(test, 'unknown') !== undefined) {
    return { kind: 'unsupported', reason: 'unknown matcher (partial eval)' };
  }
  if (fieldValue(test, 'any_unknowns') !== undefined) {
    return {
      kind: 'unsupported',
      reason: 'any_unknowns matcher (partial eval)',
    };
  }
  if (fieldValue(test, 'typed_result') !== undefined) {
    return {
      kind: 'unsupported',
      reason: 'typed_result matcher (no-eval check)',
    };
  }
  // No matcher set → the implicit bool-true convention.
  return { kind: 'boolTrue' };
}

/** Interpret a `cel.expr.Value` message into the {@link ExpectedValue} model. */
export function interpretValue(value: TextprotoMessage): ExpectedValue {
  if (fieldValue(value, 'null_value') !== undefined) {
    return { kind: 'null' };
  }
  const bool = boolFieldOpt(value, 'bool_value');
  if (bool !== undefined) {
    return { kind: 'bool', value: bool };
  }
  const i = bigintField(value, 'int64_value');
  if (i !== undefined) {
    return { kind: 'int', value: i };
  }
  const u = bigintField(value, 'uint64_value');
  if (u !== undefined) {
    return { kind: 'uint', value: u };
  }
  const d = numberField(value, 'double_value');
  if (d !== undefined) {
    return { kind: 'double', value: d };
  }
  const s = stringField(value, 'string_value');
  if (s !== undefined) {
    // The textproto reader already UTF-8-decoded the field's exact bytes
    // (escapes encode UTF-8), so `value` is the CEL string verbatim.
    return { kind: 'string', value: s };
  }
  const b = bytesField(value, 'bytes_value');
  if (b !== undefined) {
    return { kind: 'bytes', value: b };
  }
  const t = stringField(value, 'type_value');
  if (t !== undefined) {
    return { kind: 'type', name: t };
  }
  return interpretAggregateValue(value);
}

function interpretAggregateValue(value: TextprotoMessage): ExpectedValue {
  const enumMsg = fieldValue(value, 'enum_value');
  if (enumMsg !== undefined && enumMsg.kind === 'message') {
    return { kind: 'enum', value: bigintField(enumMsg, 'value') ?? 0n };
  }
  const list = fieldValue(value, 'list_value');
  if (list !== undefined && list.kind === 'message') {
    return {
      kind: 'list',
      elements: fieldValues(list, 'values')
        .filter((v): v is TextprotoMessage => v.kind === 'message')
        .map(interpretValue),
    };
  }
  const map = fieldValue(value, 'map_value');
  if (map !== undefined && map.kind === 'message') {
    return interpretMapValue(map);
  }
  if (fieldValue(value, 'object_value') !== undefined) {
    return { kind: 'object' };
  }
  return { kind: 'unrecognized', reason: 'unrecognized cel.expr.Value kind' };
}

function interpretMapValue(map: TextprotoMessage): ExpectedValue {
  const entries: ExpectedMapEntry[] = [];
  for (const entry of fieldValues(map, 'entries')) {
    if (entry.kind !== 'message') {
      continue;
    }
    const key = fieldValue(entry, 'key');
    const value = fieldValue(entry, 'value');
    if (
      key === undefined ||
      key.kind !== 'message' ||
      value === undefined ||
      value.kind !== 'message'
    ) {
      continue;
    }
    entries.push({ key: interpretValue(key), value: interpretValue(value) });
  }
  return { kind: 'map', entries };
}

// ───────────────────────────────────────────────────────────────────
// Expected-value → activation input (for bindings).
// ───────────────────────────────────────────────────────────────────

/** Lower an {@link ExpectedValue} to the JS-natural {@link CelInput}. */
export function expectedToInput(value: ExpectedValue): CelInput {
  switch (value.kind) {
    case 'null':
      return null;
    case 'bool':
      return value.value;
    case 'int':
    case 'uint':
      return value.value;
    case 'enum':
      return value.value;
    case 'double':
      return value.value;
    case 'string':
      return value.value;
    case 'bytes':
      return value.value;
    case 'list':
      return value.elements.map(expectedToInput);
    case 'map': {
      const out = new Map<CelInput, CelInput>();
      for (const entry of value.entries) {
        out.set(expectedToInput(entry.key), expectedToInput(entry.value));
      }
      return out;
    }
    case 'type':
    case 'object':
      throw new CorpusError(`cannot bind a ${value.kind} value`);
    case 'unrecognized':
      throw new CorpusError(`cannot bind: ${value.reason}`);
  }
}

// ───────────────────────────────────────────────────────────────────
// Scalar field readers.
// ───────────────────────────────────────────────────────────────────

function scalarField(
  msg: TextprotoMessage,
  name: string,
): TextprotoScalar | undefined {
  const v = fieldValue(msg, name);
  return v !== undefined && v.kind !== 'message' ? v : undefined;
}

function stringField(msg: TextprotoMessage, name: string): string | undefined {
  const v = scalarField(msg, name);
  return v?.kind === 'string' ? v.value : undefined;
}

function bytesField(
  msg: TextprotoMessage,
  name: string,
): Uint8Array | undefined {
  const v = scalarField(msg, name);
  return v?.kind === 'string' ? v.bytes : undefined;
}

function boolField(msg: TextprotoMessage, name: string): boolean {
  return boolFieldOpt(msg, name) ?? false;
}

function boolFieldOpt(
  msg: TextprotoMessage,
  name: string,
): boolean | undefined {
  const v = scalarField(msg, name);
  return v?.kind === 'bool' ? v.value : undefined;
}

function numberField(msg: TextprotoMessage, name: string): number | undefined {
  const v = scalarField(msg, name);
  return v?.kind === 'number' ? v.value : undefined;
}

function bigintField(msg: TextprotoMessage, name: string): bigint | undefined {
  const v = scalarField(msg, name);
  if (v?.kind !== 'number') {
    return undefined;
  }
  try {
    return BigInt(v.raw);
  } catch {
    // A non-integer raw (a double in an int slot) — fall back to a
    // truncating conversion so the value is still usable.
    return BigInt(Math.trunc(v.value));
  }
}

function enumField(msg: TextprotoMessage, name: string): string | undefined {
  const v = scalarField(msg, name);
  return v?.kind === 'enum' ? v.value : undefined;
}

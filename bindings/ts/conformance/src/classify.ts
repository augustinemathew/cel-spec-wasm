// Scope + skip classification for conformance rows, mirroring the C++
// harness's `RunOne` / `ScopeReject` (`conformance/runner.cc`) and the
// §A.3 binding scope.  A row is classified BEFORE any compile burns when
// it is out of scope by construction:
//
//   - `disableCheck`  — parse-only eval (the harness compiles + checks).
//   - `checkOnly`     — typed_result no-eval check path.
//   - matcher out of envelope — unknown / typed_result / object_value.
//   - a type_env decl the renderer cannot lower (function / abstract).
//   - an extension-library expression the compiler has no decls for.
//
// Compile / eval failures are classified post-hoc in `runner.ts`; this
// module is the pre-compile gate plus the shared ext-lib detection.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.3,
//       §A.7; conformance/runner.cc.

import type { DeclaredType, SimpleTest } from './corpus.js';

/**
 * The skip categories, carried over from the C++ harness
 * (`runner.h::SkipCategory`) plus the binding-specific ones.  Each SKIP
 * carries one so the aggregate report groups by reason.
 */
export type SkipCategory =
  | 'disable_check' // parse-only eval, out of scope
  | 'check_only' // typed_result no-eval check
  | 'envelope' // matcher kind the harness has no comparator for
  | 'type_env' // a declared-variable type the renderer can't lower
  | 'bindings' // a binding value the harness can't marshal
  | 'object_value' // proto-construction matcher (out of scope, §A.3)
  | 'proto_unimpl' // proto message construction / field read (no descriptors)
  | 'ext_unimpl' // an extension-library expression (no decls registered)
  | 'static_subset' // a `dyn` / dynamic-typing construct (RejectDyn)
  | 'compile_unimpl' // compiler rejected a not-yet-supported construct
  | 'eval_unimpl' // eval rejected a not-yet-supported construct
  | 'spec_unimpl'; // a feature the CEL spec flags "specified but not implemented"

/** A pre-compile scope decision: either a SKIP or "proceed to compile". */
export type ScopeDecision =
  | {
      readonly kind: 'skip';
      readonly category: SkipCategory;
      readonly detail: string;
    }
  | { readonly kind: 'proceed'; readonly compileVars: readonly CompileVar[] };

/** A renderable compile variable — the compiler's `name:type` pair. */
export interface CompileVar {
  readonly name: string;
  readonly type: string;
}

/**
 * Decide a row's fate before compiling.  Returns a SKIP (with category +
 * detail) for out-of-scope rows, or `proceed` with the rendered compile
 * variables for an in-scope row.
 */
export function classifyScope(test: SimpleTest): ScopeDecision {
  if (isSpecUnimplRow(test)) {
    return skip(
      'spec_unimpl',
      `section '${test.section}' row '${test.name}' is spec-acknowledged ` +
        'unimplemented: strong enum typing (cel-cpp issues/119); ' +
        'cel-cpp’s own conformance harness skips it',
    );
  }
  if (test.disableCheck) {
    return skip('disable_check', 'parse-only eval out of conformance scope');
  }
  if (test.checkOnly) {
    return skip('check_only', 'typed_result no-eval check path');
  }
  const matcherSkip = classifyMatcher(test);
  if (matcherSkip !== undefined) {
    return matcherSkip;
  }
  if (test.unsupportedBindingReason !== undefined) {
    return skip('bindings', test.unsupportedBindingReason);
  }
  const vars: CompileVar[] = [];
  for (const decl of test.typeEnv) {
    const rendered = renderType(decl.type);
    if (rendered === undefined) {
      return skip(
        'type_env',
        `variable '${decl.name}': ${unsupportedReason(decl.type)}`,
      );
    }
    vars.push({ name: decl.name, type: rendered });
  }
  return { kind: 'proceed', compileVars: vars };
}

// Spec-acknowledged unimplemented rows: strong-typed enum support is a
// CEL-spec feature flagged "specified but not implemented" upstream
// (cel-cpp BUILD `_TESTS_TO_SKIP`, issues/119) — `type(<enum>)` must
// yield the enum FQN and the enum name must resolve as a callable
// int→enum / string→enum conversion, neither of which cel-cpp's checker
// implements.  Mirrors the C++ harness's per-row list
// (`conformance/runner.cc::IsSpecUnimplSection`) name-for-name; rows in
// these sections NOT on the list pass (or are satisfied by their error
// matcher) and must stay live.
const SPEC_UNIMPL_ENUM_ROWS: ReadonlySet<string> = new Set([
  'type_global',
  'type_nested',
  'field_type',
  'assign_standalone_int',
  'assign_standalone_int_big',
  'assign_standalone_int_neg',
  'convert_int_inrange',
  'convert_int_big',
  'convert_int_neg',
  'convert_string',
]);

/** Is this row on the spec-unimplemented list the C++ harness also skips? */
export function isSpecUnimplRow(test: SimpleTest): boolean {
  return (
    test.file === 'enums' &&
    (test.section === 'strong_proto2' || test.section === 'strong_proto3') &&
    SPEC_UNIMPL_ENUM_ROWS.has(test.name)
  );
}

function classifyMatcher(test: SimpleTest): ScopeDecision | undefined {
  const m = test.matcher;
  if (m.kind === 'unsupported') {
    return skip('envelope', m.reason);
  }
  // object_value matchers (proto construction) now run: the comparator
  // builds the expected message from the descriptor set (`proto-compare.ts`)
  // and deep-compares the decoded result.  They are no longer pre-skipped.
  if (m.kind === 'value' && m.value.kind === 'unrecognized') {
    return skip('envelope', m.value.reason);
  }
  // enum / type matchers and aggregates are in envelope (the comparator
  // handles them); evalError / boolTrue / concrete values proceed.
  return undefined;
}

/**
 * Render a declared CEL type to the compiler's `--var` surface syntax
 * (`int`, `list(int)`, `map(string, int)`, a message FQN).  Returns
 * `undefined` for a type the harness can't lower (a function decl, an
 * abstract type, a non-WKT message — proto rows are out of scope).
 */
export function renderType(type: DeclaredType): string | undefined {
  switch (type.kind) {
    case 'primitive':
      return renderPrimitive(type.name);
    case 'wellKnown':
      return renderWellKnown(type.name);
    case 'message':
      // A non-WKT message type resolves against the descriptor set the
      // harness now loads — render it as its fully-qualified name (the
      // compiler's `--var name:<fqn>` surface).
      return type.fqn;
    case 'list': {
      // The `cel` CLI's --var type grammar uses angle brackets
      // (`list<T>`, `map<K,V>`) — see tools/cel/var_parser.cc:ParseListT.
      const elem = renderType(type.elem);
      return elem === undefined ? undefined : `list<${elem}>`;
    }
    case 'map': {
      const key = renderType(type.key);
      const value = renderType(type.value);
      return key === undefined || value === undefined
        ? undefined
        : `map<${key}, ${value}>`;
    }
    case 'unsupported':
      return undefined;
  }
}

// The textproto `primitive` enum names → the CLI's type identifiers.
const PRIMITIVE_NAMES: ReadonlyMap<string, string> = new Map([
  ['BOOL', 'bool'],
  ['INT64', 'int'],
  ['UINT64', 'uint'],
  ['DOUBLE', 'double'],
  ['STRING', 'string'],
  ['BYTES', 'bytes'],
]);

function renderPrimitive(name: string): string | undefined {
  return PRIMITIVE_NAMES.get(name);
}

// Well-known types the compiler resolves natively: the time types map to
// the CLI primitives; every other `google.protobuf.*` WKT (the wrappers,
// Struct / Value / ListValue) is declared as its message FQN and
// resolves against the descriptor set — the same pass-through the C++
// harness uses (`conformance/binding_marshal.cc::CelTypeFromProtoType`,
// message_type arm).  `google.protobuf.Any` stays out: resolving a
// packed type-url beyond the supplied descriptors is out of binding
// scope by design (§A.3), so an Any-typed variable cannot round-trip.
const WELL_KNOWN_NAMES: ReadonlyMap<string, string> = new Map([
  ['google.protobuf.Duration', 'duration'],
  ['google.protobuf.Timestamp', 'timestamp'],
]);

function renderWellKnown(name: string): string | undefined {
  if (name === 'google.protobuf.Any') return undefined;
  return WELL_KNOWN_NAMES.get(name) ?? name;
}

function unsupportedReason(type: DeclaredType): string {
  if (type.kind === 'unsupported') {
    return type.reason;
  }
  if (type.kind === 'message') {
    return `message type '${type.fqn}' (no descriptor set loaded)`;
  }
  if (type.kind === 'wellKnown') {
    return `well-known type '${type.name}' (out of scope)`;
  }
  return `unrenderable ${type.kind} type`;
}

function skip(category: SkipCategory, detail: string): ScopeDecision {
  return { kind: 'skip', category, detail };
}

// ───────────────────────────────────────────────────────────────────
// Extension-library detection (mirrors runner.cc's ext-lib roots).
//
// A compile failure whose undeclared roots are all in an extension
// library the harness does not register is an ext-lib gap, not a
// regression.  Compile diagnostics here are text, so detection is
// source-shaped: a leading namespace token or a receiver method name.
// ───────────────────────────────────────────────────────────────────

const EXTENSION_NAMESPACE_ROOTS: ReadonlySet<string> = new Set([
  'cel',
  'block',
  'optional',
  'optional_type',
  'math',
  'strings',
  'base64',
  'ip',
  'cidr',
  'net',
  'proto',
]);

const EXTENSION_RECEIVER_ROOTS: ReadonlySet<string> = new Set([
  'charAt',
  'indexOf',
  'lastIndexOf',
  'substring',
  'replace',
  'split',
  'join',
  'lowerAscii',
  'upperAscii',
  'format',
  'reverse',
  'quote',
  'trim',
  'isIP',
  'isCIDR',
  'family',
  'isCanonical',
  'isUnspecified',
  'isLoopback',
  'isGlobalUnicast',
  'isLinkLocalMulticast',
  'isLinkLocalUnicast',
  'prefixLength',
  'containsIP',
  'containsCIDR',
  'masked',
]);

// Syntax shapes that exist only in cel-cpp extensions the harness does
// not register (the optionals + block_ext markers from runner.cc).
const EXTENSION_SYNTAX = [
  '.?',
  '[?',
  '{?',
  'cel.iterVar(',
  'cel.index(',
  'cel.block(',
];

/**
 * Heuristic: does this expression call into an extension library the
 * harness has no decls for?  Mirrors `runner.cc::LooksLikeExtensionSyntax`
 * + the namespace / receiver root checks.  Used to reclassify a compile
 * failure as an ext-lib SKIP rather than a FAIL.
 */
export function looksLikeExtension(expr: string): boolean {
  for (const marker of EXTENSION_SYNTAX) {
    if (expr.includes(marker)) {
      return true;
    }
  }
  for (const root of EXTENSION_NAMESPACE_ROOTS) {
    // A namespace call is `root.` at a token boundary.
    if (new RegExp(`(^|[^A-Za-z0-9_.])${escapeRegExp(root)}\\.`).test(expr)) {
      return true;
    }
  }
  for (const recv of EXTENSION_RECEIVER_ROOTS) {
    // A receiver call is `.method(`.
    if (expr.includes(`.${recv}(`)) {
      return true;
    }
  }
  return false;
}

function escapeRegExp(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// ───────────────────────────────────────────────────────────────────
// Proto-row detection.  Message construction, field reads, and WKT
// wrapper construction need a descriptor set the harness does not load
// (proto rows beyond supplied descriptors are out of scope, §A.3).  A
// row is a proto row when its container is a proto-conformance namespace
// OR its expression constructs / references a proto message type.
// ───────────────────────────────────────────────────────────────────

const PROTO_CONTAINER_PREFIXES = [
  'cel.expr.conformance.proto2',
  'cel.expr.conformance.proto3',
  'google.api.expr.test',
];

// Proto message construction / WKT wrapper construction: a type name (a
// dotted or bare identifier) immediately followed by `{` is a message
// literal; `TestAllTypes` / `NestedTestAllTypes` are the corpus fixtures.
const PROTO_CONSTRUCTION_RE =
  /(google\.protobuf\.\w+\s*\{|\bTestAllTypes\b|\bNestedTestAllTypes\b)/;

/**
 * Does this row build or read a proto message the harness has no
 * descriptor for?  Used to reclassify a compile / eval failure as a
 * proto SKIP rather than a FAIL.
 */
export function looksLikeProtoRow(expr: string, container: string): boolean {
  for (const prefix of PROTO_CONTAINER_PREFIXES) {
    if (container === prefix || container.startsWith(`${prefix}.`)) {
      return true;
    }
  }
  return PROTO_CONSTRUCTION_RE.test(expr);
}

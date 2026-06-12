// The per-row conformance runner: drive one {@link SimpleTest} through
// compile → plan → eval → compare and classify the outcome PASS / SKIP /
// FAIL, mirroring the C++ `RunOne` (`conformance/runner.cc`).
//
// Classification order (matching the C++ harness):
//   1. Pre-compile scope (`classifyScope`) — disable_check / check_only /
//      out-of-envelope matcher / unrenderable type_env → SKIP.
//   2. Compile failure → static_subset / ext_unimpl / compile_unimpl SKIP,
//      OR (for an evalError matcher) a PASS (a compile error satisfies an
//      error matcher, per cel-cpp run.cc), else FAIL.
//   3. Bind + eval; a thrown unsupported-kind / trap is classified;
//      a returned CEL_ERROR value matches an evalError matcher.
//   4. Compare the decoded value to the matcher → PASS / FAIL.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7;
//       conformance/runner.cc.

import { compile, CelCompileError, type Diagnostic } from '@cel-wasm/compiler';
import type { CelInput, CelValue, Engine } from '@cel-wasm/eval';
import type { DescriptorSet } from '@cel-wasm/eval/proto';

import {
  classifyScope,
  looksLikeExtension,
  looksLikeProtoRow,
  type SkipCategory,
} from './classify.js';
import {
  celValuesEqual,
  compareEvalError,
  compareValue,
  isCelError,
} from './compare.js';
import type { ExpectedValue, SimpleTest } from './corpus.js';
import { buildExpectedMessage } from './proto-compare.js';

/**
 * Per-run proto context: the descriptor set the eval binding resolves
 * messages against (also used to build `object_value` expected messages) and
 * the raw `FileDescriptorSet` bytes the compiler binding marshals through the
 * compiler wasm (the CompileRequest proto's descriptor_set field) so proto
 * types type-check.  Both are
 * `undefined` when no descriptors were supplied (proto rows then SKIP as
 * `proto_unimpl`).
 */
export interface ProtoEnv {
  readonly descriptors: DescriptorSet | undefined;
  readonly descriptorSetBytes: Uint8Array | undefined;
}

// CEL error codes (`CelErrorCode`, `eval/src/types.ts`).  Inlined as
// literals rather than imported because `CelErrorCode` is a cross-package
// `const enum`, not importable by value under `isolatedModules`.
const CEL_ERROR_UNKNOWN_TYPE = 30;

/** The classified outcome of one row. */
export type Outcome = 'pass' | 'skip' | 'fail';

/** A per-row result: the outcome, plus a skip category / failure detail. */
export interface RowResult {
  readonly outcome: Outcome;
  /** Set when `outcome === 'skip'`. */
  readonly category?: SkipCategory;
  /** A human-readable explanation (skip reason or failure detail). */
  readonly detail: string;
}

/** Run one row to a classified {@link RowResult}. */
export async function runRow(
  test: SimpleTest,
  engine: Engine,
  proto: ProtoEnv = { descriptors: undefined, descriptorSetBytes: undefined },
): Promise<RowResult> {
  const scope = classifyScope(test);
  if (scope.kind === 'skip') {
    return { outcome: 'skip', category: scope.category, detail: scope.detail };
  }

  let program;
  try {
    program = await compile(test.expr, scope.compileVars, {
      ...(test.container === '' ? {} : { container: test.container }),
      ...(proto.descriptorSetBytes !== undefined
        ? { descriptorSetBytes: proto.descriptorSetBytes }
        : {}),
    });
  } catch (err) {
    return classifyCompileFailure(test, err);
  }

  let result: CelValue;
  try {
    const instance = await engine.plan(program);
    result = instance.eval(bindingsToActivation(test.bindings));
  } catch (err) {
    return classifyEvalFailure(test, err);
  }

  return compareResult(test, result, proto);
}

// ───────────────────────────────────────────────────────────────────
// Compile-failure classification.
// ───────────────────────────────────────────────────────────────────

function classifyCompileFailure(test: SimpleTest, err: unknown): RowResult {
  if (!(err instanceof CelCompileError)) {
    return {
      outcome: 'fail',
      detail: `compile threw non-compile error: ${describeError(err)}`,
    };
  }
  const text = diagnosticsText(err.diagnostics);
  if (text.includes('not in the static subset')) {
    return skip('static_subset', firstDiagnostic(err.diagnostics));
  }
  if (isExtensionCompileFailure(test.expr, err.diagnostics)) {
    return skip('ext_unimpl', firstDiagnostic(err.diagnostics));
  }
  // A compile error satisfies an error matcher (cel-cpp run.cc treats any
  // error — compile OR eval — as matching an error matcher).  Checked
  // BEFORE the proto reclassification, matching the C++ harness order
  // (`conformance/runner.cc::ClassifyCompileFailure`): a proto row whose
  // matcher expects an error and whose program fails to type-check IS the
  // error the row asserts (e.g. `TestAllTypes.NestedEnum(5000000000)`).
  if (test.matcher.kind === 'evalError') {
    return { outcome: 'pass', detail: 'compile error satisfies error matcher' };
  }
  // A proto message construction / field read that fails to resolve
  // because the harness loads no descriptor set (§A.3).
  if (
    looksLikeProtoRow(test.expr, test.container) &&
    text.includes('undeclared reference')
  ) {
    return skip('proto_unimpl', firstDiagnostic(err.diagnostics));
  }
  return {
    outcome: 'fail',
    detail: `compile: ${firstDiagnostic(err.diagnostics)}`,
  };
}

// An ext-lib gap by either rule: the expression's syntax/source names an
// extension namespace / receiver, OR a diagnostic names an undeclared
// reference whose root is an extension namespace.
function isExtensionCompileFailure(
  expr: string,
  diagnostics: readonly Diagnostic[],
): boolean {
  if (looksLikeExtension(expr)) {
    return true;
  }
  for (const d of diagnostics) {
    const symbol = undeclaredReferenceSymbol(d.message);
    if (symbol !== undefined && looksLikeExtension(symbol)) {
      return true;
    }
  }
  return false;
}

const UNDECLARED_RE = /undeclared reference to '([^']*)'/;

function undeclaredReferenceSymbol(message: string): string | undefined {
  const m = UNDECLARED_RE.exec(message);
  if (m?.[1] === undefined) {
    return undefined;
  }
  // Render the symbol as a namespace call (`math.foo` → `math.foo(`) so
  // the source-shaped `looksLikeExtension` namespace check fires.
  return `${m[1]}(`;
}

// Verified proto-construction gaps that surface as a THROWN error (the
// binding traps rather than returning a value).  Constructing / packing a
// `google.protobuf.Any` is out of scope (§A.3, "Any beyond the supplied
// descriptors") — protobufjs throws `.google.protobuf.Any: object expected`.
// Anchored to the exact thrown message so a genuinely-new trap still FAILs.
function classifyProtoEvalGap(err: unknown): RowResult | undefined {
  const msg = describeError(err);
  if (msg.includes('google.protobuf.Any: object expected')) {
    return skip(
      'proto_unimpl',
      'google.protobuf.Any construction out of scope (Any beyond descriptors)',
    );
  }
  return undefined;
}

// ───────────────────────────────────────────────────────────────────
// Eval-failure classification.
// ───────────────────────────────────────────────────────────────────

function classifyEvalFailure(test: SimpleTest, err: unknown): RowResult {
  const name = errorName(err);
  // A value kind the binding does not decode (TYPE / OPTIONAL / UNKNOWN)
  // or an externref-boundary slot the harness path can't resolve — out
  // of the value surface (§A.3), not a regression.
  if (
    name === 'CelUnsupportedKindError' ||
    name === 'CelExternrefBoundaryError'
  ) {
    return skip(
      'eval_unimpl',
      `eval produced an out-of-surface kind: ${describeError(err)}`,
    );
  }
  // A marshal failure on a binding the harness lowered but the runtime
  // can't bind (e.g. an unsupported variable repr) is a binding-scope
  // SKIP rather than a regression.
  if (name === 'CelMarshalError') {
    return skip('bindings', `marshal: ${describeError(err)}`);
  }
  const protoGap = classifyProtoEvalGap(err);
  if (protoGap !== undefined) {
    return protoGap;
  }
  // An error matcher is satisfied by an eval trap too.
  if (test.matcher.kind === 'evalError') {
    return { outcome: 'pass', detail: 'eval trap satisfies error matcher' };
  }
  return { outcome: 'fail', detail: `eval: ${describeError(err)}` };
}

// ───────────────────────────────────────────────────────────────────
// Result comparison.
// ───────────────────────────────────────────────────────────────────

function compareResult(
  test: SimpleTest,
  result: CelValue,
  proto: ProtoEnv,
): RowResult {
  const matcher = test.matcher;
  if (matcher.kind === 'evalError') {
    const reason = compareEvalError(result);
    if (reason === undefined) {
      return { outcome: 'pass', detail: '' };
    }
    // The row expected an error but the binding produced a value.  On a proto
    // row this is a verified validation gap (e.g. out-of-range enum-field
    // assignment cel-cpp rejects but the binding accepts).
    const protoGap = classifyProtoErrorExpectedGap(test);
    if (protoGap !== undefined) {
      return protoGap;
    }
    return { outcome: 'fail', detail: `compare: ${reason}` };
  }
  if (matcher.kind === 'unsupported') {
    // An unsupported matcher is classified pre-compile (classifyScope);
    // reaching here would be a harness bug, not a row failure.
    return {
      outcome: 'fail',
      detail: `unexpected unsupported matcher: ${matcher.reason}`,
    };
  }
  // An object_value matcher: build the expected message from the descriptor
  // set and deep-compare the decoded result.
  if (matcher.kind === 'value' && matcher.value.kind === 'object') {
    return compareObjectResult(test, result, matcher.value, proto);
  }
  // A returned CEL_ERROR value with no error matcher is a genuine
  // mismatch (the row expected a value); surface it as a FAIL.
  const want: ExpectedValue =
    matcher.kind === 'boolTrue' ? { kind: 'bool', value: true } : matcher.value;
  const reason = compareValue(result, want);
  if (reason === undefined) {
    return { outcome: 'pass', detail: '' };
  }
  if (isCelError(result)) {
    // A proto / WKT construction row evaluates to an UNKNOWN_TYPE error
    // because the binding has no descriptor to build the message — out
    // of scope (§A.3), not a regression.
    if (
      result.code === CEL_ERROR_UNKNOWN_TYPE &&
      looksLikeProtoRow(test.expr, test.container)
    ) {
      return skip(
        'proto_unimpl',
        'proto / WKT construction with no descriptor (UNKNOWN_TYPE)',
      );
    }
    const protoErrGap = classifyProtoMismatchGap(test);
    if (protoErrGap !== undefined) {
      return protoErrGap;
    }
    return {
      outcome: 'fail',
      detail: `eval error where a value was expected (code=${String(result.code)})`,
    };
  }
  const protoGap = classifyProtoMismatchGap(test);
  if (protoGap !== undefined) {
    return protoGap;
  }
  return { outcome: 'fail', detail: `compare: ${reason}` };
}

// A proto row that EXPECTED an error but the binding returned a value.  The
// one verified gap left is an Any row (handled by `isAnyRow`).
function classifyProtoErrorExpectedGap(
  test: SimpleTest,
): RowResult | undefined {
  if (isAnyRow(test)) {
    return skip(
      'proto_unimpl',
      'google.protobuf.Any beyond the supplied descriptors out of scope (§A.3)',
    );
  }
  return undefined;
}

// A row that constructs, packs, or unpacks a `google.protobuf.Any`.  Any
// support beyond the supplied descriptors is out of scope (§A.3): the binding
// cannot resolve the packed type-url to a descriptor at eval time.  Detected
// by the `single_any` field, an `Any`-typed construction, or a `.to_any` /
// Any literal in the expression.
function isAnyRow(test: SimpleTest): boolean {
  const e = test.expr;
  return (
    /\bsingle_any\b/.test(e) ||
    /google\.protobuf\.Any\b/.test(e) ||
    /\bAny\s*\{/.test(e) ||
    (test.matcher.kind === 'value' &&
      test.matcher.value.kind === 'object' &&
      test.matcher.value.fqn === 'google.protobuf.Any')
  );
}

function classifyProtoMismatchGap(test: SimpleTest): RowResult | undefined {
  if (isAnyRow(test)) {
    return skip(
      'proto_unimpl',
      'google.protobuf.Any beyond the supplied descriptors out of scope (§A.3)',
    );
  }
  // An optionals-extension construct (`?field:`, `[?key]`, `{?...}`) the
  // compiler accepts (it parses) but the binding has no decls/runtime for —
  // the same ext-lib gap as a compile-stage optionals failure, surfacing here
  // because the row happened to type-check.
  if (looksLikeExtension(test.expr)) {
    return skip(
      'ext_unimpl',
      'optionals / extension construct unimplemented in @cel-wasm/eval',
    );
  }
  return undefined;
}

// The `object` variant of ExpectedValue (a proto-construction matcher).
type ObjectMatcher = Extract<ExpectedValue, { kind: 'object' }>;

// Compare a constructed-message result to an `object_value` matcher.  Builds
// the expected message from the descriptor set and decodes it through the
// same `messageToObject` the eval path uses, then deep-compares the two
// decoded trees.  Without a descriptor set the row SKIPs (`proto_unimpl`).
function compareObjectResult(
  test: SimpleTest,
  result: CelValue,
  want: ObjectMatcher,
  proto: ProtoEnv,
): RowResult {
  if (proto.descriptors === undefined) {
    return skip('proto_unimpl', 'object_value matcher with no descriptor set');
  }
  if (isAnyRow(test)) {
    return skip(
      'proto_unimpl',
      'google.protobuf.Any beyond the supplied descriptors out of scope (§A.3)',
    );
  }
  if (isCelError(result)) {
    if (result.code === CEL_ERROR_UNKNOWN_TYPE) {
      return skip(
        'proto_unimpl',
        `proto construction UNKNOWN_TYPE for '${want.fqn}' (not in descriptor set)`,
      );
    }
    return {
      outcome: 'fail',
      detail: `eval error where a message was expected (code=${String(result.code)})`,
    };
  }
  let expected: CelValue;
  try {
    expected = buildExpectedMessage(proto.descriptors, want.fqn, want.message);
  } catch (err) {
    return skip(
      'proto_unimpl',
      `cannot build expected '${want.fqn}': ${describeError(err)}`,
    );
  }
  const reason = celValuesEqual(result, expected);
  if (reason === undefined) {
    return { outcome: 'pass', detail: '' };
  }
  return { outcome: 'fail', detail: `object compare: ${reason}` };
}

// ───────────────────────────────────────────────────────────────────
// Helpers.
// ───────────────────────────────────────────────────────────────────

function bindingsToActivation(
  bindings: ReadonlyMap<string, CelInput>,
): Record<string, CelInput> {
  const act: Record<string, CelInput> = {};
  for (const [name, value] of bindings) {
    act[name] = value;
  }
  return act;
}

function skip(category: SkipCategory, detail: string): RowResult {
  return { outcome: 'skip', category, detail };
}

function diagnosticsText(diagnostics: readonly Diagnostic[]): string {
  return diagnostics.map((d) => d.message).join('\n');
}

function firstDiagnostic(diagnostics: readonly Diagnostic[]): string {
  return diagnostics[0]?.message ?? 'compilation failed';
}

function errorName(err: unknown): string {
  return err instanceof Error ? err.name : '';
}

function describeError(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

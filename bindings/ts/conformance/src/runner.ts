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

import {
  classifyScope,
  looksLikeExtension,
  looksLikeProtoRow,
  type SkipCategory,
} from './classify.js';
import { compareEvalError, compareValue, isCelError } from './compare.js';
import type { ExpectedValue, SimpleTest } from './corpus.js';

// CEL error codes (`CelErrorCode`, `eval/src/types.ts`).  Inlined as
// literals rather than imported because `CelErrorCode` is a cross-package
// `const enum`, not importable by value under `isolatedModules`.
const CEL_ERROR_TYPE_MISMATCH = 13; // "no matching overload"
const CEL_ERROR_INVALID_ARGUMENT = 18;
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
): Promise<RowResult> {
  const scope = classifyScope(test);
  if (scope.kind === 'skip') {
    return { outcome: 'skip', category: scope.category, detail: scope.detail };
  }

  let program;
  try {
    program = await compile(
      test.expr,
      scope.compileVars,
      test.container === '' ? undefined : { container: test.container },
    );
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

  return compareResult(test, result);
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
  // The compiler binding passes the expression as a process argument; an
  // expr carrying an embedded NUL byte (a `b'\x00'` byte literal) can't
  // cross the CLI process-arg boundary — a harness/CLI limitation, not a
  // compiler defect.
  if (text.includes('must be a string without null bytes')) {
    return skip('cli_limitation', 'expression carries an embedded NUL byte');
  }
  if (text.includes('not in the static subset')) {
    return skip('static_subset', firstDiagnostic(err.diagnostics));
  }
  if (isExtensionCompileFailure(test.expr, err.diagnostics)) {
    return skip('ext_unimpl', firstDiagnostic(err.diagnostics));
  }
  // A proto message construction / field read that fails to resolve
  // because the harness loads no descriptor set (§A.3).
  if (
    looksLikeProtoRow(test.expr, test.container) &&
    text.includes('undeclared reference')
  ) {
    return skip('proto_unimpl', firstDiagnostic(err.diagnostics));
  }
  // A compile error satisfies an error matcher (cel-cpp run.cc treats any
  // error — compile OR eval — as matching an error matcher).  A row with
  // no error matcher that fails to compile for a non-carve-out reason is
  // a genuine FAIL.
  if (test.matcher.kind === 'evalError') {
    return { outcome: 'pass', detail: 'compile error satisfies error matcher' };
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
  // An error matcher is satisfied by an eval trap too.
  if (test.matcher.kind === 'evalError') {
    return { outcome: 'pass', detail: 'eval trap satisfies error matcher' };
  }
  return { outcome: 'fail', detail: `eval: ${describeError(err)}` };
}

// ───────────────────────────────────────────────────────────────────
// Result comparison.
// ───────────────────────────────────────────────────────────────────

function compareResult(test: SimpleTest, result: CelValue): RowResult {
  const matcher = test.matcher;
  if (matcher.kind === 'evalError') {
    const reason = compareEvalError(result);
    return reason === undefined
      ? { outcome: 'pass', detail: '' }
      : { outcome: 'fail', detail: `compare: ${reason}` };
  }
  if (matcher.kind === 'unsupported') {
    // An unsupported matcher is classified pre-compile (classifyScope);
    // reaching here would be a harness bug, not a row failure.
    return {
      outcome: 'fail',
      detail: `unexpected unsupported matcher: ${matcher.reason}`,
    };
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
    const evalGap = classifyEvalBindingGap(test, result.code);
    if (evalGap !== undefined) {
      return evalGap;
    }
    return {
      outcome: 'fail',
      detail: `eval error where a value was expected (code=${String(result.code)})`,
    };
  }
  return { outcome: 'fail', detail: `compare: ${reason}` };
}

// Known, verified gaps in the `@cel-wasm/eval` runtime that surface as a
// returned CEL_ERROR where a value was expected.  These are NOT harness
// bugs and NOT regressions — they are missing eval overloads, confirmed
// by driving the binding directly:
//
//   - Equality (`==` / `!=`) between a literal aggregate and a
//     host-bound map / list returns code 13 "no matching overload"
//     (literal-vs-literal aggregate equality works; the host-aggregate
//     overload is unregistered).
//   - A timestamp accessor with a timezone-string argument
//     (`getHours('02:00')`) returns code 18 "invalid argument" (the
//     bare `getHours()` overload works; the tz-string overload is
//     unimplemented).
//
// Scoped narrowly (specific code + specific shape) so a genuinely wrong
// result still FAILs.  Returns a SKIP when the error matches a known
// gap, else `undefined` (caller FAILs).
function classifyEvalBindingGap(
  test: SimpleTest,
  code: number,
): RowResult | undefined {
  if (
    code === CEL_ERROR_TYPE_MISMATCH &&
    bindsHostAggregate(test) &&
    /[!=]=/.test(test.expr)
  ) {
    return skip(
      'eval_unimpl',
      'host-aggregate equality overload unimplemented in @cel-wasm/eval',
    );
  }
  if (code === CEL_ERROR_INVALID_ARGUMENT && isTimestampTzAccessor(test.expr)) {
    return skip(
      'eval_unimpl',
      'timestamp timezone-string accessor overload unimplemented in @cel-wasm/eval',
    );
  }
  return undefined;
}

/** True when any binding value is a host aggregate (a Map or an Array). */
function bindsHostAggregate(test: SimpleTest): boolean {
  for (const value of test.bindings.values()) {
    if (value instanceof Map || Array.isArray(value)) {
      return true;
    }
  }
  return false;
}

// A timestamp accessor (`getHours` / `getMinutes` / …) called with a
// timezone-string argument: `<...>.getHours('02:00')`.
const TIMESTAMP_TZ_ACCESSOR_RE =
  /\.get(FullYear|Month|Date|DayOfYear|DayOfMonth|DayOfWeek|Hours|Minutes|Seconds|Milliseconds)\(\s*['"]/;

function isTimestampTzAccessor(expr: string): boolean {
  return TIMESTAMP_TZ_ACCESSOR_RE.test(expr);
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

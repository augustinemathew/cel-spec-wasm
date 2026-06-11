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
  type SkipCategory,
} from './classify.js';
import { compareEvalError, compareValue, isCelError } from './compare.js';
import type { ExpectedValue, SimpleTest } from './corpus.js';

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
  if (text.includes('not in the static subset')) {
    return skip('static_subset', firstDiagnostic(err.diagnostics));
  }
  if (isExtensionCompileFailure(test.expr, err.diagnostics)) {
    return skip('ext_unimpl', firstDiagnostic(err.diagnostics));
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
    return {
      outcome: 'fail',
      detail: `eval error where a value was expected (code=${String(result.code)})`,
    };
  }
  return { outcome: 'fail', detail: `compare: ${reason}` };
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

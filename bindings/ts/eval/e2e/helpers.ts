// Shared harness for the e2e behavior-port suites.
//
// Each suite under `eval/e2e/` compiles a real CEL expression through
// `@cel-wasm/compiler` (the native `cel` CLI behind the C ABI), plans it
// through `@cel-wasm/eval`, evaluates an activation, and asserts the
// decoded `CelValue`.  This is the broad e2e mirror of the fixture-driven
// `eval/src/instance.test.ts`, ported from the C++ `e2e/*_test.cc`
// behavior suites.
//
// Compiling shells out to the CLI, so we keep one shared `Engine` and
// compile each distinct expression at most once (`evalAll` evaluates many
// activations against a single compiled Program).  Spec citations live in
// the individual suites; this module is pure plumbing.

import { compile } from '@cel-wasm/compiler';
import type { VariableDecl } from '@cel-wasm/compiler';

import { Engine } from '@cel-wasm/eval';
import type { CelError, CelInput, CelValue, Instance } from '@cel-wasm/eval';

let sharedEngine: Engine | undefined;

async function engine(): Promise<Engine> {
  sharedEngine ??= await Engine.create();
  return sharedEngine;
}

/** Compile + plan `source` (with optional declared `vars`) into a reusable {@link Instance}. */
export async function plan(
  source: string,
  vars?: readonly VariableDecl[],
): Promise<Instance> {
  const program = await compile(source, vars);
  return (await engine()).plan(program);
}

/** Compile, plan, and evaluate `source` against `activation` in one shot. */
export async function evalCel(
  source: string,
  activation: Record<string, CelInput> = {},
  vars?: readonly VariableDecl[],
): Promise<CelValue> {
  const instance = await plan(source, vars);
  return instance.eval(activation);
}

/** True iff `value` is a {@link CelError} value (a CEL spec error on the wire). */
export function isCelError(value: CelValue): value is CelError {
  return (
    typeof value === 'object' &&
    value !== null &&
    'kind' in value &&
    (value as { kind: unknown }).kind === 'error'
  );
}

/** The {@link CelError} code, or `undefined` if `value` is not an error. */
export function errorCode(value: CelValue): number | undefined {
  return isCelError(value) ? value.code : undefined;
}

// The client-side run path — the proof of the demo's thesis.
//
// This module imports `@cel-wasm/eval` (pure TS) and drives the full
// evaluation in the browser tab: `Engine.create()` → `engine.plan(program)`
// → `instance.eval(activation)`.  There is NO network hop here; the same
// `.wasm` artifact a server would run is instantiated and evaluated in the
// page.  That is the whole point — "compile once, run anywhere".

import {
  Engine,
  type Activation,
  type CelValue,
  type Program,
} from '@cel-wasm/eval';

/**
 * Evaluate a compiled {@link Program} against `activation`, fully
 * client-side.  Returns the decoded {@link CelValue} — a CEL spec error
 * (e.g. divide-by-zero) decodes to a `CelError` *value*, not a thrown
 * exception, per the wire-format law (§A.4.5); a wasm trap or a marshal
 * failure throws.
 */
export async function runProgram(
  program: Program,
  activation: Activation,
): Promise<CelValue> {
  const engine = await Engine.create();
  const instance = await engine.plan(program);
  return instance.eval(activation);
}

// The client-side run path — the proof of the demo's thesis.
//
// This module imports `@cel-wasm/eval` (pure TS) and drives the full
// evaluation in the browser tab: `Engine.create()` → `engine.plan(program)`
// → `instance.eval(activation)`.  There is NO network hop here; the same
// `.wasm` artifact a server would run is instantiated and evaluated in the
// page.  That is the whole point — "compile once, run anywhere".
//
// The demo compiles in DYNAMIC link mode (see `compile-client.ts`), so a
// Program is a ~6 KB expr module that imports the runtime from `cel.*`.
// The Engine links it against the shared `cel_runtime.wasm`.  In the
// browser there is no `node:fs`, so the runtime bytes are fetched once
// (from the same `public/` asset the compiler is served from) and handed
// to `Engine.create({ runtime })`; the compiled runtime module is cached
// in the Engine and reused across every eval.  In Node (the test path)
// `Engine.create()` auto-loads the shipped runtime via `node:fs`, so the
// fetch is skipped.

import {
  Engine,
  type Activation,
  type CelValue,
  type Program,
} from '@cel-wasm/eval';

// Served from `public/` alongside `compiler.wasm`; `BASE_URL` is the Pages
// subpath in a production build and `/` under `npm run dev`.
const RUNTIME_WASM_URL = `${import.meta.env.BASE_URL}cel_runtime.wasm`;

// One Engine for the session: it compiles `cel_runtime.wasm` once (lazily,
// on the first dynamic Program) and reuses it for every eval.
let enginePromise: Promise<Engine> | undefined;

/**
 * Whether this code is running in a browser (vs Node, where the Engine
 * auto-loads the shipped runtime via `node:fs`).  In a browser `document`
 * exists and we must fetch the runtime bytes ourselves.
 */
function inBrowser(): boolean {
  return typeof document !== 'undefined';
}

async function createEngine(): Promise<Engine> {
  if (!inBrowser()) {
    // Node / test: the Engine reads the shipped runtime from disk.
    return Engine.create();
  }
  const response = await fetch(RUNTIME_WASM_URL);
  if (!response.ok) {
    throw new Error(
      `failed to fetch the CEL runtime wasm (HTTP ${String(response.status)})`,
    );
  }
  const runtime = await response.arrayBuffer();
  return Engine.create({ runtime });
}

function getEngine(): Promise<Engine> {
  enginePromise ??= createEngine();
  return enginePromise;
}

/**
 * Evaluate a compiled {@link Program} against `activation`, fully
 * client-side.  Returns the decoded {@link CelValue} — a CEL spec error
 * (e.g. divide-by-zero) decodes to a `CelError` *value*, not a thrown
 * exception, per the wire-format law (§A.4.5); a wasm trap or a marshal
 * failure throws.  Handles both STATIC and DYNAMIC Programs (the Engine
 * routes by the module's imports).
 */
export async function runProgram(
  program: Program,
  activation: Activation,
): Promise<CelValue> {
  const engine = await getEngine();
  const instance = await engine.plan(program);
  return instance.eval(activation);
}

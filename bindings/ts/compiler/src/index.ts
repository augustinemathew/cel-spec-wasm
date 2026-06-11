// Public surface of the cel-wasm TypeScript compiler binding.
//
// Compiles CEL source to a portable `Program` (§A.5).  The v1 backend
// shells out to the already-built native `cel` CLI (see
// `internal/cli-backend.ts`); the backend sits behind the
// {@link CompileBackend} interface so an N-API or emscripten backend can
// slot in later without changing this API.

import { decodeAbi, type Program } from '@cel-wasm/eval';

import { CelCompileError, type Diagnostic } from './errors.js';
import { CliBackend, type CompileBackend } from './internal/cli-backend.js';

export type { Program } from '@cel-wasm/eval';
export { CelCompileError } from './errors.js';
export type { Diagnostic } from './errors.js';

/**
 * A declared variable's CEL type, written in the spec's surface syntax
 * (`'int'`, `'string'`, `'list(int)'`, a message FQN, …).  The compiler
 * parses this; the binding passes it through verbatim.
 */
export type CelType = string;

/** A free-variable declaration for {@link compile} (§A.5). */
export interface VariableDecl {
  readonly name: string;
  readonly type: CelType;
}

/** Options for {@link compile} (§A.5). */
export interface CompileOptions {
  readonly container?: string;
  readonly optimizeLevel?: 0 | 1 | 2 | 3;
}

// The default backend is constructed lazily on first compile so importing
// this module never requires the CLI to be present (e.g. for type-only
// consumers); the absence is surfaced at compile time instead.
let defaultBackend: CompileBackend | undefined;

function getDefaultBackend(): CompileBackend {
  defaultBackend ??= new CliBackend();
  return defaultBackend;
}

/**
 * Compile CEL `source` (with optional declared `vars`) to a portable
 * {@link Program}: the wasm artifact plus its decoded `cel.abi`
 * descriptor.
 *
 * @throws {CelCompileError} on a parse / type-check failure, carrying the
 *   parsed {@link Diagnostic}s (line/column/message where the compiler
 *   reported a location).
 */
export async function compile(
  source: string,
  vars?: readonly VariableDecl[],
  opts?: CompileOptions,
): Promise<Program> {
  const backend = getDefaultBackend();
  const wasm = await backend.compile({
    source,
    vars: vars ?? [],
    ...(opts?.container !== undefined ? { container: opts.container } : {}),
    ...(opts?.optimizeLevel !== undefined
      ? { optimizeLevel: opts.optimizeLevel }
      : {}),
  });

  let abi;
  try {
    abi = decodeAbi(wasm);
  } catch (err) {
    const detail = err instanceof Error ? err.message : String(err);
    const diagnostics: Diagnostic[] = [
      { message: `compiled wasm lacks a readable cel.abi section: ${detail}` },
    ];
    throw new CelCompileError(diagnostics);
  }

  return { wasm, abi };
}

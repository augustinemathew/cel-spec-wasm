// Public surface of the cel-wasm TypeScript compiler binding.
//
// Compiles CEL source to a portable `Program` (§A.5) through the C ABI
// over the C++ compiler.  The `compile()` implementation and its two
// backends (N-API addon, emscripten compiler.wasm) land in WI-2.x; this
// stub fixes the public types so downstream WIs can import them.

import type { Program } from '@cel-wasm/eval';

export type { Program } from '@cel-wasm/eval';

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

/**
 * Compile CEL `source` (with optional declared `vars`) to a portable
 * {@link Program}.  Rejects with a `CelCompileError` (carrying parsed
 * diagnostics) on a parse / type-check failure.  Implemented in WI-2.3.
 */
export function compile(
  _source: string,
  _vars?: readonly VariableDecl[],
  _opts?: CompileOptions,
): Promise<Program> {
  return Promise.reject(
    new Error('compile() is a stub until WI-2.3 (compiler binding)'),
  );
}

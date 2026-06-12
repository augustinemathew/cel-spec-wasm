// Public surface of the cel-wasm TypeScript compiler binding.
//
// Compiles CEL source to a portable `Program` (§A.5).  The default backend
// runs `compiler.wasm` — the CEL compiler (cel-cpp + Binaryen)
// cross-compiled to wasm32-wasi — entirely in-process, so compilation needs
// no native `cel` CLI and the same code path serves Node and the browser.
// The backend sits behind the {@link CompileBackend} interface so an N-API
// or emscripten backend can slot in later without changing this API.

import { decodeAbi, type Program } from '@cel-wasm/eval';

import { CelCompileError, type Diagnostic } from './errors.js';
import type { CompileBackend, LinkMode } from './internal/backend.js';
import { getDefaultWasmBackend } from './internal/wasm-backend.js';

export type { Program } from '@cel-wasm/eval';
export { CelCompileError } from './errors.js';
export type { Diagnostic } from './errors.js';
// The wasm compile backend is also exposed on the
// `@cel-wasm/compiler/wasm-backend` subpath so a browser consumer can
// construct it directly from fetched bytes (no `node:fs`), and a Node
// consumer of `compile()` here gets it loaded from the shipped asset.
export type { CompileRequest, LinkMode } from './internal/backend.js';

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
  /**
   * Host-function declarations (`@host` signatures), each a `.celfn`
   * source string the compiler parses into a function declaration so an
   * expression calling the host function type-checks.
   */
  readonly fns?: readonly string[];
  /**
   * How the Program links against the CEL runtime: `'static'` (default)
   * bakes the ~1.3 MB runtime in; `'dynamic'` emits a ~6 KB expression
   * module the evaluator links against a shared `cel_runtime.wasm`.
   */
  readonly linkMode?: LinkMode;
  /**
   * A serialized `FileDescriptorSet` (the bytes `protoc --descriptor_set_out`
   * emits) supplying the message types a proto-typed expression references,
   * supplied **in memory**.  Without it, a `Foo{...}` literal or a
   * message-typed field read fails to type-check.  The bytes are marshalled
   * through the compiler wasm directly (no filesystem), so the same form
   * works in Node and the browser.
   */
  readonly descriptorSetBytes?: Uint8Array;
}

// The default backend (the in-process wasm compiler) is loaded lazily on
// first compile so importing this module never reads the ~56 MB
// `compiler.wasm` for a type-only consumer; the load is cached for the
// process.
function getDefaultBackend(): Promise<CompileBackend> {
  return getDefaultWasmBackend();
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
  const backend = await getDefaultBackend();
  const wasm = await backend.compile({
    source,
    vars: vars ?? [],
    ...(opts?.fns !== undefined ? { fns: opts.fns } : {}),
    ...(opts?.container !== undefined ? { container: opts.container } : {}),
    ...(opts?.optimizeLevel !== undefined
      ? { optimizeLevel: opts.optimizeLevel }
      : {}),
    ...(opts?.linkMode !== undefined ? { linkMode: opts.linkMode } : {}),
    ...(opts?.descriptorSetBytes !== undefined
      ? { descriptorSetBytes: opts.descriptorSetBytes }
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

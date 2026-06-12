// The backend-neutral compile contract: the {@link CompileBackend}
// interface every concrete backend implements, plus the {@link CompileRequest}
// it consumes and the {@link LinkMode} that request carries.
//
// These types are deliberately backend-agnostic — they describe *what* a
// compile asks for, not *how* it runs.  The wasm backend
// (`wasm-backend.ts`) is the only implementation today; an N-API or
// emscripten backend can implement the same contract later without
// touching this module or the public `compile()` API.

/**
 * How the compiled Program links against the CEL runtime.
 *
 *  - `static` — a self-contained ~1.3 MB module with the runtime baked
 *    in (imports only host trampolines + WASI). Runs anywhere with no
 *    companion module.
 *  - `dynamic` — a ~6 KB thin expression module that imports the runtime
 *    surface (`cel.*`); the evaluator instantiates it against a shared
 *    `cel_runtime.wasm`. Smaller artifacts, one runtime shared across
 *    many Programs.
 */
export type LinkMode = 'static' | 'dynamic';

/** A request to compile one expression. */
export interface CompileRequest {
  readonly source: string;
  readonly vars: readonly { readonly name: string; readonly type: string }[];
  /**
   * Host-function declarations (`@host` signatures), each a `.celfn`
   * source string the compiler parses into a function declaration.
   */
  readonly fns?: readonly string[];
  readonly container?: string;
  readonly optimizeLevel?: 0 | 1 | 2 | 3;
  /** Static (self-contained) vs dynamic (runtime-linked) Program. */
  readonly linkMode?: LinkMode;
  /**
   * A serialized `FileDescriptorSet` supplied **in memory** (the same bytes
   * `protoc --descriptor_set_out` emits).  The wasm backend marshals the
   * bytes through linear memory (a `'d'` record) so a proto expression's
   * message types type-check with no filesystem; it builds a descriptor pool
   * (layered over the generated pool) from them.
   */
  readonly descriptorSetBytes?: Uint8Array;
}

/**
 * A swappable compile engine.  The wasm backend (`wasm-backend.ts`) is the
 * implementation today; an N-API or emscripten backend can implement the
 * same contract later.
 */
export interface CompileBackend {
  /** Compile `request` to portable wasm bytes, or throw on failure. */
  compile(request: CompileRequest): Promise<Uint8Array>;
}

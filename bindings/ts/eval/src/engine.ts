// Engine — the descriptor + host-function registry a Program is planned
// against.
//
// `Engine.create` loads the proto descriptors (a serialized
// `FileDescriptorSet` or a protobufjs `Root`) message-typed variables /
// literals resolve against; `defineFunction` registers a JS host function
// exposed to a Program as a `cel_fn.*` import; `plan` instantiates a
// static Program with the full host import object assembled and returns a
// ready-to-eval {@link Instance}.
//
// The Engine is the reusable, Program-independent half: one Engine plans
// many Programs.  Each `plan` builds a fresh per-Instance externref table,
// codec, and host-trampoline context, so two Instances of the same Engine
// never share mutable eval state.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.5.

import type * as protobuf from 'protobufjs';

import { hostFnDecl } from './celfn-decl.js';
import { instantiateProgram } from './instance.js';
import type { HostFnRegistration, Instance } from './instance.js';
import { DescriptorSet } from './proto/descriptors.js';
import { loadRuntimeModule } from './runtime-loader.js';
import type { CelValue, HostFunction, Program } from './types.js';

/** Options for {@link Engine.create}. */
export interface EngineOptions {
  /**
   * Proto descriptors message-typed variables / literals resolve against:
   * a serialized `FileDescriptorSet` (`protoc --descriptor_set_out`) or an
   * already-loaded protobufjs `Root`.  Required only to evaluate
   * message-typed Programs; scalar / aggregate Programs need none.
   */
  readonly descriptors?: Uint8Array | protobuf.Root;

  /**
   * The `cel_runtime.wasm` bytes (or a pre-compiled module) a DYNAMIC
   * Program links against.  A dynamic Program is a thin (~6 KB) expr
   * module that imports the runtime helpers from the `cel` namespace
   * instead of bundling them; the Engine instantiates this standalone
   * runtime and exposes its exports as the expr module's `cel.*` imports
   * (mirroring the C++ engine's `InstantiateRuntime`).
   *
   * Omit to use the runtime shipped at
   * `bindings/ts/eval/runtime/cel_runtime.wasm` (loaded via `node:fs` in
   * Node).  In the browser — where there is no `node:fs` — pass the bytes
   * or a module the caller fetched.  The runtime is compiled once per
   * Engine; a STATIC Program needs none, so the load is lazy.
   */
  readonly runtime?: BufferSource | WebAssembly.Module;
}

/**
 * A registered host function — the overload id it is exposed under as a
 * `cel_fn.*` import, plus its JS implementation.  The overload id is
 * synthesized from the `.celfn` declaration (`celfn-decl.ts`), matching
 * the import name the compiler stamps on the Program
 * (`SynthesiseOverloadId`, compiler/celfn/function_library.cc:180).
 */
interface RegisteredFunction {
  readonly overloadId: string;
  readonly impl: HostFunction;
  /** Declared `uint` return — the trampoline re-stamps CEL_UINT (see
   * `HostFnRegistration`, `instance.ts`). */
  readonly returnsUint: boolean;
  /** Declared `proto(<fqn>)` return — the trampoline interns the impl's
   * return as a CEL_MESSAGE (see `HostFnRegistration`, `instance.ts`);
   * `undefined` for every other return type. */
  readonly returnMessageFqn: string | undefined;
}

/**
 * The descriptor + host-function registry a {@link Program} is planned
 * against.  Construct via {@link Engine.create}; plan one or many
 * Programs.
 */
export class Engine {
  private readonly descriptors: DescriptorSet | undefined;
  private readonly functions = new Map<string, RegisteredFunction>();

  // The override the caller passed for the dynamic-link runtime (bytes or
  // a pre-compiled module), or undefined to load the shipped runtime.
  private readonly runtimeOverride:
    | BufferSource
    | WebAssembly.Module
    | undefined;
  // The compiled `cel_runtime.wasm`, lazily compiled the first time a
  // DYNAMIC Program is planned and reused across every dynamic Instance.
  // A static-only Engine never compiles it.
  private runtimeModule: WebAssembly.Module | undefined;

  private constructor(
    descriptors: DescriptorSet | undefined,
    runtimeOverride: BufferSource | WebAssembly.Module | undefined,
  ) {
    this.descriptors = descriptors;
    this.runtimeOverride = runtimeOverride;
  }

  /**
   * Build an Engine, loading any supplied descriptors.  Async to match
   * the §A.5 API (descriptor loading may become async for a remote
   * descriptor source); today the load is synchronous under the hood.
   */
  static create(opts: EngineOptions = {}): Promise<Engine> {
    const descriptors = loadDescriptors(opts.descriptors);
    return Promise.resolve(new Engine(descriptors, opts.runtime));
  }

  /**
   * Register a host function, exposed to a planned Program as a
   * `cel_fn.<overload_id>` import.  `decl` is the same `.celfn` `@host`
   * declaration string passed to the compiler (e.g.
   * `'int @host.addOne(int x);'` — `CompileOptions.fns`), from which the
   * overload id is synthesized exactly as the compiler does; a bare
   * identifier is taken verbatim as an already-synthesized overload id
   * (the C++ `Engine::AddFunction(overload_id, …)` form).  The
   * implementation receives already-decoded {@link CelValue} arguments
   * and returns a `HostFnResult` — a {@link CelValue}, or (for a
   * `proto(<fqn>)`-declared return) a protobufjs `Message` / plain field
   * object interned as a CEL_MESSAGE; a CEL spec error it wants to
   * surface is a {@link CelError} value, never a thrown exception
   * (§A.4.5).
   * Throws {@link CelFnDeclError} on a malformed `decl`.
   */
  defineFunction(decl: string, impl: HostFunction): void {
    const { overloadId, returnsUint, returnMessageFqn } = hostFnDecl(decl);
    this.functions.set(overloadId, {
      overloadId,
      impl,
      returnsUint,
      returnMessageFqn,
    });
  }

  /**
   * Instantiate `program` and return a ready-to-eval {@link Instance}.
   *
   * Routes by import introspection, mirroring the C++ engine's `Plan`:
   *
   *   - A STATIC Program (no `cel.*` imports) bundles the runtime; the
   *     host supplies only the `cel_host` trampolines, the registered
   *     `cel_fn` functions, `cel_env.cel_log`, and the WASI stubs, and the
   *     Program exports its own `memory` + `arena_alloc`.
   *   - A DYNAMIC Program (imports from the `cel` namespace) is a thin
   *     expr module; the Engine instantiates `cel_runtime.wasm`, exposes
   *     its exports as the expr module's `cel.*` imports (incl. the shared
   *     `cel.memory`), and the resulting Instance's memory is the
   *     runtime's.
   *
   * The same {@link Instance.eval} API works for both modes.  Compiles
   * `cel_runtime.wasm` once (lazily, only when the first dynamic Program
   * is planned) and reuses the module across dynamic Instances; each
   * Instance still gets its own fresh runtime *instance* (per-Instance
   * isolation, matching the C++ per-Plan store).
   */
  async plan(program: Program): Promise<Instance> {
    const fns = new Map<string, HostFnRegistration>();
    for (const [overloadId, fn] of this.functions) {
      fns.set(overloadId, {
        impl: fn.impl,
        returnsUint: fn.returnsUint,
        returnMessageFqn: fn.returnMessageFqn,
      });
    }
    return instantiateProgram(program, this.descriptors, fns, () =>
      this.runtimeModuleFor(),
    );
  }

  /**
   * Resolve the compiled `cel_runtime.wasm` for a dynamic Program,
   * compiling it once on first use.  Called lazily from the dynamic
   * branch of {@link instantiateProgram}; a static-only Engine never
   * reaches it, so the runtime is never loaded for a static workload.
   */
  private async runtimeModuleFor(): Promise<WebAssembly.Module> {
    if (this.runtimeModule === undefined) {
      this.runtimeModule = await loadRuntimeModule(this.runtimeOverride);
    }
    return this.runtimeModule;
  }
}

/** A JS host function's CelValue contract, re-exported for callers. */
export type { HostFunction, CelValue };

function loadDescriptors(
  descriptors: Uint8Array | protobuf.Root | undefined,
): DescriptorSet | undefined {
  if (descriptors === undefined) {
    return undefined;
  }
  if (descriptors instanceof Uint8Array) {
    return DescriptorSet.fromFileDescriptorSet(descriptors);
  }
  return DescriptorSet.fromRoot(descriptors);
}

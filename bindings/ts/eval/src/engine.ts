/**
 * `Engine` — the runtime side, the TS counterpart to
 * `compiler_v2/api/engine.cc` (`cel::Engine`). Compiles `cel_runtime.wasm`
 * once (`create`), then `plan`s a `Program` into an `Instance` by
 * reproducing the §4.4 instantiation sequence the probes validated:
 * instantiate the runtime (it owns + exports the shared memory),
 * `arena_init`, bind the runtime's `cel_*` exports as the expr module's
 * `cel.*` imports, instantiate the expr module against the real
 * `cel_host.*` proto-reflection trampolines, and hand the resulting
 * handles to an `Instance`.
 *
 * Unlike wasmtime there is **no `Linker`** — JS builds the import object by
 * hand. The runtime *also* imports `cel_host.*`, but those fire only during
 * `$eval` (after `arena_init`); a const box holder lets us bind a
 * deferred proxy at the runtime's instantiation and swap in the real
 * trampolines once the runtime's memory + arena are available.
 *
 * This module is the thin WebAssembly-instantiation shell. Its real test
 * is the real-wasm e2e + the `conformance_ts` integration gate (they
 * instantiate the actual runtime + corpus Programs); it is excluded from
 * the hermetic unit-coverage gate (vitest.config.ts).
 */
import { ExternrefTable } from './externref.js';
import { Instance, type RuntimeHandles } from './instance.js';
import type { Program } from './program.js';
import type { FieldEntry, TypeEntry } from './abi.js';
import {
  makeCelHostImports,
  type TrampolineContext,
} from './host/trampolines.js';
import type {
  ListBacking,
  MapBacking,
  MessageBacking,
} from './host/backing.js';
import type { TypeRegistry } from './type-registry.js';
import { createWasiPreview1 } from './wasi-shim.js';

/** Bump-arena capacity seeded per Instance (cel_layout.h
 *  `CELWASM_ARENA_CAPACITY_BYTES`). */
const ARENA_CAPACITY_BYTES = 64 * 1024;

/** Optional `Engine.create` config (the canonical object form). */
export interface EngineOptions {
  /** protobuf-es descriptors for binding / returning / constructing real
   *  proto messages. Optional: scalar / JS-object / list / map evals need
   *  no registry. */
  readonly registry?: TypeRegistry;
}

/** Thrown on a wasm compile / instantiate failure, or a missing /
 *  wrong-typed runtime export. */
export class EngineError extends Error {
  public override readonly name = 'EngineError';
}

function exportFunction(
  exports: WebAssembly.Exports,
  name: string,
): (...args: number[]) => number {
  const f = exports[name];
  if (typeof f !== 'function') {
    throw new EngineError(
      `runtime export '${name}' is missing or not a function`,
    );
  }
  return f as (...args: number[]) => number;
}

function exportMemory(
  exports: WebAssembly.Exports,
  name: string,
): WebAssembly.Memory {
  const m = exports[name];
  if (!(m instanceof WebAssembly.Memory)) {
    throw new EngineError(`runtime export '${name}' is not a Memory`);
  }
  return m;
}

export class Engine {
  private constructor(
    private readonly runtimeModule: WebAssembly.Module,
    private readonly options: EngineOptions,
  ) {}

  /**
   * Compile `cel_runtime.wasm` once. Reuse the Engine across many `plan`
   * calls (the parse is the expensive part). Pass a {@link TypeRegistry}
   * only if you bind / return real protobuf-es messages.
   *
   * @example
   * ```ts
   * import { readFileSync } from 'node:fs';
   * import { Engine } from '@celwasm/eval';
   *
   * const engine = await Engine.create(readFileSync('cel_runtime.wasm'));
   * // with proto messages:
   * // const engine = await Engine.create(bytes, { registry });
   * ```
   */
  public static async create(
    runtimeWasm: BufferSource,
    options: EngineOptions = {},
  ): Promise<Engine> {
    try {
      return new Engine(await WebAssembly.compile(runtimeWasm), options);
    } catch (cause) {
      throw new EngineError(
        `failed to compile cel_runtime.wasm: ${String(cause)}`,
      );
    }
  }

  /** The configured type registry, if any (for `registry.toMessage(...)`
   *  on a returned `CEL_MESSAGE`). */
  public get registry(): TypeRegistry | undefined {
    return this.options.registry;
  }

  /** Instantiate `program` against a fresh runtime+memory and return a
   *  ready `Instance`. */
  public async plan(program: Program): Promise<Instance> {
    const refs = new ExternrefTable<MessageBacking, MapBacking, ListBacking>();
    const fields = new Map<number, FieldEntry>(
      (program.abi?.fields ?? []).map((f) => [f.id, f]),
    );
    const types = new Map<number, TypeEntry>(
      (program.abi?.types ?? []).map((t) => [t.id, t]),
    );

    // Late-bound holders. The runtime's exported memory + arena aren't
    // available until after it instantiates, and the runtime's own
    // `cel_host.*` imports fire only during `$eval` (post arena_init) — so
    // reading them through these const boxes at call time is safe.
    const box: {
      memory: WebAssembly.Memory | undefined;
      host: WebAssembly.ModuleImports | undefined;
    } = { memory: undefined, host: undefined };
    const memoryBytes = (): Uint8Array => {
      if (box.memory === undefined) {
        throw new EngineError('runtime memory accessed before instantiation');
      }
      return new Uint8Array(box.memory.buffer);
    };
    const deferHost = new Proxy(
      {},
      {
        get: (_t, prop): ((...a: number[]) => void) => {
          return (...a: number[]): void => {
            if (box.host === undefined) {
              throw new EngineError('cel_host called before plan completed');
            }
            const fn = box.host[prop as string];
            (fn as (...x: number[]) => void)(...a);
          };
        },
        has: (): boolean => true,
      },
    );

    const runtime = await this.instantiate(
      this.runtimeModule,
      {
        cel_env: { cel_log: (): void => undefined },
        cel_host: deferHost,
        wasi_snapshot_preview1: { ...createWasiPreview1(memoryBytes) },
      },
      'cel_runtime.wasm',
    );
    const rx = runtime.exports;
    const memory = exportMemory(rx, 'memory');
    box.memory = memory;
    exportFunction(rx, 'arena_init')(ARENA_CAPACITY_BYTES);

    const ctx: TrampolineContext = {
      memory,
      refs,
      fields,
      types,
      registry: this.options.registry,
      arenaAlloc: exportFunction(rx, 'arena_alloc'),
    };
    const host = makeCelHostImports(ctx);
    box.host = host;

    // Bind the runtime's exports as the expr module's `cel.*` imports
    // (no lazy tracking — link the whole runtime; repo rule).
    const cel: Record<string, WebAssembly.ImportValue> = { memory };
    for (const [name, value] of Object.entries(rx)) {
      if (typeof value === 'function') {
        cel[name] = value;
      }
    }

    let exprModule: WebAssembly.Module;
    try {
      // `wasmBytes` is `Uint8Array<ArrayBufferLike>`; `compile` wants a
      // (non-shared) `BufferSource`. Program bytes come from fs/fetch
      // (ArrayBuffer-backed), so the assertion is sound.
      exprModule = await WebAssembly.compile(program.wasmBytes as BufferSource);
    } catch (cause) {
      throw new EngineError(`failed to compile program: ${String(cause)}`);
    }
    const expr = await this.instantiate(
      exprModule,
      { cel, cel_host: host },
      'program',
    );

    const handles: RuntimeHandles = {
      ctx,
      malloc: exportFunction(rx, 'malloc'),
      evalFn: exportFunction(expr.exports, 'eval'),
      abi: program.abi,
    };
    return new Instance(handles);
  }

  private async instantiate(
    module: WebAssembly.Module,
    imports: WebAssembly.Imports,
    label: string,
  ): Promise<WebAssembly.Instance> {
    try {
      return await WebAssembly.instantiate(module, imports);
    } catch (cause) {
      throw new EngineError(`failed to instantiate ${label}: ${String(cause)}`);
    }
  }
}

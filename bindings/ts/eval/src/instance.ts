// Instance — one instantiated Program, ready to evaluate.
//
// `instantiateProgram` (called by `Engine.plan`) builds the full host
// import object, instantiates the static Program, and assembles the
// per-Instance eval context: a memory accessor that re-reads on growth,
// the per-Eval externref table, the resolving codec, the decoded
// `cel.abi`, the descriptors, and the arena allocator (the Program's
// `arena_alloc` export).  `Instance.eval` then runs the marshal → `$eval`
// → decode → reset sequence the C++ `Instance::Eval(Activation)` defines
// (`eval/instance.cc`).
//
// The eval sequence, mirrored from C++:
//   1. Reset the externref table (slot indices must not leak across Evals).
//   2. Reset the activation arena cursor.
//   3. Marshal each declared variable into its slot — string / bytes
//      payloads into the activation buffer ABOVE the runtime's bump arena,
//      because `$eval`'s prelude calls `arena_reset` and would otherwise
//      wipe a payload placed in the arena (`instance_impl.h:82`).
//   4. Call `$eval` (nullary `() -> i32`) → the result slot offset.
//   5. Decode the result CelValue at that slot, resolving externref kinds.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.4.

import { Root } from 'protobufjs';

import { decodeAbi } from './abi.js';
import { normalizeActivation } from './activation.js';
import type { Activation } from './activation.js';
import { synthesizeErrorMessage } from './celvalue.js';
import { ExternrefTable } from './externref.js';
import { makeAggregateTrampolines } from './host/aggregates.js';
import type { AggregateContext } from './host/aggregates.js';
import { makeProtoTrampolines } from './host/proto.js';
import type { ProtoCodec, ProtoContext } from './host/proto.js';
import {
  CEL_ENV_MODULE,
  WASI_MODULE,
  makeStubImports,
  makeTimestampTzAccessor,
} from './host/stubs.js';
import type { TzAccessorCodec } from './host/stubs.js';
import {
  CelMarshalError,
  marshalActivation,
  totalActivationBufferBytes,
} from './marshal.js';
import type { ActivationArena, MarshalEnv } from './marshal.js';
import { DescriptorSet } from './proto/descriptors.js';
import { encodeCelValue, resolveCelValue } from './resolving-codec.js';
import type { CodecEnv } from './resolving-codec.js';
import {
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CEL_VALUE_SIZE,
  CelKind,
  LinkMode,
} from './types.js';
import type {
  CelAbi,
  CelInput,
  CelValue,
  HostFunction,
  Program,
} from './types.js';

/** The `cel_fn.*` import module name registered host functions surface under. */
const CEL_FN_MODULE = 'cel_fn';

/**
 * The `cel.*` import module name a DYNAMIC Program imports the runtime
 * helpers (incl. the shared `cel.memory`) from.  A STATIC Program imports
 * nothing from this namespace — its presence is the routing signal.
 */
const CEL_RUNTIME_MODULE = 'cel';

/**
 * Thrown when `$eval` traps or the Program's exports are malformed — a
 * host / Program failure, not a CEL spec error (those are CEL_ERROR
 * values, returned).  Carries a `.code` distinguishing the failure mode.
 */
export class CelEvalError extends Error {
  override readonly name = 'CelEvalError';
  readonly code: 'TRAP' | 'BAD_EXPORTS' | 'MARSHAL' | 'LINK_MODE';

  constructor(
    code: 'TRAP' | 'BAD_EXPORTS' | 'MARSHAL' | 'LINK_MODE',
    message: string,
  ) {
    super(message);
    this.code = code;
  }
}

/** The runtime exports the eval sequence drives. */
interface ProgramExports {
  readonly memory: WebAssembly.Memory;
  readonly eval: () => number;
  readonly arenaInit: (capBytes: number) => void;
  readonly arenaAlloc: (n: number) => number;
  readonly malloc: (n: number) => number;
  readonly callCtors: (() => void) | undefined;
}

/**
 * The arena capacity seeded once per Instance (`CELWASM_ARENA_CAPACITY_BYTES`,
 * `runtime/cel_layout.h:41`).  The C++ engine calls `arena_init` with this
 * exact value at Plan time; the binding mirrors it so the runtime's bump
 * arena is live before the first `$eval` allocates.
 */
const ARENA_CAPACITY_BYTES = 64 * 1024;

/**
 * One instantiated Program.  Build via {@link Engine.plan}; call
 * {@link Instance.eval} once per activation.  Single-threaded: a second
 * `eval` re-uses the same memory + externref table after the first
 * returns (the externref reset is the between-Eval boundary).
 */
export class Instance {
  private readonly exports: ProgramExports;
  private readonly abi: CelAbi;
  private readonly refs: ExternrefTable;
  private readonly descriptors: DescriptorSet | undefined;

  // The activation buffer (above __heap_base) string / bytes variable
  // payloads land in, plus its cursor (rewound per Eval).
  private activationBufOffset = 0;
  private activationBufCapacity = 0;
  private activationCursor = 0;

  constructor(
    exports: ProgramExports,
    abi: CelAbi,
    refs: ExternrefTable,
    descriptors: DescriptorSet | undefined,
  ) {
    this.exports = exports;
    this.abi = abi;
    this.refs = refs;
    this.descriptors = descriptors;
  }

  /** The decoded `cel.abi` descriptor of the planned Program. */
  get programAbi(): CelAbi {
    return this.abi;
  }

  /**
   * Evaluate the Program against `activation`.  Marshals each declared
   * variable, calls `$eval`, and returns the decoded result CelValue
   * (a CEL spec error decodes to a {@link CelError} value).  Throws
   * {@link CelMarshalError} on a bad activation and {@link CelEvalError}
   * on a wasm trap.
   */
  eval(activation?: Activation): CelValue {
    const act = normalizeActivation(activation);
    this.refs.reset();
    this.activationCursor = 0;
    this.marshal(act);
    const slot = this.runEval();
    return resolveCelValue(this.codecEnv(), slot);
  }

  // ── internals ─────────────────────────────────────────────────────

  private view(): DataView {
    return new DataView(this.exports.memory.buffer);
  }

  private bytes(): Uint8Array {
    return new Uint8Array(this.exports.memory.buffer);
  }

  private codecEnv(): CodecEnv {
    return {
      view: () => this.view(),
      bytes: () => this.bytes(),
      refs: this.refs,
      arenaAlloc: (n: number) => this.exports.arenaAlloc(n),
    };
  }

  private activationArena(): ActivationArena {
    return {
      alloc: (n: number) => this.allocActivation(n),
    };
  }

  /**
   * Reserve `n` bytes (8-aligned) in the activation buffer, growing it via
   * `malloc` when the current capacity is exhausted.  Returns the
   * linear-memory offset, or 0 on allocation failure.  The buffer lives
   * above `__heap_base`, so its bytes survive `$eval`'s `arena_reset`.
   */
  private allocActivation(n: number): number {
    const aligned = (n + 7) & ~7;
    if (this.activationCursor + aligned > this.activationBufCapacity) {
      const need = this.activationCursor + aligned;
      const offset = this.exports.malloc(need);
      if (offset === 0) {
        return 0;
      }
      // A fresh, larger buffer; the previous one is left to dlmalloc's
      // free list (reclaimed on the next sized alloc), mirroring the C++
      // activation-buffer grow path (`eval/instance.cc`).
      this.activationBufOffset = offset;
      this.activationBufCapacity = need;
      this.activationCursor = 0;
    }
    const ptr = this.activationBufOffset + this.activationCursor;
    this.activationCursor += aligned;
    return ptr;
  }

  private marshal(activation: Record<string, CelInput>): void {
    // Pre-size the activation buffer so a single malloc covers every
    // string / bytes payload (a mid-marshal malloc could grow memory and
    // detach the byte view, mirroring the C++ pre-pass).
    const need = totalActivationBufferBytes(this.abi.variables, activation);
    if (need > this.activationBufCapacity) {
      const offset = this.exports.malloc(need);
      if (offset === 0) {
        throw new CelMarshalError(
          `Activation: malloc(${String(need)}) for the activation buffer ` +
            `returned NULL`,
        );
      }
      this.activationBufOffset = offset;
      this.activationBufCapacity = need;
    }
    this.activationCursor = 0;
    marshalActivation(this.marshalEnv(), this.abi.variables, activation);
  }

  private marshalEnv(): MarshalEnv {
    return {
      view: () => this.view(),
      bytes: () => this.bytes(),
      refs: this.refs,
      codec: this.codecEnv(),
      activationArena: this.activationArena(),
      descriptors: this.descriptors,
      types: this.abi.types,
    };
  }

  private runEval(): number {
    let slot: number;
    try {
      slot = this.exports.eval();
    } catch (err) {
      const detail = err instanceof Error ? err.message : String(err);
      throw new CelEvalError('TRAP', `$eval trapped: ${detail}`);
    }
    if (!Number.isInteger(slot) || slot < 0) {
      throw new CelEvalError(
        'BAD_EXPORTS',
        `$eval returned a non-offset result (${String(slot)})`,
      );
    }
    return slot;
  }
}

/**
 * Instantiate `program`, assemble the host import object, and return a
 * ready-to-eval {@link Instance}.  Called by {@link Engine.plan}.
 *
 * Routes by import introspection, mirroring the C++ engine's `Plan`
 * (`eval/engine.cc`):
 *
 *   - STATIC (no `cel.*` imports): the Program bundles the runtime.  Build
 *     the four host import groups (§A.4.4) — `cel_host` trampolines,
 *     `cel_fn` functions, `cel_env`, WASI — instantiate the Program, and
 *     its own exports back the Instance's memory + arena.
 *   - DYNAMIC (`cel.*` imported): the Program is a thin expr module.
 *     Instantiate the standalone `cel_runtime.wasm` (supplying it the same
 *     `cel_host` + `cel_env` + WASI groups), expose its exports as the
 *     expr module's `cel.*` imports (incl. the shared `cel.memory`), then
 *     instantiate the expr module against `{ cel, cel_host, cel_fn }`.
 *     The Instance's memory is the runtime's; its `eval` is the expr
 *     module's.
 *
 * Both paths build a fresh per-Instance externref table + codec +
 * host-trampoline context, and `runtimeModule` is fetched lazily — only
 * the dynamic branch calls it, so a static workload never compiles the
 * runtime.
 */
export async function instantiateProgram(
  program: Program,
  descriptors: DescriptorSet | undefined,
  functions: ReadonlyMap<string, HostFunction>,
  runtimeModule: () => Promise<WebAssembly.Module>,
): Promise<Instance> {
  const refs = new ExternrefTable();
  const abi = program.abi;

  // The instance handle is populated after instantiation; the host
  // imports close over getters that read this holder, so they see the
  // live exports once instantiation completes.  A mutable holder (not a
  // reassigned `let`) keeps the closures' view of it explicit.  In
  // dynamic mode the handle is set to the runtime's exports — the
  // trampolines + codec + marshal then operate on the runtime's shared
  // memory.
  const handle: { exports: ProgramExports | undefined } = {
    exports: undefined,
  };
  const requireExports = (): ProgramExports => {
    if (handle.exports === undefined) {
      throw new CelEvalError(
        'BAD_EXPORTS',
        'host import ran before instantiation',
      );
    }
    return handle.exports;
  };
  const memoryOf = (): WebAssembly.Memory | undefined => handle.exports?.memory;
  const view = (): DataView => new DataView(requireExports().memory.buffer);
  const bytes = (): Uint8Array =>
    new Uint8Array(requireExports().memory.buffer);
  const arenaAlloc = (n: number): number => requireExports().arenaAlloc(n);

  const codecEnv: CodecEnv = { view, bytes, refs, arenaAlloc };
  const hostImports = buildImports(
    { view, bytes, refs, arenaAlloc, memoryOf },
    codecEnv,
    abi,
    descriptors,
    functions,
  );

  // Compile the expr module once so we can introspect its imports before
  // deciding how to link it.
  const exprModule = await WebAssembly.compile(programBytes(program));
  const isDynamic = importsCelNamespace(exprModule);
  crossCheckLinkMode(abi, isDynamic);

  if (isDynamic) {
    await linkDynamic(exprModule, hostImports, handle, await runtimeModule());
  } else {
    await linkStatic(exprModule, hostImports, handle);
  }

  // Seed the runtime's bump arena once per Instance (mirrors the C++
  // engine's `arena_init(CELWASM_ARENA_CAPACITY_BYTES)` Plan step,
  // `eval/engine.cc`).  Without it, the first `$eval` allocation traps
  // `unreachable`.  In dynamic mode this seeds the runtime's arena, which
  // the expr module shares.
  requireExports().arenaInit(ARENA_CAPACITY_BYTES);

  // Re-decode the ABI from the wasm bytes so the Instance owns its own
  // copy (the caller's Program.abi is the same data; decode defensively
  // in case a caller hand-built a Program with an empty abi).
  const resolvedAbi: CelAbi =
    abi.variables.length > 0 || abi.types.length > 0 || abi.fields.length > 0
      ? abi
      : decodeAbi(program.wasm);

  return new Instance(requireExports(), resolvedAbi, refs, descriptors);
}

/** The Program's wasm as a non-shared ArrayBuffer for compilation. */
function programBytes(program: Program): ArrayBuffer {
  return program.wasm.buffer.slice(
    program.wasm.byteOffset,
    program.wasm.byteOffset + program.wasm.byteLength,
  );
}

/**
 * True iff `module` imports anything from the `cel` namespace — the
 * import-shape routing signal a DYNAMIC Program carries (it imports the
 * runtime helpers, incl. `cel.memory`, from `cel`).  A STATIC Program
 * bundles the runtime and imports nothing from `cel`.  This is the
 * authoritative router (the `cel.abi` link_mode label is only a
 * cross-check); mirrors `ModuleImportsCelNamespace` in `eval/engine.cc`.
 */
function importsCelNamespace(module: WebAssembly.Module): boolean {
  return WebAssembly.Module.imports(module).some(
    (i) => i.module === CEL_RUNTIME_MODULE,
  );
}

/**
 * Cross-check the `cel.abi` link_mode label against the import-derived
 * routing — a tripwire for mislabeled / corrupted artifacts (cache
 * validators, cross-process shipping).  Routing stays driven by the
 * import shape; only a label that contradicts that shape fails here.
 * Mirrors `ValidateLinkModeLabel` in `eval/engine.cc`.
 */
function crossCheckLinkMode(abi: CelAbi, isDynamic: boolean): void {
  if (abi.linkMode === LinkMode.STATIC && isDynamic) {
    throw new CelEvalError(
      'LINK_MODE',
      'cel.abi link_mode says STATIC but the module imports from the `cel` ' +
        'namespace (dynamic-link shape) — the Program is mislabeled or corrupted',
    );
  }
  if (abi.linkMode === LinkMode.DYNAMIC && !isDynamic) {
    throw new CelEvalError(
      'LINK_MODE',
      'cel.abi link_mode says DYNAMIC but the module has no `cel` namespace ' +
        'imports (static-link shape) — the Program is mislabeled or corrupted',
    );
  }
}

/**
 * STATIC link: instantiate the self-contained Program; its own exports
 * (memory + arena + eval) back the Instance.  Mirrors the static tail of
 * `Engine::Plan` (`BindStaticModeHelpers`).
 */
async function linkStatic(
  exprModule: WebAssembly.Module,
  hostImports: HostImports,
  handle: { exports: ProgramExports | undefined },
): Promise<void> {
  const instance = await WebAssembly.instantiate(exprModule, {
    cel_host: hostImports.celHost,
    [CEL_FN_MODULE]: hostImports.celFn,
    [CEL_ENV_MODULE]: hostImports.celEnv,
    [WASI_MODULE]: hostImports.wasi,
  });
  handle.exports = readExports(instance);
  // Defense-in-depth: a static Program exports `__wasm_call_ctors`
  // explicitly (the strip tool keeps it through DCE) so a future surface
  // that needs C++ static-ctor init is covered.  Today's surface is
  // empirically zero-init-safe; absent the export we skip.
  handle.exports.callCtors?.();
}

/**
 * DYNAMIC link: instantiate the standalone `cel_runtime.wasm`, expose its
 * exports as the expr module's `cel.*` imports, then instantiate the expr
 * module.  The runtime's exported (shared) memory + arena back the
 * Instance; the expr module contributes `eval`.  Mirrors the C++ engine's
 * `InstantiateRuntime` → `DefineCelLinkerBindings` → `InstantiateExpr`.
 */
async function linkDynamic(
  exprModule: WebAssembly.Module,
  hostImports: HostImports,
  handle: { exports: ProgramExports | undefined },
  runtimeModule: WebAssembly.Module,
): Promise<void> {
  // 1. Instantiate the runtime.  It imports the SAME cel_host trampolines
  //    + cel_env + WASI groups (its aggregate dispatchers call back into
  //    cel_host; its libc references cel_env/WASI).  The trampolines close
  //    over `handle.exports`, so set the handle to the runtime's exports
  //    immediately — before any expr eval drives a trampoline.
  const runtimeInstance = await WebAssembly.instantiate(runtimeModule, {
    cel_host: hostImports.celHost,
    [CEL_ENV_MODULE]: hostImports.celEnv,
    [WASI_MODULE]: hostImports.wasi,
  });
  const runtimeExports = readRuntimeExports(runtimeInstance);
  handle.exports = runtimeExports;

  // 2. Build the `cel` import object from the runtime's exports:
  //    `cel.memory` = the runtime's shared memory, `cel.arena_alloc` =
  //    the runtime's bump allocator, and every other `cel.<name>` =
  //    `runtime.exports.<name>` (the helper functions the expr module
  //    calls).  No lazy tracking — every `cel.*` import the expr declares
  //    is bound from the runtime export of the same name (a missing one
  //    surfaces as an instantiate LinkError naming `cel.<name>`).
  const celImports = buildCelImports(exprModule, runtimeInstance.exports);

  // 3. Instantiate the expr module against the runtime-backed `cel` group
  //    plus the host trampolines + registered functions.
  const exprInstance = await WebAssembly.instantiate(exprModule, {
    [CEL_RUNTIME_MODULE]: celImports,
    cel_host: hostImports.celHost,
    [CEL_FN_MODULE]: hostImports.celFn,
  });

  // 4. Run the expr module's ctors if present (defense-in-depth; the
  //    stripped runtime's command wrappers no longer run them per call).
  const exprCtors = exprInstance.exports.__wasm_call_ctors;
  if (typeof exprCtors === 'function') {
    (exprCtors as () => void)();
  }

  // 5. The Instance's `eval` is the expr module's; everything else
  //    (memory + arena) stays the runtime's.
  handle.exports = { ...runtimeExports, eval: readEvalExport(exprInstance) };
}

/**
 * Build the expr module's `cel.*` import object from the runtime's
 * exports: bind every name the expr module imports from `cel` to the
 * runtime export of the same name (`cel.memory` → the shared memory,
 * `cel.arena_alloc` → the allocator, the helpers → their functions).
 */
function buildCelImports(
  exprModule: WebAssembly.Module,
  runtimeExports: WebAssembly.Exports,
): WebAssembly.ModuleImports {
  const cel: Record<string, WebAssembly.ImportValue> = {};
  for (const imp of WebAssembly.Module.imports(exprModule)) {
    if (imp.module !== CEL_RUNTIME_MODULE) continue;
    const exported = runtimeExports[imp.name];
    if (exported === undefined) {
      throw new CelEvalError(
        'BAD_EXPORTS',
        `dynamic Program imports cel.${imp.name} but cel_runtime.wasm has ` +
          `no export of that name`,
      );
    }
    cel[imp.name] = exported as WebAssembly.ImportValue;
  }
  return cel;
}

// ── Import assembly ───────────────────────────────────────────────────

interface HostEnv {
  view(): DataView;
  bytes(): Uint8Array;
  readonly refs: ExternrefTable;
  arenaAlloc(n: number): number;
  memoryOf(): WebAssembly.Memory | undefined;
}

/**
 * The four host import groups, kept SEPARATE rather than pre-merged so the
 * static and dynamic linkers can each wire the subset they need: the
 * static Program takes all four on itself; in dynamic mode `cel_host` +
 * `cel_env` + WASI go to the runtime and `cel_host` + `cel_fn` go to the
 * expr module.  `cel_host` is shared by both runtime and expr (it satisfies
 * both the runtime's aggregate dispatchers and the expr's proto/field
 * trampolines).
 */
interface HostImports {
  readonly celHost: Record<string, (...args: number[]) => void>;
  readonly celFn: Record<string, (...args: number[]) => void>;
  readonly celEnv: WebAssembly.ModuleImports;
  readonly wasi: WebAssembly.ModuleImports;
}

function buildImports(
  host: HostEnv,
  codecEnv: CodecEnv,
  abi: CelAbi,
  descriptors: DescriptorSet | undefined,
  functions: ReadonlyMap<string, HostFunction>,
): HostImports {
  const aggregateCtx: AggregateContext = {
    view: () => host.view(),
    bytes: () => host.bytes(),
    refs: host.refs,
    readValue: (slot: number) => resolveCelValue(codecEnv, slot),
    writeValue: (slot: number, value: CelValue) => {
      encodeCelValue(codecEnv, slot, value);
    },
    arenaAlloc: (n: number) => host.arenaAlloc(n),
  };

  const protoCtx: ProtoContext = {
    codec: makeProtoCodec(host, codecEnv),
    refs: host.refs,
    fields: abi.fields,
    types: abi.types,
    descriptors: descriptors ?? DescriptorSet.fromRoot(emptyRoot()),
    arenaAlloc: (n: number) => host.arenaAlloc(n),
  };

  const tzCodec: TzAccessorCodec = makeTzCodec(host, codecEnv);

  // `makeAggregateTrampolines` + `makeProtoTrampolines` together cover the
  // full empirical `cel_host` import surface; the tz accessor is the one
  // trampoline whose civil-time projection lives in `host/stubs.js`.
  const celHost: Record<string, (...args: number[]) => void> = {
    ...makeAggregateTrampolines(aggregateCtx),
    ...makeProtoTrampolines(protoCtx),
    cel_timestamp_tz_accessor: makeTimestampTzAccessor(tzCodec),
  };

  const stubs = makeStubImports({ memory: () => host.memoryOf() });

  const celFn = buildCelFnImports(codecEnv, functions);

  return {
    celHost,
    celFn,
    celEnv: stubs[CEL_ENV_MODULE] ?? {},
    wasi: stubs[WASI_MODULE] ?? {},
  };
}

/**
 * Build the `cel_fn.*` imports for the registered host functions: each is
 * a `(out, ...argSlots) => void` trampoline that decodes its argument
 * slots through the resolving codec, calls the JS impl, and encodes the
 * result back into the out slot — the host-call ABI of
 * `eval/host_call_context.h:127`.  Exported so the host-fn round-trip can
 * be exercised without a compiled `@host` Program (none is reachable in
 * the TS test toolchain yet — see the e2e gap note in `instance.test.ts`).
 */
export function buildCelFnImports(
  codecEnv: CodecEnv,
  functions: ReadonlyMap<string, HostFunction>,
): Record<string, (...args: number[]) => void> {
  const imports: Record<string, (...args: number[]) => void> = {};
  for (const [name, impl] of functions) {
    imports[name] = (out: number, ...argSlots: number[]): void => {
      const args = argSlots.map((slot) => resolveCelValue(codecEnv, slot));
      const result = impl(...args);
      encodeCelValue(codecEnv, out, result);
    };
  }
  return imports;
}

/** Adapt the resolving codec to the `ProtoCodec` hook surface. */
function makeProtoCodec(host: HostEnv, codecEnv: CodecEnv): ProtoCodec {
  return {
    readValue: (slot: number) => resolveCelValue(codecEnv, slot),
    writeValue: (slot: number, value: CelValue) => {
      encodeCelValue(codecEnv, slot, value);
    },
    readKind: (slot: number) =>
      host.view().getUint32(slot + CEL_VALUE_KIND_OFFSET, true) as CelKind,
    readMessageSlot: (slot: number) =>
      host.view().getUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, true),
    copyValue: (dst: number, src: number) => {
      copySlot(host.view(), dst, src);
    },
    writeBool: (slot: number, value: boolean) => {
      const view = host.view();
      view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.BOOL, true);
      view.setInt32(slot + CEL_VALUE_PAYLOAD_OFFSET, value ? 1 : 0, true);
    },
    writeError: (slot: number, code: number) => {
      const view = host.view();
      view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.ERROR, true);
      view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, code, true);
    },
    writeType: (slot: number, fqn: string) => {
      writeTypeSpan(host, codecEnv, slot, fqn);
    },
    writeMessageSlot: (slot: number, messageSlot: number) => {
      const view = host.view();
      view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.MESSAGE, true);
      view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, messageSlot, true);
    },
  };
}

/** Arena-copy `fqn` and stamp a CEL_TYPE span (`cel_host.h:712`). */
function writeTypeSpan(
  host: HostEnv,
  codecEnv: CodecEnv,
  slot: number,
  fqn: string,
): void {
  const payload = new TextEncoder().encode(fqn);
  let ptr = 0;
  if (payload.length > 0) {
    ptr = codecEnv.arenaAlloc(payload.length);
    host.bytes().set(payload, ptr);
  }
  const view = host.view();
  view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.TYPE, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ptr, true);
  view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET + 4, payload.length, true);
}

/** Adapt the resolving codec to the `TzAccessorCodec` hook surface. */
function makeTzCodec(host: HostEnv, codecEnv: CodecEnv): TzAccessorCodec {
  return {
    readTimestamp: (slot: number) => {
      const view = host.view();
      return {
        epochSeconds: view.getBigInt64(slot + CEL_VALUE_PAYLOAD_OFFSET, true),
        nanos: view.getInt32(slot + CEL_VALUE_PAYLOAD_OFFSET + 8, true),
      };
    },
    readZone: (slot: number) => {
      const value = resolveCelValue(codecEnv, slot);
      return typeof value === 'string' ? value : '';
    },
    writeInt: (slot: number, value: number) => {
      const view = host.view();
      view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.INT, true);
      view.setBigInt64(slot + CEL_VALUE_PAYLOAD_OFFSET, BigInt(value), true);
    },
    writeError: (slot: number, code: number) => {
      const view = host.view();
      view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.ERROR, true);
      view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, code, true);
    },
  };
}

/** Copy a 24-byte CelValue from `src` to `dst`. */
function copySlot(view: DataView, dst: number, src: number): void {
  for (let i = 0; i < CEL_VALUE_SIZE; i += 4) {
    view.setUint32(dst + i, view.getUint32(src + i, true), true);
  }
}

function readExports(instance: WebAssembly.Instance): ProgramExports {
  const e = instance.exports;
  const memory = e.memory;
  const evalFn = e.eval;
  if (!(memory instanceof WebAssembly.Memory)) {
    throw new CelEvalError('BAD_EXPORTS', "Program does not export 'memory'");
  }
  if (typeof evalFn !== 'function') {
    throw new CelEvalError('BAD_EXPORTS', "Program does not export 'eval'");
  }
  const callCtors = e.__wasm_call_ctors;
  return {
    memory,
    eval: evalFn as () => number,
    arenaInit: requireFn(e, 'arena_init') as (capBytes: number) => void,
    arenaAlloc: requireFn(e, 'arena_alloc') as (n: number) => number,
    malloc: requireFn(e, 'malloc') as (n: number) => number,
    callCtors:
      typeof callCtors === 'function' ? (callCtors as () => void) : undefined,
  };
}

/**
 * Pull the runtime-supplied exports (memory + arena + malloc) off the
 * standalone `cel_runtime.wasm` instance in dynamic mode.  The runtime
 * does NOT export `eval` (the expr module does — see {@link
 * readEvalExport}) and does NOT export `__wasm_call_ctors` (the strip tool
 * exports it on the static Program, not the standalone runtime), so this
 * variant requires only the memory + arena surface.
 */
function readRuntimeExports(instance: WebAssembly.Instance): ProgramExports {
  const e = instance.exports;
  const memory = e.memory;
  if (!(memory instanceof WebAssembly.Memory)) {
    throw new CelEvalError(
      'BAD_EXPORTS',
      "cel_runtime.wasm does not export 'memory'",
    );
  }
  return {
    memory,
    // Filled in by linkDynamic from the expr instance; never called on the
    // runtime instance itself (the runtime has no `eval`).
    eval: () => {
      throw new CelEvalError(
        'BAD_EXPORTS',
        'cel_runtime.wasm has no eval export (the expr module supplies it)',
      );
    },
    arenaInit: requireFn(e, 'arena_init') as (capBytes: number) => void,
    arenaAlloc: requireFn(e, 'arena_alloc') as (n: number) => number,
    malloc: requireFn(e, 'malloc') as (n: number) => number,
    callCtors: undefined,
  };
}

/** Pull the `eval` export off the dynamic expr instance. */
function readEvalExport(instance: WebAssembly.Instance): () => number {
  const evalFn = instance.exports.eval;
  if (typeof evalFn !== 'function') {
    throw new CelEvalError(
      'BAD_EXPORTS',
      "dynamic expr module does not export 'eval'",
    );
  }
  return evalFn as () => number;
}

/**
 * Require a function export by name, throwing BAD_EXPORTS if absent.  The
 * caller casts the result to the concrete arity/return it expects (the
 * arena/malloc exports are `(i32) -> i32`, arena_init is `(i32) -> ()`).
 */
function requireFn(
  exports: WebAssembly.Exports,
  name: string,
): (...args: number[]) => unknown {
  const fn = exports[name];
  if (typeof fn !== 'function') {
    throw new CelEvalError('BAD_EXPORTS', `instance does not export '${name}'`);
  }
  return fn as (...args: number[]) => unknown;
}

/**
 * A protobufjs Root with no types — the descriptor placeholder for a
 * scalar / aggregate Program that declares no message types.  Proto
 * trampolines never run for such a Program; the placeholder only
 * satisfies the ProtoContext shape.
 */
function emptyRoot(): Root {
  return new Root();
}

// Re-export the error message synthesizer so an error result's message is
// available to a caller that constructs a CelError by hand.
export { synthesizeErrorMessage };

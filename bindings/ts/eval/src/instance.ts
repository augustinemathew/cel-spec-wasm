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
 * Thrown when `$eval` traps or the Program's exports are malformed — a
 * host / Program failure, not a CEL spec error (those are CEL_ERROR
 * values, returned).  Carries a `.code` distinguishing the failure mode.
 */
export class CelEvalError extends Error {
  override readonly name = 'CelEvalError';
  readonly code: 'TRAP' | 'BAD_EXPORTS' | 'MARSHAL';

  constructor(code: 'TRAP' | 'BAD_EXPORTS' | 'MARSHAL', message: string) {
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
 * Builds a fresh per-Instance externref table + codec + host-trampoline
 * context, then merges the four host import groups the static Program
 * declares (§A.4.4): the `cel_host` trampolines (aggregate + proto + the
 * timestamp-tz accessor), the registered `cel_fn` functions, `cel_env`,
 * and the WASI stubs.
 */
export async function instantiateProgram(
  program: Program,
  descriptors: DescriptorSet | undefined,
  functions: ReadonlyMap<string, HostFunction>,
): Promise<Instance> {
  const refs = new ExternrefTable();
  const abi = program.abi;

  // The instance handle is populated after instantiation; the host
  // imports close over getters that read this holder, so they see the
  // live exports once instantiation completes.  A mutable holder (not a
  // reassigned `let`) keeps the closures' view of it explicit.
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
  const importObject = buildImports(
    { view, bytes, refs, arenaAlloc, memoryOf },
    codecEnv,
    abi,
    descriptors,
    functions,
  );

  const { instance } = await WebAssembly.instantiate(
    program.wasm.buffer.slice(
      program.wasm.byteOffset,
      program.wasm.byteOffset + program.wasm.byteLength,
    ),
    importObject,
  );
  handle.exports = readExports(instance);
  handle.exports.callCtors?.();
  // Seed the runtime's bump arena once per Instance (mirrors the C++
  // engine's `arena_init(CELWASM_ARENA_CAPACITY_BYTES)` Plan step,
  // `eval/engine.cc:343`).  Without it, the first `$eval` allocation traps
  // `unreachable`.
  handle.exports.arenaInit(ARENA_CAPACITY_BYTES);

  // Re-decode the ABI from the wasm bytes so the Instance owns its own
  // copy (the caller's Program.abi is the same data; decode defensively
  // in case a caller hand-built a Program with an empty abi).
  const resolvedAbi: CelAbi =
    abi.variables.length > 0 || abi.types.length > 0 || abi.fields.length > 0
      ? abi
      : decodeAbi(program.wasm);

  return new Instance(handle.exports, resolvedAbi, refs, descriptors);
}

// ── Import assembly ───────────────────────────────────────────────────

interface HostEnv {
  view(): DataView;
  bytes(): Uint8Array;
  readonly refs: ExternrefTable;
  arenaAlloc(n: number): number;
  memoryOf(): WebAssembly.Memory | undefined;
}

function buildImports(
  host: HostEnv,
  codecEnv: CodecEnv,
  abi: CelAbi,
  descriptors: DescriptorSet | undefined,
  functions: ReadonlyMap<string, HostFunction>,
): WebAssembly.Imports {
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
    cel_host: celHost,
    [CEL_FN_MODULE]: celFn,
    [CEL_ENV_MODULE]: stubs[CEL_ENV_MODULE] ?? {},
    [WASI_MODULE]: stubs[WASI_MODULE] ?? {},
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
  const arenaInit = e.arena_init;
  const arenaAlloc = e.arena_alloc;
  const malloc = e.malloc;
  if (!(memory instanceof WebAssembly.Memory)) {
    throw new CelEvalError('BAD_EXPORTS', "Program does not export 'memory'");
  }
  if (typeof evalFn !== 'function') {
    throw new CelEvalError('BAD_EXPORTS', "Program does not export 'eval'");
  }
  if (typeof arenaInit !== 'function') {
    throw new CelEvalError(
      'BAD_EXPORTS',
      "Program does not export 'arena_init'",
    );
  }
  if (typeof arenaAlloc !== 'function') {
    throw new CelEvalError(
      'BAD_EXPORTS',
      "Program does not export 'arena_alloc'",
    );
  }
  if (typeof malloc !== 'function') {
    throw new CelEvalError('BAD_EXPORTS', "Program does not export 'malloc'");
  }
  const callCtors = e.__wasm_call_ctors;
  return {
    memory,
    eval: evalFn as () => number,
    arenaInit: arenaInit as (capBytes: number) => void,
    arenaAlloc: arenaAlloc as (n: number) => number,
    malloc: malloc as (n: number) => number,
    callCtors:
      typeof callCtors === 'function' ? (callCtors as () => void) : undefined,
  };
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

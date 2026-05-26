/**
 * `Instance` — a live evaluator, the TS counterpart to
 * `compiler_v2/api/instance.cc` (`cel::Instance`). Holds the per-Plan
 * runtime handles (the trampoline context: shared memory + externref
 * table + field table + arena, plus `$eval` + `malloc`) and, per `eval`,
 * marshals the activation into the workspace slots, calls `$eval`, and
 * decodes the result.
 *
 * Marshalling mirrors `instance.cc::MarshalActivation`/`EncodeBoundValue`:
 *   - inline scalars (null/bool/int/uint/double) are stamped in place;
 *   - string/bytes go through `malloc` — NOT the bump arena — because
 *     `$eval`'s first op is `arena_reset`, which would wipe arena-allocated
 *     activation bytes (instance.cc:349 + the P-5 probe);
 *   - host-backed aggregates (message/list/map) are interned in the
 *     externref table and written as a `{kind, slot}` ref CelValue, exactly
 *     as the `cel_host.*` trampolines would (reusing `encodeHostValue`).
 *
 * Decoding handles the full result surface: scalars + error via the
 * codec, host-backed kinds by resolving the externref slot back to its
 * backing, and `UNKNOWN` as the unknown sentinel.
 */
import { type Activation } from './activation.js';
import { Repr, type CelAbi } from './abi.js';
import { CelKind, encodeInlineScalar, type CelValue } from './celvalue.js';
import { encodeHostValue, type TrampolineContext } from './host/trampolines.js';
import { decodeValueAt } from './host/arena-backing.js';
import { valueToHost } from './host/value-backing.js';
import type { Value } from './value.js';

/** Thrown on a marshal failure (unbound variable, kind/repr mismatch, an
 *  unsupported repr) or an unsupported / dangling result. */
export class EvalError extends Error {
  public override readonly name = 'EvalError';
}

/** The per-Plan handles `Engine.plan` hands the Instance. */
export interface RuntimeHandles {
  /** Trampoline context: shared memory, externref table, field table,
   *  arena allocator — shared with the `cel_host.*` imports so a marshalled
   *  backing and a trampoline-read backing live in the same table. */
  readonly ctx: TrampolineContext;
  /** Runtime `malloc(size) -> offset`; for activation span payloads that
   *  must survive `$eval`'s arena_reset. */
  readonly malloc: (size: number) => number;
  /** The expr module's `$eval()` -> result-CelValue byte offset. */
  readonly evalFn: () => number;
  readonly abi: CelAbi | null;
}

const PAYLOAD = 8;

// repr (ir::Repr ordinal) → the inline-scalar CelKind it must carry.
const INLINE_REPR_KIND: ReadonlyMap<number, CelValue['kind']> = new Map([
  [Repr.Null, CelKind.Null],
  [Repr.Bool, CelKind.Bool],
  [Repr.Int, CelKind.Int],
  [Repr.Uint, CelKind.Uint],
  [Repr.Double, CelKind.Double],
]);

// repr (ir::Repr ordinal) → the host-backed CelKind it must carry.
const HOST_REPR_KIND: ReadonlyMap<number, CelKind> = new Map([
  [Repr.Message, CelKind.Message],
  [Repr.List, CelKind.ListHost],
  [Repr.Map, CelKind.MapHost],
]);

export class Instance {
  public constructor(private readonly h: RuntimeHandles) {}

  /**
   * Evaluate once. Resets the externref table, marshals each declared
   * variable from `activation` into its workspace slot, calls `$eval`, and
   * decodes the result into a public `Value`. A declared variable missing
   * from the activation throws `EvalError`; a bound value whose kind
   * disagrees with the declared repr throws `EvalError`.
   */
  public eval(activation?: Activation): Value {
    this.h.ctx.refs.reset();
    if (activation !== undefined) {
      this.marshal(activation);
    }
    return this.decodeResult(this.h.evalFn());
  }

  private get memory(): WebAssembly.Memory {
    return this.h.ctx.memory;
  }

  private memoryBytes(): Uint8Array {
    return new Uint8Array(this.memory.buffer);
  }

  private marshal(activation: Activation): void {
    for (const v of this.h.abi?.variables ?? []) {
      const bound = activation.find(v.name);
      if (bound === undefined) {
        throw new EvalError(`variable '${v.name}' declared but not bound`);
      }
      this.encodeBound(bound, v.repr, v.slotOffset);
    }
  }

  private encodeBound(value: Value, repr: number, slot: number): void {
    const inlineKind = INLINE_REPR_KIND.get(repr);
    if (inlineKind !== undefined) {
      if (value.kind !== inlineKind) {
        throw new EvalError(
          `binding kind mismatch: repr ${repr} wants CelKind ${inlineKind}, ` +
            `got ${value.kind}`,
        );
      }
      encodeInlineScalar(this.memoryBytes(), slot, value);
      return;
    }
    if (repr === Repr.String || repr === Repr.Bytes) {
      this.encodeSpan(value, repr, slot);
      return;
    }
    const hostKind = HOST_REPR_KIND.get(repr);
    if (hostKind !== undefined) {
      this.encodeHostBacked(value, repr, hostKind, slot);
      return;
    }
    throw new EvalError(`activation marshal not implemented for repr ${repr}`);
  }

  // Intern a host-backed binding (message/list/map) and stamp a ref
  // CelValue at `slot`, exactly as a trampoline would.
  private encodeHostBacked(
    value: Value,
    repr: number,
    wantKind: CelKind,
    slot: number,
  ): void {
    if (value.kind !== wantKind) {
      throw new EvalError(
        `binding kind mismatch: repr ${repr} wants CelKind ${wantKind}, ` +
          `got ${value.kind}`,
      );
    }
    encodeHostValue(this.h.ctx, slot, valueToHost(value));
  }

  // Write a string/bytes value into a malloc'd region (survives
  // arena_reset) and stamp a {kind, ptr, len} CelValue at `slot`.
  private encodeSpan(value: Value, repr: number, slot: number): void {
    let bytes: Uint8Array;
    let kind: CelValue['kind'];
    if (repr === Repr.String && value.kind === CelKind.String) {
      bytes = new TextEncoder().encode(value.value);
      kind = CelKind.String;
    } else if (repr === Repr.Bytes && value.kind === CelKind.Bytes) {
      bytes = value.bytes;
      kind = CelKind.Bytes;
    } else {
      throw new EvalError(
        `binding kind mismatch: repr ${repr} wants string/bytes, ` +
          `got ${value.kind}`,
      );
    }
    const ptr = this.h.malloc(Math.max(bytes.length, 1));
    this.memoryBytes().set(bytes, ptr);
    const dv = new DataView(this.memory.buffer);
    dv.setUint32(slot, kind, true);
    dv.setUint32(slot + 4, 0, true);
    dv.setUint32(slot + PAYLOAD, ptr, true);
    dv.setUint32(slot + PAYLOAD + 4, bytes.length, true);
  }

  // Decode the result CelValue at `offset`. Delegated to the shared
  // recursive `decodeValueAt`: scalars/error via the codec, host kinds via
  // the externref table, UNKNOWN as the sentinel, and arena list/map by
  // walking their headers in linear memory (elements/values recurse, so a
  // returned arena list of messages comes back fully resolved).
  private decodeResult(offset: number): Value {
    return decodeValueAt(this.h.ctx, offset);
  }
}

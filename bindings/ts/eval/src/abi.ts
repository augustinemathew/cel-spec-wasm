/**
 * `cel.abi` custom-section decoder — the TS mirror of
 * `compiler_v2/api/internal/abi_decode.cc` + the `abi/cel_abi.proto`
 * schema.
 *
 * Two layers, each using the right tool:
 *   1. The **wasm section walk** (find the `cel.abi` custom section) is
 *      hand-rolled — that's wasm module framing, not protobuf.
 *   2. The **`CelAbi` message** is decoded with the real protobuf reader
 *      (`@bufbuild/protobuf` `fromBinary` over the generated
 *      `CelAbiSchema` in `gen/cel_abi_pb.ts`) — no hand-rolled protobuf.
 *      Regenerate the schema with `npm run proto:gen` when
 *      `cel_abi.proto` changes.
 *
 * The public `CelAbi` / `VariableEntry` / … interfaces below are a thin,
 * stable projection of the generated message types, so callers don't
 * depend on the generated shapes directly.
 */
import { fromBinary } from '@bufbuild/protobuf';
import { CelAbiSchema } from './gen/cel_abi_pb.js';

/**
 * `repr` is the `ir::Repr` ordinal (`compiler_v2/ir/annotations.h`),
 * which is NOT `CelKind` — `Repr` leads with `Unknown = 0`, so an `int`
 * variable carries `repr = 3` and a `string` variable `repr = 6` (P-5
 * finding). The repr→encoder dispatch (instance.ts) keys on these.
 */
export const Repr = {
  Unknown: 0,
  Null: 1,
  Bool: 2,
  Int: 3,
  Uint: 4,
  Double: 5,
  String: 6,
  Bytes: 7,
  List: 8,
  Map: 9,
  Message: 10,
  Enum: 11,
  Duration: 12,
  Timestamp: 13,
  Type: 14,
  Optional: 15,
} as const;

export type Repr = (typeof Repr)[keyof typeof Repr];

/** One declared free variable (`cel.abi.VariableEntry`). */
export interface VariableEntry {
  readonly name: string;
  readonly localIndex: number;
  readonly slotOffset: number;
  /** `ir::Repr` ordinal — see {@link Repr}. */
  readonly repr: number;
}

/** One field intern-table row (`cel.abi.FieldEntry`). */
export interface FieldEntry {
  readonly id: number;
  readonly fieldNumber: number;
  readonly name: string;
  readonly ownerFqn: string;
}

/** One message-type intern-table row (`cel.abi.TypeEntry`). */
export interface TypeEntry {
  readonly id: number;
  readonly fullyQualifiedName: string;
}

/** Decoded `cel.abi` custom section (`cel.abi.CelAbi`). */
export interface CelAbi {
  readonly version: number;
  readonly runtimeAbiVersion: number;
  readonly variables: readonly VariableEntry[];
  readonly fields: readonly FieldEntry[];
  readonly types: readonly TypeEntry[];
}

/** Thrown on a malformed wasm module or `cel.abi` proto payload. */
export class AbiDecodeError extends Error {
  public override readonly name = 'AbiDecodeError';
}

const CEL_ABI_SECTION = 'cel.abi';
const WASM_MAGIC = [0x00, 0x61, 0x73, 0x6d] as const; // "\0asm"

const utf8 = new TextDecoder('utf-8', { fatal: false });

/** Minimal forward cursor for the wasm section walk (LEB128 + framing).
 *  Protobuf parsing is NOT done here — that's `fromBinary`. */
class WasmReader {
  private pos = 0;

  public constructor(private readonly buf: Uint8Array) {}

  public get done(): boolean {
    return this.pos >= this.buf.length;
  }

  public byte(): number {
    const b = this.buf[this.pos++];
    if (b === undefined) {
      throw new AbiDecodeError('unexpected end of wasm buffer');
    }
    return b;
  }

  /** Unsigned LEB128 → number (section ids/sizes/name-lengths fit u32). */
  public varU32(): number {
    let result = 0;
    let shift = 0;
    for (;;) {
      const b = this.byte();
      result += (b & 0x7f) * 2 ** shift;
      if ((b & 0x80) === 0) {
        return result;
      }
      shift += 7;
    }
  }

  /** A length-delimited blob (`len` varint then `len` bytes). */
  public lengthDelimited(): Uint8Array {
    const len = this.varU32();
    const start = this.pos;
    const end = start + len;
    if (end > this.buf.length) {
      throw new AbiDecodeError('wasm section overruns the module');
    }
    this.pos = end;
    return this.buf.subarray(start, end);
  }

  public string(): string {
    return utf8.decode(this.lengthDelimited());
  }

  /** Remaining bytes from the cursor (the section content after its name). */
  public rest(): Uint8Array {
    return this.buf.subarray(this.pos);
  }
}

/**
 * Walk the wasm module's sections and return the payload of the custom
 * section named `name` (the bytes after the section's name vec), or
 * `null` if absent. Throws `AbiDecodeError` on bad magic / truncation.
 */
export function findCustomSection(
  wasm: Uint8Array,
  name: string,
): Uint8Array | null {
  const r = new WasmReader(wasm);
  const b0 = r.byte();
  const b1 = r.byte();
  const b2 = r.byte();
  const b3 = r.byte();
  if (
    b0 !== WASM_MAGIC[0] ||
    b1 !== WASM_MAGIC[1] ||
    b2 !== WASM_MAGIC[2] ||
    b3 !== WASM_MAGIC[3]
  ) {
    throw new AbiDecodeError('not a wasm module (bad magic)');
  }
  r.byte(); // version u32 (4 bytes) — value unused
  r.byte();
  r.byte();
  r.byte();
  while (!r.done) {
    const id = r.byte();
    const payload = r.lengthDelimited(); // section size + content
    if (id === 0) {
      const sub = new WasmReader(payload);
      if (sub.string() === name) {
        return sub.rest();
      }
    }
  }
  return null;
}

/**
 * Decode the `cel.abi` section from a Program's wasm bytes. Returns
 * `null` if the module has no `cel.abi` section. Throws `AbiDecodeError`
 * on malformed framing (the wasm walk) or a malformed proto payload
 * (wrapping the `fromBinary` failure).
 */
export function decodeCelAbi(wasm: Uint8Array): CelAbi | null {
  const payload = findCustomSection(wasm, CEL_ABI_SECTION);
  if (payload === null) {
    return null;
  }
  let msg;
  try {
    msg = fromBinary(CelAbiSchema, payload);
  } catch (cause) {
    throw new AbiDecodeError(
      `cel.abi protobuf decode failed: ${String(cause)}`,
    );
  }
  return {
    version: msg.version,
    runtimeAbiVersion: msg.runtimeAbiVersion,
    variables: msg.variables.map((v) => ({
      name: v.name,
      localIndex: v.localIndex,
      slotOffset: v.slotOffset,
      repr: v.repr,
    })),
    fields: msg.fields.map((f) => ({
      id: f.id,
      fieldNumber: f.fieldNumber,
      name: f.name,
      ownerFqn: f.ownerFqn,
    })),
    types: msg.types.map((t) => ({
      id: t.id,
      fullyQualifiedName: t.fullyQualifiedName,
    })),
  };
}

/** Build a name→VariableEntry lookup (the marshaller's hot path). */
export function variablesByName(abi: CelAbi): Map<string, VariableEntry> {
  const m = new Map<string, VariableEntry>();
  for (const v of abi.variables) {
    m.set(v.name, v);
  }
  return m;
}

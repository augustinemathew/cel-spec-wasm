import * as protobuf from 'protobufjs';
import { FileDescriptorSet } from 'protobufjs/ext/descriptor/index.js';
import { describe, it, expect, beforeEach } from 'vitest';

import {
  readCelValue,
  writeScalarBool,
  writeScalarDouble,
  writeScalarInt,
  writeScalarNull,
  writeSpan,
  encodeUtf8,
} from '../celvalue.js';
import { ExternrefTable } from '../externref.js';
import { ProtoMessageBacking } from '../proto/backing.js';
import { DescriptorSet } from '../proto/descriptors.js';
import {
  CEL_VALUE_KIND_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CEL_VALUE_SIZE,
  CelErrorCode,
  CelKind,
} from '../types.js';
import type { CelValue, FieldEntry, TypeEntry } from '../types.js';

import {
  makeProtoTrampolines,
  type ProtoCodec,
  type ProtoContext,
} from './proto.js';

// ───────────────────────────────────────────────────────────────────
// A descriptor set built in-test (the proto/backing.test.ts pattern,
// round-tripped through a real FileDescriptorSet so it exercises the
// same descriptors.ts load path the assembly WI uses).
// ───────────────────────────────────────────────────────────────────
const ROOT_JSON = {
  nested: {
    test: {
      nested: {
        Color: { values: { RED: 0, GREEN: 1 } },
        User: {
          fields: {
            id: { type: 'int64', id: 1 },
            name: { type: 'string', id: 2 },
          },
        },
        Msg: {
          fields: {
            i64: { type: 'int64', id: 1 },
            u64: { type: 'uint64', id: 2 },
            b: { type: 'bool', id: 4 },
            s: { type: 'string', id: 5 },
            by: { type: 'bytes', id: 6 },
            d: { type: 'double', id: 7 },
            color: { type: 'Color', id: 8 },
            user: { type: 'User', id: 9 },
            tags: { rule: 'repeated', type: 'string', id: 10 },
            ts: { type: 'google.protobuf.Timestamp', id: 12 },
            dur: { type: 'google.protobuf.Duration', id: 13 },
            cnt: { type: 'google.protobuf.Int32Value', id: 14 },
            nm: { type: 'google.protobuf.StringValue', id: 15 },
          },
        },
      },
    },
    google: {
      nested: {
        protobuf: {
          nested: {
            Timestamp: {
              fields: {
                seconds: { type: 'int64', id: 1 },
                nanos: { type: 'int32', id: 2 },
              },
            },
            Duration: {
              fields: {
                seconds: { type: 'int64', id: 1 },
                nanos: { type: 'int32', id: 2 },
              },
            },
            Int32Value: { fields: { value: { type: 'int32', id: 1 } } },
            StringValue: { fields: { value: { type: 'string', id: 1 } } },
          },
        },
      },
    },
  },
};

// Round-trip the JSON root through a serialized FileDescriptorSet so the
// DescriptorSet is loaded the same way the assembly WI loads caller bytes.
function descriptorSet(): DescriptorSet {
  const root = protobuf.Root.fromJSON(ROOT_JSON);
  root.resolveAll();
  const toDescriptor = (
    root as unknown as { toDescriptor: (v: string) => protobuf.Message }
  ).toDescriptor('proto3');
  const bytes = FileDescriptorSet.encode(toDescriptor).finish();
  return DescriptorSet.fromFileDescriptorSet(new Uint8Array(bytes));
}

function msgType(set: DescriptorSet): protobuf.Type {
  return set.messageType('test.Msg');
}

// ───────────────────────────────────────────────────────────────────
// A faithful test codec over a hand-built linear memory.  Mirrors the
// encoder the assembly WI (WI-1.5) supplies: scalars inline, string /
// bytes arena-copied, nested message / list / map interned into the
// externref table as a CEL_*_HOST handle.
// ───────────────────────────────────────────────────────────────────
class TestMemory {
  readonly buffer = new ArrayBuffer(64 * 1024);
  readonly view = new DataView(this.buffer);
  readonly bytes = new Uint8Array(this.buffer);
  // Arena bump pointer; slot region [0, SLOT_BASE) is reserved for
  // hand-placed CelValue slots, the arena grows above it.
  private next = 8 * 1024;

  alloc(n: number): number {
    const off = this.next;
    this.next += (n + 7) & ~7;
    return off;
  }
}

function makeCodec(mem: TestMemory, refs: ExternrefTable): ProtoCodec {
  const writeValue = (slot: number, value: CelValue): void => {
    encodeValue(mem, refs, slot, value);
  };
  return {
    readValue: (slot) => readCelValue(mem.view, slot, mem.bytes),
    writeValue,
    readKind: (slot) =>
      mem.view.getUint32(slot + CEL_VALUE_KIND_OFFSET, true) as CelKind,
    readMessageSlot: (slot) =>
      mem.view.getUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, true),
    copyValue: (dst, src) => {
      mem.bytes.copyWithin(dst, src, src + CEL_VALUE_SIZE);
    },
    writeBool: (slot, value) => {
      writeScalarBool(mem.view, slot, value);
    },
    writeError: (slot, code) => {
      writeError(mem, slot, code);
    },
    writeType: (slot, fqn) => {
      const enc = encodeUtf8(fqn);
      const ptr = mem.alloc(enc.length);
      mem.bytes.set(enc, ptr);
      writeSpanKind(mem, slot, CelKind.TYPE, ptr, enc.length);
    },
    writeMessageSlot: (slot, messageSlot) => {
      writeMessageSlot(mem, slot, messageSlot);
    },
  };
}

function encodeValue(
  mem: TestMemory,
  refs: ExternrefTable,
  slot: number,
  value: CelValue,
): void {
  if (value === null) {
    writeScalarNull(mem.view, slot);
    return;
  }
  if (typeof value === 'boolean') {
    writeScalarBool(mem.view, slot, value);
    return;
  }
  if (typeof value === 'bigint') {
    // These trampolines' decoded bigints are CEL ints (int64 / enum); a
    // wrapper UInt*Value peels to a UINT, but the test reads it back as a
    // bigint either way, so encode all bigints as INT here.
    writeScalarInt(mem.view, slot, value);
    return;
  }
  if (typeof value === 'number') {
    writeScalarDouble(mem.view, slot, value);
    return;
  }
  if (typeof value === 'string') {
    const enc = encodeUtf8(value);
    const ptr = mem.alloc(enc.length);
    mem.bytes.set(enc, ptr);
    writeSpan(mem.view, slot, CelKind.STRING, ptr, enc.length);
    return;
  }
  if (value instanceof Uint8Array) {
    const ptr = mem.alloc(value.length);
    mem.bytes.set(value, ptr);
    writeSpan(mem.view, slot, CelKind.BYTES, ptr, value.length);
    return;
  }
  if (Array.isArray(value)) {
    const listSlot = refs.list.intern(value);
    writeRefKind(mem, slot, CelKind.LIST_HOST, listSlot);
    return;
  }
  if (value instanceof Map) {
    const mapSlot = refs.map.intern(value);
    writeRefKind(mem, slot, CelKind.MAP_HOST, mapSlot);
    return;
  }
  // CelValue's message member ({[k]:CelValue}) defeats discriminant
  // narrowing, so bind each tagged member explicitly.
  if ('kind' in value && value.kind === 'timestamp') {
    const ts = value as Extract<CelValue, { kind: 'timestamp' }>;
    writeDurTs(mem, slot, CelKind.TIMESTAMP, ts.epochSeconds, ts.nanos);
    return;
  }
  if ('kind' in value && value.kind === 'duration') {
    const d = value as Extract<CelValue, { kind: 'duration' }>;
    writeDurTs(mem, slot, CelKind.DURATION, d.seconds, d.nanos);
    return;
  }
  if ('kind' in value && value.kind === 'error') {
    const e = value as Extract<CelValue, { kind: 'error' }>;
    writeError(mem, slot, e.code);
    return;
  }
  // A decoded message object — not produced by these trampolines' tests
  // (nested messages are interned via writeMessageSlot), so reject loudly.
  throw new Error('encodeValue: unexpected message-object value');
}

function writeError(mem: TestMemory, slot: number, code: number): void {
  mem.view.setUint32(slot + CEL_VALUE_KIND_OFFSET, CelKind.ERROR, true);
  mem.view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, code, true);
}

function writeMessageSlot(mem: TestMemory, slot: number, ref: number): void {
  writeRefKind(mem, slot, CelKind.MESSAGE, ref);
}

function writeRefKind(
  mem: TestMemory,
  slot: number,
  kind: CelKind,
  ref: number,
): void {
  mem.view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
  mem.view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ref, true);
}

function writeSpanKind(
  mem: TestMemory,
  slot: number,
  kind: CelKind,
  ptr: number,
  len: number,
): void {
  mem.view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
  mem.view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET, ptr, true);
  mem.view.setUint32(slot + CEL_VALUE_PAYLOAD_OFFSET + 4, len, true);
}

function writeDurTs(
  mem: TestMemory,
  slot: number,
  kind: CelKind,
  seconds: bigint,
  nanos: number,
): void {
  mem.view.setUint32(slot + CEL_VALUE_KIND_OFFSET, kind, true);
  mem.view.setBigInt64(slot + CEL_VALUE_PAYLOAD_OFFSET, seconds, true);
  mem.view.setInt32(slot + CEL_VALUE_PAYLOAD_OFFSET + 8, nanos, true);
}

// ───────────────────────────────────────────────────────────────────
// Slot bookkeeping: each named slot is a 24-byte CelValue cell in the
// reserved low region.
// ───────────────────────────────────────────────────────────────────
const OUT = 0;
const A = CEL_VALUE_SIZE;
const B = CEL_VALUE_SIZE * 2;
const C = CEL_VALUE_SIZE * 3;

// ───────────────────────────────────────────────────────────────────
// ABI intern tables.  Index 0 is the sentinel in each; field/type ids
// start at 1.  `fieldNumber === 0` would mean "by name only".
// ───────────────────────────────────────────────────────────────────
const FIELDS: readonly FieldEntry[] = [
  { id: 0, fieldNumber: 0, name: '', ownerFqn: '' },
  { id: 1, fieldNumber: 1, name: 'i64', ownerFqn: 'test.Msg' },
  { id: 2, fieldNumber: 5, name: 's', ownerFqn: 'test.Msg' },
  { id: 3, fieldNumber: 9, name: 'user', ownerFqn: 'test.Msg' },
  { id: 4, fieldNumber: 12, name: 'ts', ownerFqn: 'test.Msg' },
  { id: 5, fieldNumber: 14, name: 'cnt', ownerFqn: 'test.Msg' },
  { id: 6, fieldNumber: 0, name: 'name', ownerFqn: 'test.User' },
  { id: 7, fieldNumber: 99, name: 'nope', ownerFqn: 'test.Msg' },
];
const F_I64 = 1;
const F_S = 2;
const F_USER = 3;
const F_TS = 4;
const F_CNT = 5;
const F_USER_NAME = 6;
const F_UNKNOWN = 7;

const TYPES: readonly TypeEntry[] = [
  { id: 0, fullyQualifiedName: '' },
  { id: 1, fullyQualifiedName: 'test.Msg' },
  { id: 2, fullyQualifiedName: 'test.User' },
  { id: 3, fullyQualifiedName: 'google.protobuf.Timestamp' },
  { id: 4, fullyQualifiedName: 'test.Missing' },
];
const T_MSG = 1;
const T_USER = 2;
const T_MISSING = 4;

// ───────────────────────────────────────────────────────────────────
// Per-test context.
// ───────────────────────────────────────────────────────────────────
interface Harness {
  mem: TestMemory;
  refs: ExternrefTable;
  ctx: ProtoContext;
  set: DescriptorSet;
  tramps: ReturnType<typeof makeProtoTrampolines>;
}

function makeHarness(): Harness {
  const set = descriptorSet();
  const mem = new TestMemory();
  const refs = new ExternrefTable();
  const codec = makeCodec(mem, refs);
  const ctx: ProtoContext = {
    codec,
    refs,
    fields: FIELDS,
    types: TYPES,
    descriptors: set,
    arenaAlloc: (n) => mem.alloc(n),
  };
  return { mem, refs, ctx, set, tramps: makeProtoTrampolines(ctx) };
}

// Intern a test.Msg built from `obj`, write its CEL_MESSAGE at `slot`,
// return the externref slot index.
function internMsg(
  h: Harness,
  slot: number,
  obj: Record<string, unknown>,
): number {
  const type = msgType(h.set);
  const ref = h.refs.message.intern(
    new ProtoMessageBacking(type, type.fromObject(obj)),
  );
  writeMessageSlot(h.mem, slot, ref);
  return ref;
}

function read(h: Harness, slot: number): CelValue {
  return readCelValue(h.mem.view, slot, h.mem.bytes);
}

// ───────────────────────────────────────────────────────────────────
// cel_get_field
// ───────────────────────────────────────────────────────────────────
describe('cel_get_field', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  it('reads a set scalar field', () => {
    internMsg(h, A, { i64: 42 });
    h.tramps.cel_get_field(OUT, A, F_I64, 0);
    expect(read(h, OUT)).toBe(42n);
  });

  it('reads a set string field (arena-copied)', () => {
    internMsg(h, A, { s: 'hello' });
    h.tramps.cel_get_field(OUT, A, F_S, 0);
    expect(read(h, OUT)).toBe('hello');
  });

  it('reads an unset proto3 scalar as the default', () => {
    internMsg(h, A, {});
    h.tramps.cel_get_field(OUT, A, F_I64, 0);
    expect(read(h, OUT)).toBe(0n);
  });

  // langdef §"Field Selection": selecting an unset singular message field
  // yields the default instance, not null (cel-cpp ReadSingularMessageField
  // serves reflection's GetMessage default; corpus
  // proto3/empty_field/nested_message).
  it('reads an unset message field as the default-instance message', () => {
    internMsg(h, A, {});
    h.tramps.cel_get_field(OUT, A, F_USER, 0);
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.MESSAGE);
    // A sub-field read chained through the default instance serves the
    // sub-field's default (corpus proto3/empty_field/nested_message_subfield).
    h.tramps.cel_get_field(B, OUT, F_USER_NAME, 0);
    expect(read(h, B)).toBe('');
  });

  it('reads a set nested message as a chainable CEL_MESSAGE slot', () => {
    internMsg(h, A, { user: { id: 7, name: 'amy' } });
    h.tramps.cel_get_field(OUT, A, F_USER, 0);
    // OUT now holds a CEL_MESSAGE; chain a read of user.name through it.
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.MESSAGE);
    h.tramps.cel_get_field(B, OUT, F_USER_NAME, 0);
    expect(read(h, B)).toBe('amy');
  });

  it('peels a Timestamp WKT field to a tagged record', () => {
    internMsg(h, A, { ts: { seconds: 5, nanos: 250 } });
    h.tramps.cel_get_field(OUT, A, F_TS, 0);
    expect(read(h, OUT)).toEqual({
      kind: 'timestamp',
      epochSeconds: 5n,
      nanos: 250,
    });
  });

  it('peels an Int32Value wrapper field to a scalar', () => {
    internMsg(h, A, { cnt: { value: 99 } });
    h.tramps.cel_get_field(OUT, A, F_CNT, 0);
    expect(read(h, OUT)).toBe(99n);
  });

  it('errors FIELD_NOT_FOUND on an unknown field on the type', () => {
    internMsg(h, A, {});
    h.tramps.cel_get_field(OUT, A, F_UNKNOWN, 0);
    expect(read(h, OUT)).toEqual({
      kind: 'error',
      code: CelErrorCode.FIELD_NOT_FOUND,
      message: expect.any(String) as unknown as string,
    });
  });

  it('errors FIELD_NOT_FOUND when the message slot is unmapped', () => {
    writeMessageSlot(h.mem, A, 999);
    h.tramps.cel_get_field(OUT, A, F_I64, 0);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.FIELD_NOT_FOUND,
    );
  });

  it('absorbs an ERROR operand (copies it to out)', () => {
    writeError(h.mem, A, CelErrorCode.DIVIDE_BY_ZERO);
    h.tramps.cel_get_field(OUT, A, F_I64, 0);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.DIVIDE_BY_ZERO,
    );
  });

  it('absorbs an UNKNOWN operand (copies it to out)', () => {
    h.mem.view.setUint32(A + CEL_VALUE_KIND_OFFSET, CelKind.UNKNOWN, true);
    h.mem.view.setUint32(A + CEL_VALUE_PAYLOAD_OFFSET, 0, true);
    h.tramps.cel_get_field(OUT, A, F_I64, 0);
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.UNKNOWN);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_has_field
// ───────────────────────────────────────────────────────────────────
describe('cel_has_field', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  it('reports a set scalar present', () => {
    internMsg(h, A, { s: 'x' });
    h.tramps.cel_has_field(OUT, A, F_S, 0);
    expect(read(h, OUT)).toBe(true);
  });

  it('reports an unset scalar absent', () => {
    internMsg(h, A, {});
    h.tramps.cel_has_field(OUT, A, F_S, 0);
    expect(read(h, OUT)).toBe(false);
  });

  it('reports a set message field present and unset absent', () => {
    internMsg(h, A, { user: { id: 1 } });
    h.tramps.cel_has_field(OUT, A, F_USER, 0);
    expect(read(h, OUT)).toBe(true);
    internMsg(h, B, {});
    h.tramps.cel_has_field(OUT, B, F_USER, 0);
    expect(read(h, OUT)).toBe(false);
  });

  it('absorbs an ERROR operand', () => {
    writeError(h.mem, A, CelErrorCode.NO_SUCH_KEY);
    h.tramps.cel_has_field(OUT, A, F_S, 0);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.NO_SUCH_KEY,
    );
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_make_message / cel_set_field
// ───────────────────────────────────────────────────────────────────
describe('cel_make_message + cel_set_field', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  it('constructs, sets, and reads back a scalar field', () => {
    h.tramps.cel_make_message(T_MSG, OUT);
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.MESSAGE);
    // set s = "built"
    encodeStringSlot(h, A, 'built');
    h.tramps.cel_set_field(OUT, F_S, A);
    h.tramps.cel_get_field(B, OUT, F_S, 0);
    expect(read(h, B)).toBe('built');
  });

  it('constructs an empty message whose unset field reads the default', () => {
    h.tramps.cel_make_message(T_MSG, OUT);
    h.tramps.cel_get_field(A, OUT, F_I64, 0);
    expect(read(h, A)).toBe(0n);
  });

  it('errors UNKNOWN_TYPE for a type id not in the descriptor set', () => {
    h.tramps.cel_make_message(T_MISSING, OUT);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.UNKNOWN_TYPE,
    );
  });

  it('errors UNKNOWN_TYPE for a type id past the table', () => {
    h.tramps.cel_make_message(999, OUT);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.UNKNOWN_TYPE,
    );
  });

  it('cel_set_field is a no-op for a poisoned value', () => {
    h.tramps.cel_make_message(T_MSG, OUT);
    writeError(h.mem, A, CelErrorCode.OVERFLOW);
    h.tramps.cel_set_field(OUT, F_S, A);
    h.tramps.cel_get_field(B, OUT, F_S, 0);
    // s was never set → proto3 default empty string.
    expect(read(h, B)).toBe('');
  });

  // An out-of-range 32-bit narrowing (here: an Int32Value wrapper field set
  // from an int outside the int32 domain) poisons the MESSAGE slot with
  // CEL_ERROR(OVERFLOW), so the construction's result carries the range
  // error — mirrors `CelSetFieldImpl`'s kOutOfRange arm
  // (`eval/internal/cel_host.cc`; corpus dynamic/int32/field_assign_*_range).
  it('poisons the message slot on an out-of-range field assignment', () => {
    h.tramps.cel_make_message(T_MSG, OUT);
    writeScalarInt(h.mem.view, A, 12345678900n);
    h.tramps.cel_set_field(OUT, F_CNT, A);
    expect((read(h, OUT) as { code: number }).code).toBe(CelErrorCode.OVERFLOW);
    // A later cel_set_field on the poisoned slot is a no-op (the poison
    // rides out as the construction's value).
    encodeStringSlot(h, B, 'late');
    h.tramps.cel_set_field(OUT, F_S, B);
    expect((read(h, OUT) as { code: number }).code).toBe(CelErrorCode.OVERFLOW);
  });

  it('sets a nested User message field via a constructed sub-message', () => {
    h.tramps.cel_make_message(T_USER, A);
    encodeStringSlot(h, B, 'sub');
    h.tramps.cel_set_field(A, F_USER_NAME, B);
    h.tramps.cel_make_message(T_MSG, OUT);
    h.tramps.cel_set_field(OUT, F_USER, A);
    h.tramps.cel_get_field(C, OUT, F_USER, 0);
    h.tramps.cel_get_field(A, C, F_USER_NAME, 0);
    expect(read(h, A)).toBe('sub');
  });
});

// Write a CEL_STRING at `slot` (helper for set-field tests).
function encodeStringSlot(h: Harness, slot: number, value: string): void {
  const enc = encodeUtf8(value);
  const ptr = h.mem.alloc(enc.length);
  h.mem.bytes.set(enc, ptr);
  writeSpan(h.mem.view, slot, CelKind.STRING, ptr, enc.length);
}

// ───────────────────────────────────────────────────────────────────
// cel_wkt_unwrap_time / cel_wkt_unwrap_wrapper
// ───────────────────────────────────────────────────────────────────
describe('cel_wkt_unwrap_time', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  function internWkt(
    slot: number,
    fqn: string,
    obj: Record<string, unknown>,
  ): void {
    const type = h.set.messageType(fqn);
    const ref = h.refs.message.intern(
      new ProtoMessageBacking(type, type.fromObject(obj)),
    );
    writeMessageSlot(h.mem, slot, ref);
  }

  it('peels a Timestamp to a tagged record', () => {
    internWkt(A, 'google.protobuf.Timestamp', { seconds: 9, nanos: 8 });
    h.tramps.cel_wkt_unwrap_time(OUT, A);
    expect(read(h, OUT)).toEqual({
      kind: 'timestamp',
      epochSeconds: 9n,
      nanos: 8,
    });
  });

  it('peels a Duration to a tagged record', () => {
    internWkt(A, 'google.protobuf.Duration', { seconds: 3, nanos: 1 });
    h.tramps.cel_wkt_unwrap_time(OUT, A);
    expect(read(h, OUT)).toEqual({ kind: 'duration', seconds: 3n, nanos: 1 });
  });

  it('errors TYPE_MISMATCH on a non-time message', () => {
    internMsg(h, A, {});
    h.tramps.cel_wkt_unwrap_time(OUT, A);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.TYPE_MISMATCH,
    );
  });

  it('absorbs an ERROR operand', () => {
    writeError(h.mem, A, CelErrorCode.INVALID_ARGUMENT);
    h.tramps.cel_wkt_unwrap_time(OUT, A);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.INVALID_ARGUMENT,
    );
  });
});

describe('cel_wkt_unwrap_wrapper', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  function internWrapper(
    slot: number,
    fqn: string,
    obj: Record<string, unknown>,
  ): void {
    const type = h.set.messageType(fqn);
    const ref = h.refs.message.intern(
      new ProtoMessageBacking(type, type.fromObject(obj)),
    );
    writeMessageSlot(h.mem, slot, ref);
  }

  it('peels an Int32Value to an INT scalar', () => {
    internWrapper(A, 'google.protobuf.Int32Value', { value: 77 });
    h.tramps.cel_wkt_unwrap_wrapper(OUT, A, CelKind.INT);
    expect(read(h, OUT)).toBe(77n);
  });

  it('peels a StringValue to a string scalar', () => {
    internWrapper(A, 'google.protobuf.StringValue', { value: 'wrapped' });
    h.tramps.cel_wkt_unwrap_wrapper(OUT, A, CelKind.STRING);
    expect(read(h, OUT)).toBe('wrapped');
  });

  it('errors TYPE_MISMATCH for a non-wrapper message', () => {
    internMsg(h, A, {});
    h.tramps.cel_wkt_unwrap_wrapper(OUT, A, CelKind.INT);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.TYPE_MISMATCH,
    );
  });

  it('errors TYPE_MISMATCH for an out-of-range wrapper_kind', () => {
    internWrapper(A, 'google.protobuf.Int32Value', { value: 1 });
    h.tramps.cel_wkt_unwrap_wrapper(OUT, A, 99);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.TYPE_MISMATCH,
    );
  });

  it('absorbs an UNKNOWN operand', () => {
    h.mem.view.setUint32(A + CEL_VALUE_KIND_OFFSET, CelKind.UNKNOWN, true);
    h.mem.view.setUint32(A + CEL_VALUE_PAYLOAD_OFFSET, 0, true);
    h.tramps.cel_wkt_unwrap_wrapper(OUT, A, CelKind.INT);
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.UNKNOWN);
  });
});

// ───────────────────────────────────────────────────────────────────
// cel_message_eq / cel_message_is_zero
// ───────────────────────────────────────────────────────────────────
describe('cel_message_eq', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  it('reports two equal messages equal', () => {
    internMsg(h, A, { i64: 1, s: 'x' });
    internMsg(h, B, { i64: 1, s: 'x' });
    h.tramps.cel_message_eq(OUT, A, B);
    expect(read(h, OUT)).toBe(true);
  });

  it('reports two unequal messages unequal', () => {
    internMsg(h, A, { i64: 1 });
    internMsg(h, B, { i64: 2 });
    h.tramps.cel_message_eq(OUT, A, B);
    expect(read(h, OUT)).toBe(false);
  });

  it('errors TYPE_MISMATCH when one operand is unmapped', () => {
    internMsg(h, A, {});
    writeMessageSlot(h.mem, B, 999);
    h.tramps.cel_message_eq(OUT, A, B);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.TYPE_MISMATCH,
    );
  });

  it('absorbs an ERROR on either operand', () => {
    internMsg(h, A, {});
    writeError(h.mem, B, CelErrorCode.OVERFLOW);
    h.tramps.cel_message_eq(OUT, A, B);
    expect((read(h, OUT) as { code: number }).code).toBe(CelErrorCode.OVERFLOW);
  });
});

describe('cel_message_is_zero', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  it('reports an empty message zero', () => {
    internMsg(h, A, {});
    h.tramps.cel_message_is_zero(OUT, A);
    expect(read(h, OUT)).toBe(true);
  });

  it('reports a message with a set field non-zero', () => {
    internMsg(h, A, { i64: 1 });
    h.tramps.cel_message_is_zero(OUT, A);
    expect(read(h, OUT)).toBe(false);
  });

  it('errors HOST_ADAPTER_ERROR when the slot is unmapped', () => {
    writeMessageSlot(h.mem, A, 999);
    h.tramps.cel_message_is_zero(OUT, A);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.HOST_ADAPTER_ERROR,
    );
  });

  it('absorbs an UNKNOWN operand', () => {
    h.mem.view.setUint32(A + CEL_VALUE_KIND_OFFSET, CelKind.UNKNOWN, true);
    h.mem.view.setUint32(A + CEL_VALUE_PAYLOAD_OFFSET, 0, true);
    h.tramps.cel_message_is_zero(OUT, A);
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.UNKNOWN);
  });
});

// ───────────────────────────────────────────────────────────────────
// resolve_message_type_name
// ───────────────────────────────────────────────────────────────────
describe('resolve_message_type_name', () => {
  let h: Harness;
  beforeEach(() => {
    h = makeHarness();
  });

  it('resolves the FQN as a CEL_TYPE value', () => {
    internMsg(h, A, {});
    h.tramps.resolve_message_type_name(OUT, A);
    expect(h.mem.view.getUint32(OUT, true)).toBe(CelKind.TYPE);
    // Decode the TYPE span manually (the codec rejects TYPE on read).
    const ptr = h.mem.view.getUint32(OUT + CEL_VALUE_PAYLOAD_OFFSET, true);
    const len = h.mem.view.getUint32(OUT + CEL_VALUE_PAYLOAD_OFFSET + 4, true);
    const fqn = new TextDecoder().decode(h.mem.bytes.subarray(ptr, ptr + len));
    expect(fqn).toBe('test.Msg');
  });

  it('errors UNKNOWN_TYPE when the slot is unmapped', () => {
    writeMessageSlot(h.mem, A, 999);
    h.tramps.resolve_message_type_name(OUT, A);
    expect((read(h, OUT) as { code: number }).code).toBe(
      CelErrorCode.UNKNOWN_TYPE,
    );
  });

  it('absorbs an ERROR operand', () => {
    writeError(h.mem, A, CelErrorCode.TIMEOUT);
    h.tramps.resolve_message_type_name(OUT, A);
    expect((read(h, OUT) as { code: number }).code).toBe(CelErrorCode.TIMEOUT);
  });
});

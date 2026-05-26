/**
 * `message-builder.ts` — `createMessage` + `setField` (the write side of
 * `cel_make_message` / `cel_set_field`). Hermetic: uses `HostMsg3`
 * (committed `host_msg3.fds.bin`), which carries a field of every proto
 * scalar wire type + an enum, so every `scalarProtoValue` arm is exercised;
 * round-trips each set value back through the read backing.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
  createFileRegistry,
  fromBinary,
  type DescMessage,
} from '@bufbuild/protobuf';
import { FileDescriptorSetSchema } from '@bufbuild/protobuf/wkt';
import { CelKind, type CelValue } from '../../src/celvalue.js';
import {
  MessageBuildError,
  createMessage,
  setField,
} from '../../src/host/message-builder.js';
import { Value } from '../../src/value.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const fds = fromBinary(
  FileDescriptorSetSchema,
  readFileSync(join(HERE, '../testdata/host_msg3.fds.bin')),
);
const HOSTMSG3 = createFileRegistry(fds).getMessage(
  'celwasm.testdata.HostMsg3',
);
if (HOSTMSG3 === undefined) throw new Error('HostMsg3 descriptor missing');
const desc: DescMessage = HOSTMSG3;

describe('createMessage', () => {
  it('makes an empty mutable backing of the type', () => {
    const b = createMessage(desc);
    expect(b.descriptor.typeName).toBe('celwasm.testdata.HostMsg3');
    // Unset scalar reads as its proto default.
    expect(b.getField('i32')).toEqual({
      host: 'scalar',
      value: { kind: CelKind.Int, int: 0n },
    });
  });
});

describe('setField — every scalar wire type round-trips', () => {
  // field name → [value to set, expected read-back CelValue]
  const CASES: [string, CelValue, CelValue][] = [
    ['b', Value.bool(true), { kind: CelKind.Bool, bool: true }],
    ['i32', Value.int(5n), { kind: CelKind.Int, int: 5n }],
    ['i64', Value.int(7n), { kind: CelKind.Int, int: 7n }],
    ['si32', Value.int(-3n), { kind: CelKind.Int, int: -3n }],
    ['si64', Value.int(-9n), { kind: CelKind.Int, int: -9n }],
    ['sfx32', Value.int(11n), { kind: CelKind.Int, int: 11n }],
    ['sfx64', Value.int(13n), { kind: CelKind.Int, int: 13n }],
    ['u32', Value.uint(4n), { kind: CelKind.Uint, uint: 4n }],
    ['u64', Value.uint(6n), { kind: CelKind.Uint, uint: 6n }],
    ['fx32', Value.uint(8n), { kind: CelKind.Uint, uint: 8n }],
    ['fx64', Value.uint(10n), { kind: CelKind.Uint, uint: 10n }],
    ['f32', Value.double(1.5), { kind: CelKind.Double, double: 1.5 }],
    ['f64', Value.double(2.5), { kind: CelKind.Double, double: 2.5 }],
    ['s', Value.string('hi'), { kind: CelKind.String, value: 'hi' }],
    [
      'by',
      Value.bytes(new Uint8Array([1, 2])),
      { kind: CelKind.Bytes, bytes: new Uint8Array([1, 2]) },
    ],
    ['kind', Value.int(7n), { kind: CelKind.Int, int: 7n }], // enum → int
  ];

  it.each(CASES)('%s', (name, value, want) => {
    const b = createMessage(desc);
    setField(b, name, value);
    expect(b.getField(name)).toEqual({ host: 'scalar', value: want });
  });
});

describe('setField — null clears the field', () => {
  it('a set field cleared by a null value reads back unset', () => {
    const b = createMessage(desc);
    setField(b, 's', Value.string('x'));
    expect(b.hasField('s')).toBe(true);
    setField(b, 's', Value.null());
    expect(b.hasField('s')).toBe(false);
  });
});

describe('setField — rejects bad inputs', () => {
  it('unknown field', () => {
    expect(() => {
      setField(createMessage(desc), 'nope', Value.int(1n));
    }).toThrow(MessageBuildError);
  });

  it('scalar kind mismatch (int field, bool value)', () => {
    expect(() => {
      setField(createMessage(desc), 'i32', Value.bool(true));
    }).toThrow(/wants CEL int/);
  });
  it('uint field, int value', () => {
    expect(() => {
      setField(createMessage(desc), 'u32', Value.int(1n));
    }).toThrow(/wants CEL uint/);
  });
  it('double / bool / string / bytes mismatches', () => {
    expect(() => {
      setField(createMessage(desc), 'f64', Value.int(1n));
    }).toThrow(/wants CEL double/);
    expect(() => {
      setField(createMessage(desc), 'b', Value.int(1n));
    }).toThrow(/wants CEL bool/);
    expect(() => {
      setField(createMessage(desc), 's', Value.int(1n));
    }).toThrow(/wants CEL string/);
    expect(() => {
      setField(createMessage(desc), 'by', Value.int(1n));
    }).toThrow(/wants CEL bytes/);
  });
  it('enum field, non-int value', () => {
    expect(() => {
      setField(createMessage(desc), 'kind', Value.string('x'));
    }).toThrow(/enum field wants CEL int/);
  });

  it('unsupported field kinds (message / list / map)', () => {
    expect(() => {
      setField(createMessage(desc), 'inner', Value.int(1n));
    }).toThrow(/message field 'inner' is not supported/);
    expect(() => {
      setField(createMessage(desc), 'rep_i32', Value.int(1n));
    }).toThrow(/list field 'rep_i32' is not supported/);
    expect(() => {
      setField(createMessage(desc), 'str_to_i32', Value.int(1n));
    }).toThrow(/map field 'str_to_i32' is not supported/);
  });
});

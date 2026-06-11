import { describe, expect, it } from 'vitest';

import {
  CelExternrefBoundaryError,
  CelUnsupportedKindError,
  encodeUtf8,
  readCelValue,
  synthesizeErrorMessage,
  writeScalarBool,
  writeScalarDouble,
  writeScalarInt,
  writeScalarNull,
  writeScalarUint,
  writeSpan,
} from './celvalue.js';
import {
  ARENA_HEADER_SIZE,
  ARENA_LIST_ELEMENT_STRIDE,
  ARENA_MAP_ENTRY_STRIDE,
  CEL_DURTS_NANOS_OFFSET,
  CEL_DURTS_SECONDS_OFFSET,
  CEL_SPAN_LEN_OFFSET,
  CEL_SPAN_PTR_OFFSET,
  CEL_VALUE_PAYLOAD_OFFSET,
  CEL_VALUE_SIZE,
  CelErrorCode,
  CelKind,
} from './types.js';
import type { CelDuration, CelError, CelTimestamp, CelValue } from './types.js';

// ───────────────────────────────────────────────────────────────────
// A tiny linear-memory builder.  Hand-stamps CelValue bytes per the
// frozen layout (`runtime/cel_data.h`) so the codec is exercised
// against bytes we constructed independently of it.
// ───────────────────────────────────────────────────────────────────
const MEM_BYTES = 4096;

function newMemory(): { view: DataView; bytes: Uint8Array } {
  const buf = new ArrayBuffer(MEM_BYTES);
  return { view: new DataView(buf), bytes: new Uint8Array(buf) };
}

/** Stamp a raw CelValue header at `offset`. */
function stampKind(view: DataView, offset: number, kind: CelKind): void {
  view.setUint32(offset, kind, /* littleEndian */ true);
}

describe('readCelValue — scalars', () => {
  it('decodes NULL → null', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.NULL);
    expect(readCelValue(view, 0, bytes)).toBeNull();
  });

  it('decodes BOOL → boolean', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.BOOL);
    view.setInt32(CEL_VALUE_PAYLOAD_OFFSET, 1, true);
    expect(readCelValue(view, 0, bytes)).toBe(true);

    stampKind(view, 32, CelKind.BOOL);
    view.setInt32(32 + CEL_VALUE_PAYLOAD_OFFSET, 0, true);
    expect(readCelValue(view, 32, bytes)).toBe(false);
  });

  it.each<[string, bigint]>([
    ['zero', 0n],
    ['minus one', -1n],
    ['INT64_MAX', 9223372036854775807n],
    ['INT64_MIN', -9223372036854775808n],
  ])('decodes INT (%s) → bigint', (_name, value) => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.INT);
    view.setBigInt64(CEL_VALUE_PAYLOAD_OFFSET, value, true);
    expect(readCelValue(view, 0, bytes)).toBe(value);
  });

  it.each<[string, bigint]>([
    ['zero', 0n],
    ['one', 1n],
    ['UINT64_MAX', 18446744073709551615n],
  ])('decodes UINT (%s) → bigint', (_name, value) => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.UINT);
    view.setBigUint64(CEL_VALUE_PAYLOAD_OFFSET, value, true);
    expect(readCelValue(view, 0, bytes)).toBe(value);
  });

  it.each<[string, number]>([
    ['zero', 0],
    ['pi', 3.141592653589793],
    ['negative', -2.5],
  ])('decodes DOUBLE (%s) → number', (_name, value) => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.DOUBLE);
    view.setFloat64(CEL_VALUE_PAYLOAD_OFFSET, value, true);
    expect(readCelValue(view, 0, bytes)).toBe(value);
  });

  it('decodes DOUBLE NaN', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.DOUBLE);
    view.setFloat64(CEL_VALUE_PAYLOAD_OFFSET, NaN, true);
    expect(readCelValue(view, 0, bytes)).toBeNaN();
  });
});

describe('readCelValue — string / bytes spans', () => {
  // Place the CelValue at offset 0 and the payload bytes at offset 256.
  const PAYLOAD_PTR = 256;

  function buildSpan(
    kind: CelKind.STRING | CelKind.BYTES,
    raw: Uint8Array,
  ): {
    view: DataView;
    bytes: Uint8Array;
  } {
    const { view, bytes } = newMemory();
    stampKind(view, 0, kind);
    view.setUint32(CEL_SPAN_PTR_OFFSET, PAYLOAD_PTR, true);
    view.setUint32(CEL_SPAN_LEN_OFFSET, raw.length, true);
    bytes.set(raw, PAYLOAD_PTR);
    return { view, bytes };
  }

  it('decodes an empty STRING → ""', () => {
    const { view, bytes } = buildSpan(CelKind.STRING, new Uint8Array(0));
    expect(readCelValue(view, 0, bytes)).toBe('');
  });

  it('decodes an ASCII STRING', () => {
    const { view, bytes } = buildSpan(CelKind.STRING, encodeUtf8('hello'));
    expect(readCelValue(view, 0, bytes)).toBe('hello');
  });

  it('decodes a multi-byte UTF-8 STRING ("héllo")', () => {
    // "héllo" — the é is 2 bytes (U+00E9), so byte length is 6, not 5.
    const raw = encodeUtf8('héllo');
    expect(raw.length).toBe(6);
    const { view, bytes } = buildSpan(CelKind.STRING, raw);
    expect(readCelValue(view, 0, bytes)).toBe('héllo');
  });

  it('decodes BYTES → Uint8Array (incl. embedded NUL)', () => {
    const raw = new Uint8Array([0x00, 0xff, 0x00, 0x41, 0x00]);
    const { view, bytes } = buildSpan(CelKind.BYTES, raw);
    const out = readCelValue(view, 0, bytes);
    expect(out).toBeInstanceOf(Uint8Array);
    expect(Array.from(out as Uint8Array)).toEqual([
      0x00, 0xff, 0x00, 0x41, 0x00,
    ]);
  });

  it('decodes an empty BYTES → empty Uint8Array', () => {
    const { view, bytes } = buildSpan(CelKind.BYTES, new Uint8Array(0));
    const out = readCelValue(view, 0, bytes);
    expect(out).toBeInstanceOf(Uint8Array);
    expect((out as Uint8Array).length).toBe(0);
  });

  it('returns a COPY of the bytes (stable across memory mutation)', () => {
    const { view, bytes } = buildSpan(CelKind.BYTES, new Uint8Array([1, 2, 3]));
    const out = readCelValue(view, 0, bytes) as Uint8Array;
    bytes[PAYLOAD_PTR] = 99; // mutate underlying memory
    expect(Array.from(out)).toEqual([1, 2, 3]);
  });
});

describe('readCelValue — timestamp / duration', () => {
  it('decodes TIMESTAMP → tagged record', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.TIMESTAMP);
    view.setBigInt64(CEL_DURTS_SECONDS_OFFSET, 1_700_000_000n, true);
    view.setInt32(CEL_DURTS_NANOS_OFFSET, 123_456_789, true);
    const out = readCelValue(view, 0, bytes) as CelTimestamp;
    expect(out).toEqual({
      kind: 'timestamp',
      epochSeconds: 1_700_000_000n,
      nanos: 123_456_789,
    });
  });

  it('decodes DURATION → tagged record (negative seconds)', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.DURATION);
    view.setBigInt64(CEL_DURTS_SECONDS_OFFSET, -5n, true);
    view.setInt32(CEL_DURTS_NANOS_OFFSET, -250, true);
    const out = readCelValue(view, 0, bytes) as CelDuration;
    expect(out).toEqual({ kind: 'duration', seconds: -5n, nanos: -250 });
  });
});

describe('readCelValue — error', () => {
  it('decodes a divide-by-zero ERROR (code 11) → CelError', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.ERROR);
    view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, CelErrorCode.DIVIDE_BY_ZERO, true);
    const out = readCelValue(view, 0, bytes) as CelError;
    expect(out.kind).toBe('error');
    expect(out.code).toBe(11);
    expect(out.message).toBe('divide by zero');
  });

  it('decodes a NO_SUCH_KEY ERROR (code 15)', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.ERROR);
    view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, CelErrorCode.NO_SUCH_KEY, true);
    const out = readCelValue(view, 0, bytes) as CelError;
    expect(out.code).toBe(15);
    expect(out.message).toBe('no such key');
  });
});

// ───────────────────────────────────────────────────────────────────
// Arena aggregates.  Hand-build a header (16 B) + an element/entry run.
// ───────────────────────────────────────────────────────────────────
describe('readCelValue — arena list', () => {
  const HEADER_PTR = 128;
  const ELEMENTS_PTR = 512;

  function buildList(elements: bigint[]): {
    view: DataView;
    bytes: Uint8Array;
  } {
    const { view, bytes } = newMemory();
    // The CelValue at offset 0 points at the header.
    stampKind(view, 0, CelKind.LIST_ARENA);
    view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, HEADER_PTR, true);
    // Header: {count, capacity, elements_offset, _pad}.
    view.setUint32(HEADER_PTR, elements.length, true);
    view.setUint32(HEADER_PTR + 4, elements.length, true);
    view.setUint32(HEADER_PTR + 8, ELEMENTS_PTR, true);
    // Elements: count × 24-byte INT CelValues.
    elements.forEach((value, i) => {
      const off = ELEMENTS_PTR + i * ARENA_LIST_ELEMENT_STRIDE;
      stampKind(view, off, CelKind.INT);
      view.setBigInt64(off + CEL_VALUE_PAYLOAD_OFFSET, value, true);
    });
    return { view, bytes };
  }

  it('pins the header size', () => {
    expect(ARENA_HEADER_SIZE).toBe(16);
  });

  it('decodes an empty list → []', () => {
    const { view, bytes } = buildList([]);
    expect(readCelValue(view, 0, bytes)).toEqual([]);
  });

  it('decodes a list of ints → bigint[]', () => {
    const { view, bytes } = buildList([1n, -2n, 9223372036854775807n]);
    expect(readCelValue(view, 0, bytes)).toEqual([
      1n,
      -2n,
      9223372036854775807n,
    ]);
  });
});

describe('readCelValue — arena map', () => {
  const HEADER_PTR = 128;
  const ENTRIES_PTR = 512;

  it('decodes a map with a string key → Map', () => {
    const { view, bytes } = newMemory();
    const KEY_BYTES_PTR = 1024;
    stampKind(view, 0, CelKind.MAP_ARENA);
    view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, HEADER_PTR, true);
    // Header: count=1.
    view.setUint32(HEADER_PTR, 1, true);
    view.setUint32(HEADER_PTR + 4, 1, true);
    view.setUint32(HEADER_PTR + 8, ENTRIES_PTR, true);
    // Entry 0: key STRING "k" at [ENTRIES_PTR], value INT 42 at +24.
    const keyOff = ENTRIES_PTR;
    const raw = encodeUtf8('k');
    stampKind(view, keyOff, CelKind.STRING);
    view.setUint32(keyOff + CEL_SPAN_PTR_OFFSET, KEY_BYTES_PTR, true);
    view.setUint32(keyOff + CEL_SPAN_LEN_OFFSET, raw.length, true);
    bytes.set(raw, KEY_BYTES_PTR);
    const valOff = ENTRIES_PTR + ARENA_LIST_ELEMENT_STRIDE;
    stampKind(view, valOff, CelKind.INT);
    view.setBigInt64(valOff + CEL_VALUE_PAYLOAD_OFFSET, 42n, true);

    const out = readCelValue(view, 0, bytes) as Map<CelValue, CelValue>;
    expect(out).toBeInstanceOf(Map);
    expect(out.size).toBe(1);
    expect(out.get('k')).toBe(42n);
  });

  it('decodes an empty map → empty Map', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.MAP_ARENA);
    view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, HEADER_PTR, true);
    view.setUint32(HEADER_PTR, 0, true);
    view.setUint32(HEADER_PTR + 8, ENTRIES_PTR, true);
    const out = readCelValue(view, 0, bytes) as Map<CelValue, CelValue>;
    expect(out).toBeInstanceOf(Map);
    expect(out.size).toBe(0);
  });

  it('pins the entry stride at 48 bytes', () => {
    expect(ARENA_MAP_ENTRY_STRIDE).toBe(48);
  });
});

describe('readCelValue — externref boundary', () => {
  it.each<[string, CelKind]>([
    ['MESSAGE', CelKind.MESSAGE],
    ['MAP_HOST', CelKind.MAP_HOST],
    ['LIST_HOST', CelKind.LIST_HOST],
  ])(
    'throws CelExternrefBoundaryError for %s, carrying the slot',
    (_name, kind) => {
      const { view, bytes } = newMemory();
      stampKind(view, 0, kind);
      view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, 7, true);
      try {
        readCelValue(view, 0, bytes);
        expect.unreachable('should have thrown');
      } catch (err) {
        expect(err).toBeInstanceOf(CelExternrefBoundaryError);
        expect((err as CelExternrefBoundaryError).slot).toBe(7);
        expect((err as CelExternrefBoundaryError).celKind).toBe(kind);
      }
    },
  );
});

describe('readCelValue — out-of-scope kinds', () => {
  it.each<[string, CelKind]>([
    ['OPTIONAL', CelKind.OPTIONAL],
    ['UNKNOWN', CelKind.UNKNOWN],
    ['TYPE', CelKind.TYPE],
    ['IP', CelKind.IP],
    ['CIDR', CelKind.CIDR],
  ])('throws CelUnsupportedKindError for %s', (_name, kind) => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, kind);
    expect(() => readCelValue(view, 0, bytes)).toThrow(CelUnsupportedKindError);
  });
});

// ───────────────────────────────────────────────────────────────────
// Little-endianness — assert the codec reads LE, not host-native.  We
// stamp a known byte pattern manually and check the decoded value.
// ───────────────────────────────────────────────────────────────────
describe('endianness', () => {
  it('reads INT as little-endian', () => {
    const { view, bytes } = newMemory();
    stampKind(view, 0, CelKind.INT);
    // 0x0102030405060708 little-endian: low byte 0x08 first.
    const raw = [0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01];
    raw.forEach((b, i) => {
      bytes[CEL_VALUE_PAYLOAD_OFFSET + i] = b;
    });
    expect(readCelValue(view, 0, bytes)).toBe(0x0102030405060708n);
  });

  it('reads the kind discriminant as little-endian', () => {
    const { view, bytes } = newMemory();
    // ERROR = 16 = 0x10; little-endian first byte 0x10, rest 0.
    bytes[0] = 0x10;
    bytes[1] = 0x00;
    bytes[2] = 0x00;
    bytes[3] = 0x00;
    view.setUint32(CEL_VALUE_PAYLOAD_OFFSET, CelErrorCode.OVERFLOW, true);
    const out = readCelValue(view, 0, bytes) as CelError;
    expect(out.kind).toBe('error');
    expect(out.code).toBe(CelErrorCode.OVERFLOW);
  });
});

// ───────────────────────────────────────────────────────────────────
// Write helpers — round-trip via readCelValue.
// ───────────────────────────────────────────────────────────────────
describe('writeScalar* → readCelValue round-trip', () => {
  it('round-trips NULL', () => {
    const { view, bytes } = newMemory();
    writeScalarNull(view, 0);
    expect(readCelValue(view, 0, bytes)).toBeNull();
  });

  it.each<[boolean]>([[true], [false]])('round-trips BOOL %s', (value) => {
    const { view, bytes } = newMemory();
    writeScalarBool(view, 0, value);
    expect(readCelValue(view, 0, bytes)).toBe(value);
  });

  it.each<bigint>([0n, -1n, 9223372036854775807n, -9223372036854775808n])(
    'round-trips INT %s',
    (value) => {
      const { view, bytes } = newMemory();
      writeScalarInt(view, 0, value);
      expect(readCelValue(view, 0, bytes)).toBe(value);
    },
  );

  it.each<bigint>([0n, 1n, 18446744073709551615n])(
    'round-trips UINT %s',
    (value) => {
      const { view, bytes } = newMemory();
      writeScalarUint(view, 0, value);
      expect(readCelValue(view, 0, bytes)).toBe(value);
    },
  );

  it.each<number>([0, -2.5, 3.141592653589793])(
    'round-trips DOUBLE %s',
    (value) => {
      const { view, bytes } = newMemory();
      writeScalarDouble(view, 0, value);
      expect(readCelValue(view, 0, bytes)).toBe(value);
    },
  );

  it('writes a STRING span over caller-allocated bytes', () => {
    const { view, bytes } = newMemory();
    const PTR = 300;
    const raw = encodeUtf8('héllo');
    bytes.set(raw, PTR);
    writeSpan(view, 0, CelKind.STRING, PTR, raw.length);
    expect(readCelValue(view, 0, bytes)).toBe('héllo');
  });

  it('writes a BYTES span over caller-allocated bytes', () => {
    const { view, bytes } = newMemory();
    const PTR = 300;
    const raw = new Uint8Array([0, 255, 0]);
    bytes.set(raw, PTR);
    writeSpan(view, 0, CelKind.BYTES, PTR, raw.length);
    const out = readCelValue(view, 0, bytes) as Uint8Array;
    expect(Array.from(out)).toEqual([0, 255, 0]);
  });

  it('writes scalars at a non-zero offset (slot_offset > 0)', () => {
    const { view, bytes } = newMemory();
    const slot = CEL_VALUE_SIZE * 3;
    writeScalarInt(view, slot, 777n);
    expect(readCelValue(view, slot, bytes)).toBe(777n);
  });
});

describe('synthesizeErrorMessage', () => {
  it.each<[CelErrorCode, string]>([
    [CelErrorCode.OVERFLOW, 'integer overflow'],
    [CelErrorCode.DIVIDE_BY_ZERO, 'divide by zero'],
    [CelErrorCode.MODULUS_BY_ZERO, 'modulus by zero'],
    [CelErrorCode.TYPE_MISMATCH, 'no matching overload'],
    [CelErrorCode.NO_SUCH_KEY, 'no such key'],
    [CelErrorCode.DUPLICATE_KEY, 'duplicate key in map literal'],
    [CelErrorCode.INDEX_OUT_OF_BOUNDS, 'index out of bounds'],
    [CelErrorCode.INVALID_ARGUMENT, 'invalid argument'],
    [CelErrorCode.FIELD_NOT_FOUND, 'no such field'],
    [CelErrorCode.TIMEOUT, 'evaluation timed out'],
  ])('maps code %i to a human message', (code, message) => {
    expect(synthesizeErrorMessage(code)).toBe(message);
  });

  it('falls back for an unknown code', () => {
    expect(synthesizeErrorMessage(999)).toBe('cel error (code 999)');
  });
});

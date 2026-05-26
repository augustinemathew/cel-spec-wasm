import { describe, it, expect } from 'vitest';
import {
  AbiDecodeError,
  Repr,
  decodeCelAbi,
  findCustomSection,
  variablesByName,
} from '../src/abi.js';

// ── synthetic protobuf + wasm encoders (test-only) ──────────────────
function uvarint(n: number): number[] {
  const out: number[] = [];
  let v = n;
  do {
    let b = v & 0x7f;
    v = Math.floor(v / 128);
    if (v > 0) {
      b |= 0x80;
    }
    out.push(b);
  } while (v > 0);
  return out;
}
function tag(field: number, wire: number): number[] {
  return uvarint((field << 3) | wire);
}
function vField(field: number, n: number): number[] {
  return [...tag(field, 0), ...uvarint(n)];
}
function bytesField(field: number, bytes: number[]): number[] {
  return [...tag(field, 2), ...uvarint(bytes.length), ...bytes];
}
function sField(field: number, s: string): number[] {
  return bytesField(field, [...new TextEncoder().encode(s)]);
}
function variableEntry(
  name: string,
  localIndex: number,
  slotOffset: number,
  repr: number,
): number[] {
  return [
    ...sField(1, name),
    ...vField(2, localIndex),
    ...vField(3, slotOffset),
    ...vField(4, repr),
  ];
}
function customSection(name: string, content: number[]): number[] {
  const nameBytes = [...new TextEncoder().encode(name)];
  const payload = [...uvarint(nameBytes.length), ...nameBytes, ...content];
  return [0x00, ...uvarint(payload.length), ...payload];
}
function rawSection(id: number, content: number[]): number[] {
  return [id, ...uvarint(content.length), ...content];
}
function wasm(...sections: number[][]): Uint8Array {
  return new Uint8Array([
    0x00,
    0x61,
    0x73,
    0x6d,
    0x01,
    0,
    0,
    0,
    ...sections.flat(),
  ]);
}

// ── findCustomSection ───────────────────────────────────────────────
describe('findCustomSection', () => {
  it('returns null when the named section is absent', () => {
    const w = wasm(rawSection(1, [1, 2, 3]), customSection('other', [4, 5]));
    expect(findCustomSection(w, 'cel.abi')).toBeNull();
  });

  it('finds cel.abi past a non-custom section and a different custom one', () => {
    const w = wasm(
      rawSection(1, [9, 9]),
      customSection('name', [1, 2]),
      customSection('cel.abi', vField(1, 7)),
    );
    const payload = findCustomSection(w, 'cel.abi');
    expect(payload).not.toBeNull();
    expect([...(payload ?? [])]).toEqual(vField(1, 7));
  });

  it('throws on bad wasm magic', () => {
    expect(() =>
      findCustomSection(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]), 'cel.abi'),
    ).toThrow(AbiDecodeError);
  });
});

// ── decodeCelAbi ────────────────────────────────────────────────────
describe('decodeCelAbi', () => {
  it('returns null when the module has no cel.abi section', () => {
    expect(decodeCelAbi(wasm(rawSection(1, [1])))).toBeNull();
  });

  it('an empty section decodes to an empty CelAbi (variable-free Eval)', () => {
    expect(decodeCelAbi(wasm(customSection('cel.abi', [])))).toEqual({
      version: 0,
      runtimeAbiVersion: 0,
      variables: [],
      fields: [],
      types: [],
    });
  });

  it('decodes variables (repr = ir::Repr ordinal) + versions', () => {
    const content = [
      ...vField(1, 2), // version
      ...bytesField(2, variableEntry('x', 0, 40, Repr.Int)),
      ...bytesField(2, variableEntry('s', 1, 48, Repr.String)),
      ...vField(6, 2), // runtime_abi_version
    ];
    const abi = decodeCelAbi(wasm(customSection('cel.abi', content)));
    expect(abi?.version).toBe(2);
    expect(abi?.runtimeAbiVersion).toBe(2);
    expect(abi?.variables).toEqual([
      { name: 'x', localIndex: 0, slotOffset: 40, repr: 3 },
      { name: 's', localIndex: 1, slotOffset: 48, repr: 6 },
    ]);
  });

  it('handles a section larger than 127 bytes (multi-byte LEB128 size)', () => {
    // A long variable name pushes the cel.abi section past 127 bytes, so
    // the section-size varint is multi-byte (exercises the LEB128
    // continuation path in the wasm-section reader).
    const longName = 'v'.repeat(200);
    const abi = decodeCelAbi(
      wasm(
        customSection(
          'cel.abi',
          bytesField(2, variableEntry(longName, 0, 8, Repr.Int)),
        ),
      ),
    );
    expect(abi?.variables).toEqual([
      { name: longName, localIndex: 0, slotOffset: 8, repr: 3 },
    ]);
  });

  it('decodes field + type intern rows', () => {
    const field = [
      ...vField(1, 1),
      ...vField(2, 1),
      ...sField(3, 'name'),
      ...sField(4, 'acme.User'),
    ];
    const type = [...vField(1, 1), ...sField(2, 'acme.User')];
    const abi = decodeCelAbi(
      wasm(
        customSection('cel.abi', [
          ...bytesField(3, field),
          ...bytesField(5, type),
        ]),
      ),
    );
    expect(abi?.fields).toEqual([
      { id: 1, fieldNumber: 1, name: 'name', ownerFqn: 'acme.User' },
    ]);
    expect(abi?.types).toEqual([{ id: 1, fullyQualifiedName: 'acme.User' }]);
  });

  it('skips unknown fields at every wire type (forward-compat)', () => {
    const content = [
      ...vField(1, 1),
      ...bytesField(99, [1, 2, 3]), // unknown len-delimited
      ...vField(7, 123), // unknown varint
      ...tag(8, 5),
      0,
      0,
      0,
      0, // unknown 32-bit
      ...tag(9, 1),
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      0, // unknown 64-bit
      ...bytesField(2, variableEntry('x', 0, 40, Repr.Int)),
    ];
    const abi = decodeCelAbi(wasm(customSection('cel.abi', content)));
    expect(abi?.version).toBe(1);
    expect(abi?.variables).toEqual([
      { name: 'x', localIndex: 0, slotOffset: 40, repr: 3 },
    ]);
  });

  it('ignores unknown sub-fields inside a VariableEntry', () => {
    const entry = [
      ...sField(1, 'y'),
      ...bytesField(5, [7, 7]), // reserved-5 (future full CelType) — skip
      ...vField(3, 56),
    ];
    const abi = decodeCelAbi(
      wasm(customSection('cel.abi', bytesField(2, entry))),
    );
    expect(abi?.variables).toEqual([
      { name: 'y', localIndex: 0, slotOffset: 56, repr: 0 },
    ]);
  });

  it('ignores unknown sub-fields inside a FieldEntry', () => {
    const entry = [...vField(1, 5), ...vField(9, 1), ...sField(3, 'n')];
    const abi = decodeCelAbi(
      wasm(customSection('cel.abi', bytesField(3, entry))),
    );
    expect(abi?.fields).toEqual([
      { id: 5, fieldNumber: 0, name: 'n', ownerFqn: '' },
    ]);
  });

  it('ignores unknown sub-fields inside a TypeEntry', () => {
    const entry = [...vField(1, 3), ...sField(7, 'skip'), ...sField(2, 'fqn')];
    const abi = decodeCelAbi(
      wasm(customSection('cel.abi', bytesField(5, entry))),
    );
    expect(abi?.types).toEqual([{ id: 3, fullyQualifiedName: 'fqn' }]);
  });
});

// ── variablesByName ─────────────────────────────────────────────────
describe('variablesByName', () => {
  it('builds a name lookup; misses are undefined', () => {
    const abi = decodeCelAbi(
      wasm(
        customSection(
          'cel.abi',
          bytesField(2, variableEntry('x', 0, 40, Repr.Int)),
        ),
      ),
    );
    expect(abi).not.toBeNull();
    const byName = variablesByName(abi!);
    expect(byName.get('x')?.slotOffset).toBe(40);
    expect(byName.get('missing')).toBeUndefined();
  });
});

// ── negative paths ──────────────────────────────────────────────────
describe('findCustomSection / wasm-walk negatives', () => {
  it('throws on bad wasm magic', () => {
    expect(() =>
      findCustomSection(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]), 'cel.abi'),
    ).toThrow(/bad magic/);
  });

  it('throws on a truncated module header', () => {
    // valid magic, but the 4-byte version is cut short.
    expect(() =>
      findCustomSection(new Uint8Array([0x00, 0x61, 0x73, 0x6d, 1, 0]), 'x'),
    ).toThrow(/unexpected end of wasm buffer/);
  });

  it('throws when a section size overruns the module', () => {
    // header + section id 1 claiming 99 bytes, none follow.
    const w = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 0x01, 99]);
    expect(() => findCustomSection(w, 'cel.abi')).toThrow(
      /section overruns the module/,
    );
  });
});

describe('decodeCelAbi negative (malformed proto → wrapped)', () => {
  // These payloads are valid wasm framing but malformed protobuf; the
  // real protobuf reader (`fromBinary`) rejects them and decodeCelAbi
  // wraps the failure as AbiDecodeError.
  it('wraps a length-delimited field that overruns', () => {
    const content = [...tag(2, 2), ...uvarint(99)]; // claims 99 bytes, none follow
    const fn = (): unknown =>
      decodeCelAbi(wasm(customSection('cel.abi', content)));
    expect(fn).toThrow(AbiDecodeError);
    expect(fn).toThrow(/protobuf decode failed/);
  });

  it('wraps a truncated varint', () => {
    const content = [...tag(1, 0), 0x80]; // continuation bit set, buffer ends
    expect(() => decodeCelAbi(wasm(customSection('cel.abi', content)))).toThrow(
      AbiDecodeError,
    );
  });

  it('wraps a fixed-width field that overruns', () => {
    const content = [...tag(8, 5), 0, 0]; // wire 5 needs 4 bytes; only 2 follow
    expect(() => decodeCelAbi(wasm(customSection('cel.abi', content)))).toThrow(
      AbiDecodeError,
    );
  });
});

import { describe, it, expect } from 'vitest';
import { Program } from '../src/program.js';
import { Repr } from '../src/abi.js';

// Minimal synthetic wasm builders (a cel.abi-carrying module).
function uvarint(n: number): number[] {
  const out: number[] = [];
  let v = n;
  do {
    let b = v & 0x7f;
    v = Math.floor(v / 128);
    if (v > 0) b |= 0x80;
    out.push(b);
  } while (v > 0);
  return out;
}
function customSection(name: string, content: number[]): number[] {
  const nameBytes = [...new TextEncoder().encode(name)];
  const payload = [...uvarint(nameBytes.length), ...nameBytes, ...content];
  return [0x00, ...uvarint(payload.length), ...payload];
}
function wasm(sections: number[]): Uint8Array {
  return new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0, 0, 0, ...sections]);
}
// One VariableEntry { name:"x"(f1), slot_offset:40(f3), repr:Int(f4) }.
function celAbiWithVarX(): number[] {
  const entry = [
    0x0a,
    0x01,
    0x78, // f1 (string) len 1 "x"
    0x18,
    40, // f3 (varint) slot_offset
    0x20,
    Repr.Int, // f4 (varint) repr
  ];
  const variableField = [0x12, entry.length, ...entry]; // f2 (len-delim)
  return customSection('cel.abi', variableField);
}

describe('Program.fromBytes', () => {
  it('exposes the wasm bytes verbatim', () => {
    const bytes = wasm(customSection('cel.abi', []));
    const p = Program.fromBytes(bytes);
    expect(p.wasmBytes).toBe(bytes);
  });

  it('decodes the cel.abi section when present', () => {
    const p = Program.fromBytes(wasm(celAbiWithVarX()));
    expect(p.abi).not.toBeNull();
    expect(p.abi?.variables).toEqual([
      { name: 'x', localIndex: 0, slotOffset: 40, repr: 3 },
    ]);
  });

  it('abi is null when the module carries no cel.abi section', () => {
    // A module with only a (non-custom) section id 1.
    const p = Program.fromBytes(wasm([0x01, 0x01, 0x00]));
    expect(p.abi).toBeNull();
  });
});

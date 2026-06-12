// Tests for the `cel.abi` section decoder against the REAL golden
// Programs in `eval/fixtures/` (compiled by the C++ `cel` CLI).  The
// fixtures are the contract the codec satisfies: if the C++ side
// renumbers a proto field or moves the section, these tests fail loudly
// rather than the decoder silently misreading bytes.
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.4.3;
//       wire schema `abi/cel_abi.proto`.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { describe, expect, it } from 'vitest';

import { CelAbiError, decodeAbi } from './abi.js';
import { LinkMode } from './types.js';

// ───────────────────────────────────────────────────────────────────
// Fixture loading.  The manifest is the source of truth for each
// fixture's declared variables (`compileVars`, e.g. "x:int").
// ───────────────────────────────────────────────────────────────────

const FIXTURES_DIR = fileURLToPath(new URL('../fixtures/', import.meta.url));

interface ManifestFixture {
  readonly name: string;
  readonly expr: string;
  readonly compileVars: readonly string[];
}

interface Manifest {
  readonly fixtures: readonly ManifestFixture[];
}

const manifest = JSON.parse(
  readFileSync(`${FIXTURES_DIR}manifest.json`, 'utf-8'),
) as Manifest;

function fixtureBytes(name: string): Uint8Array {
  return new Uint8Array(readFileSync(`${FIXTURES_DIR}${name}.wasm`));
}

function manifestFixture(name: string): ManifestFixture {
  const fx = manifest.fixtures.find((f) => f.name === name);
  if (fx === undefined) {
    throw new Error(`fixture "${name}" not present in manifest.json`);
  }
  return fx;
}

/** The variable names a fixture declares, in `compileVars` order. */
function expectedVarNames(name: string): string[] {
  return manifestFixture(name).compileVars.map((v) => {
    const colon = v.indexOf(':');
    return colon < 0 ? v : v.slice(0, colon);
  });
}

describe('decodeAbi — golden Program fixtures', () => {
  // No-variable program: `1 + 2`.
  it('decodes a no-variable program to an empty variables array (int_add)', () => {
    const abi = decodeAbi(fixtureBytes('int_add'));
    expect(abi.variables).toEqual([]);
    expect(abi.linkMode).toBe(LinkMode.STATIC);
  });

  // Single-variable program: `x` (int).
  it('decodes a single int variable (var_int_identity → x:int)', () => {
    const abi = decodeAbi(fixtureBytes('var_int_identity'));
    expect(abi.variables.map((v) => v.name)).toEqual(['x']);
    expect(abi.linkMode).toBe(LinkMode.STATIC);
  });

  // Single string-typed variable: `name` (string).
  it('decodes a single string variable (var_string_identity → name:string)', () => {
    const abi = decodeAbi(fixtureBytes('var_string_identity'));
    expect(abi.variables.map((v) => v.name)).toEqual(['name']);
  });

  // Multi-variable program: `x + y` (both int).
  it('decodes two int variables (var_int_add → x, y)', () => {
    const abi = decodeAbi(fixtureBytes('var_int_add'));
    expect(abi.variables.map((v) => v.name)).toEqual(['x', 'y']);
    expect(abi.linkMode).toBe(LinkMode.STATIC);
  });

  // Mixed int + string variables: `age >= 18 && country in [...]`.
  it('decodes mixed int + string variables (policy_age_country_true → age, country)', () => {
    const abi = decodeAbi(fixtureBytes('policy_age_country_true'));
    expect(abi.variables.map((v) => v.name)).toEqual(['age', 'country']);
    expect(abi.linkMode).toBe(LinkMode.STATIC);
  });

  // Every variable entry carries the marshal-table fields, indexed by
  // local_index (cel_abi.proto:226-229 — position i holds local_index i).
  // `repr` is a numeric mirror of `ir::Repr` (annotations.h): kInt is 3,
  // kString is 6 (kUnknown=0, kNull=1, kBool=2, kInt=3, kUint=4,
  // kDouble=5, kString=6, kBytes=7 ...).
  const REPR_INT = 3;
  const REPR_STRING = 6;

  it('populates marshal-table fields (localIndex / slotOffset / repr) for each variable', () => {
    const abi = decodeAbi(fixtureBytes('var_int_add'));
    expect(abi.variables).toHaveLength(2);
    abi.variables.forEach((v, i) => {
      expect(v.localIndex).toBe(i);
      // slot_offset is an absolute linear-memory byte offset; distinct
      // per variable and non-negative.
      expect(v.slotOffset).toBeGreaterThanOrEqual(0);
      expect(Number.isInteger(v.slotOffset)).toBe(true);
      // Both vars are int-typed.
      expect(v.repr).toBe(REPR_INT);
    });
    const offsets = abi.variables.map((v) => v.slotOffset);
    expect(new Set(offsets).size).toBe(offsets.length);
  });

  // Pins `repr` to the ir::Repr discriminant per declared type: a mixed
  // int + string program decodes age→int(3), country→string(6).
  it('decodes per-variable repr matching the declared type (policy: age int, country string)', () => {
    const abi = decodeAbi(fixtureBytes('policy_age_country_true'));
    const byName = new Map(abi.variables.map((v) => [v.name, v.repr]));
    expect(byName.get('age')).toBe(REPR_INT);
    expect(byName.get('country')).toBe(REPR_STRING);
  });

  // Programs are emitted with the runtime statically linked (the golden
  // fixtures are self-contained Programs).
  it('reports STATIC link mode and a non-zero runtime ABI version', () => {
    const abi = decodeAbi(fixtureBytes('int_add'));
    expect(abi.linkMode).toBe(LinkMode.STATIC);
    expect(abi.runtimeAbiVersion).toBeGreaterThan(0);
    expect(abi.version).toBeGreaterThanOrEqual(0);
  });

  // Cross-check every fixture's decoded variable names against the
  // manifest's compileVars — the whole corpus, not just the spotlighted
  // ones.
  it.each(manifest.fixtures.map((f) => f.name))(
    'decodes %s variable names matching manifest compileVars',
    (name) => {
      const abi = decodeAbi(fixtureBytes(name));
      expect(abi.variables.map((v) => v.name)).toEqual(expectedVarNames(name));
    },
  );
});

describe('decodeAbi — rejection', () => {
  it('rejects a non-wasm Uint8Array (bad magic)', () => {
    expect(() => decodeAbi(new Uint8Array([1, 2, 3]))).toThrow(CelAbiError);
    try {
      decodeAbi(new Uint8Array([1, 2, 3]));
      expect.unreachable('decodeAbi should have thrown');
    } catch (err) {
      expect(err).toBeInstanceOf(CelAbiError);
      expect((err as CelAbiError).code).toBe('NOT_WASM');
    }
  });

  it('rejects an empty buffer', () => {
    expect(() => decodeAbi(new Uint8Array([]))).toThrow(CelAbiError);
  });

  it('rejects a wasm header with the wrong magic but full length', () => {
    // 8 bytes so the length check passes, but the magic is wrong.
    const bytes = new Uint8Array([
      0xde, 0xad, 0xbe, 0xef, 0x01, 0x00, 0x00, 0x00,
    ]);
    try {
      decodeAbi(bytes);
      expect.unreachable('decodeAbi should have thrown');
    } catch (err) {
      expect(err).toBeInstanceOf(CelAbiError);
      expect((err as CelAbiError).code).toBe('NOT_WASM');
    }
  });

  it('rejects a valid wasm module that lacks a cel.abi section', () => {
    // Minimal valid wasm: magic + version 1, no sections.
    const bytes = new Uint8Array([
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ]);
    try {
      decodeAbi(bytes);
      expect.unreachable('decodeAbi should have thrown');
    } catch (err) {
      expect(err).toBeInstanceOf(CelAbiError);
      expect((err as CelAbiError).code).toBe('NO_ABI_SECTION');
    }
  });
});

/**
 * Fixture manifest consistency — the guard that makes "which expression is
 * this .wasm?" always answerable. Every compiled `.wasm` in testdata must
 * have a `fixtures.json` entry (the CEL source it was compiled from), and
 * every manifest entry must point at a real `.wasm`. If `gen_fixtures.sh`
 * adds a fixture without recording its source (or vice-versa), this fails.
 */
import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { TESTDATA } from './harness.js';

const SOURCES = JSON.parse(
  readFileSync(join(TESTDATA, 'fixtures.json'), 'utf8'),
) as Record<string, string>;

const WASM_FILES = readdirSync(TESTDATA).filter((f) => f.endsWith('.wasm'));

describe('fixture manifest', () => {
  it('every compiled .wasm has a source expression in fixtures.json', () => {
    const undocumented = WASM_FILES.filter((f) => !(f in SOURCES));
    expect(undocumented).toEqual([]);
  });

  it('every manifest entry points at an existing .wasm', () => {
    const dangling = Object.keys(SOURCES).filter(
      (f) => !WASM_FILES.includes(f),
    );
    expect(dangling).toEqual([]);
  });

  it('every source expression is a non-empty string', () => {
    for (const [fixture, expr] of Object.entries(SOURCES)) {
      expect(expr, fixture).toBeTypeOf('string');
      expect(expr.length, fixture).toBeGreaterThan(0);
    }
  });
});

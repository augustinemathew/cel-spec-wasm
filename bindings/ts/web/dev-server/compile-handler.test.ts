import { describe, expect, it } from 'vitest';

import { parseCompileRequest, runCompile } from './compile-handler.js';

describe('parseCompileRequest', () => {
  it('accepts a minimal source-only body', () => {
    expect(parseCompileRequest({ source: '1 + 2' })).toEqual({
      source: '1 + 2',
    });
  });

  it('accepts vars and optimizeLevel', () => {
    const req = parseCompileRequest({
      source: 'age > 18',
      vars: [{ name: 'age', type: 'int' }],
      optimizeLevel: 2,
    });
    expect(req.source).toBe('age > 18');
    expect(req.vars).toEqual([{ name: 'age', type: 'int' }]);
    expect(req.optimizeLevel).toBe(2);
  });

  it('rejects a non-object body', () => {
    expect(() => parseCompileRequest(null)).toThrow(TypeError);
    expect(() => parseCompileRequest('x')).toThrow(TypeError);
  });

  it('rejects a missing or non-string source', () => {
    expect(() => parseCompileRequest({})).toThrow(/source/);
    expect(() => parseCompileRequest({ source: 5 })).toThrow(/source/);
  });

  it('rejects malformed vars', () => {
    expect(() => parseCompileRequest({ source: 'x', vars: 'no' })).toThrow(
      /vars/,
    );
    expect(() =>
      parseCompileRequest({ source: 'x', vars: [{ name: 'a' }] }),
    ).toThrow(/vars\[0\]/);
  });

  it('rejects an out-of-range optimizeLevel', () => {
    expect(() =>
      parseCompileRequest({ source: 'x', optimizeLevel: 9 }),
    ).toThrow(/optimizeLevel/);
  });
});

// The end-to-end compile path drives the real native `cel` CLI through
// `@cel-wasm/compiler`.  When the CLI is not built (a fresh checkout that
// has not run `bazel build //tools/cel:cel`), `runCompile` rethrows a
// "cel CLI not found" error; these cases skip in that case rather than
// fail, mirroring the compiler binding's own CLI-absent discipline.
async function cliAvailable(): Promise<boolean> {
  try {
    await runCompile({ source: '1 + 2' });
    return true;
  } catch (err) {
    if (err instanceof Error && err.message.includes('cel CLI not found')) {
      return false;
    }
    return true;
  }
}

describe('runCompile (native CLI)', () => {
  it('compiles a valid expression to a wasm Program', async () => {
    if (!(await cliAvailable())) {
      return;
    }
    const response = await runCompile({ source: '1 + 2' });
    expect(response.ok).toBe(true);
    if (response.ok) {
      expect(response.byteLength).toBeGreaterThan(0);
      const bytes = Buffer.from(response.wasmBase64, 'base64');
      // The wasm magic number — proof this is a real module.
      expect(Array.from(bytes.subarray(0, 4))).toEqual([
        0x00, 0x61, 0x73, 0x6d,
      ]);
    }
  });

  it('returns diagnostics for a parse error', async () => {
    if (!(await cliAvailable())) {
      return;
    }
    const response = await runCompile({ source: '1 +' });
    expect(response.ok).toBe(false);
    if (!response.ok) {
      expect(response.error.length).toBeGreaterThan(0);
      expect(response.diagnostics.length).toBeGreaterThan(0);
    }
  });
});

import { CelCompileError } from '@cel-wasm/compiler/wasm-backend';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { CompileClient } from './compile-client.js';

// The client fetches `compiler.wasm`, instantiates a `WasmCompileBackend`,
// and decodes the returned bytes' `cel.abi` via `@cel-wasm/eval`.  These
// unit tests stub both heavy collaborators so they exercise the transport
// (lazy fetch + caching + load hooks) and error shaping in isolation; the
// genuine compiler.wasm → decode → eval round-trip is pinned end-to-end in
// `run.test.ts` against the committed `compiler.wasm`.

const { compileMock, createMock } = vi.hoisted(() => {
  const compile = vi.fn();
  return {
    compileMock: compile,
    createMock: vi.fn(() => Promise.resolve({ compile })),
  };
});

vi.mock('@cel-wasm/compiler/wasm-backend', async () => {
  const actual = await vi.importActual<
    typeof import('@cel-wasm/compiler/wasm-backend')
  >('@cel-wasm/compiler/wasm-backend');
  return {
    ...actual,
    WasmCompileBackend: { create: createMock },
  };
});

vi.mock('@cel-wasm/eval', () => ({
  decodeAbi: (): unknown => ({
    version: 1,
    variables: [],
    fields: [],
    types: [],
    runtimeAbiVersion: 1,
    linkMode: 1,
  }),
}));

function stubFetch(ok: boolean, bytes: Uint8Array, status = 200): void {
  vi.stubGlobal(
    'fetch',
    vi.fn(
      (): Promise<Response> =>
        Promise.resolve({
          ok,
          status,
          arrayBuffer: (): Promise<ArrayBuffer> =>
            Promise.resolve(bytes.buffer),
        } as Response),
    ),
  );
}

const WASM_MAGIC = new Uint8Array([0, 97, 115, 109]);

describe('CompileClient', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    compileMock.mockReset();
    createMock.mockClear();
  });

  it('lazy-fetches compiler.wasm and returns a Program with decoded abi', async () => {
    stubFetch(true, WASM_MAGIC);
    compileMock.mockResolvedValue(WASM_MAGIC);
    const client = new CompileClient();
    expect(client.ready).toBe(false);

    const program = await client.compile('1 + 2', []);

    expect(Array.from(program.wasm)).toEqual([0, 97, 115, 109]);
    expect(program.abi.version).toBe(1);
    expect(client.ready).toBe(true);
  });

  it('fires the load hooks exactly once, even across two compiles', async () => {
    stubFetch(true, WASM_MAGIC);
    compileMock.mockResolvedValue(WASM_MAGIC);
    const onLoadStart = vi.fn();
    const onLoadEnd = vi.fn();
    const client = new CompileClient({ onLoadStart, onLoadEnd });

    await client.compile('1', []);
    await client.compile('2', []);

    expect(onLoadStart).toHaveBeenCalledTimes(1);
    expect(onLoadEnd).toHaveBeenCalledTimes(1);
    expect(createMock).toHaveBeenCalledTimes(1);
  });

  it('passes source and vars through to the backend in dynamic link mode', async () => {
    stubFetch(true, WASM_MAGIC);
    compileMock.mockResolvedValue(WASM_MAGIC);
    const client = new CompileClient();

    await client.compile('age > 18', [{ name: 'age', type: 'int' }]);

    // The demo compiles dynamically so a Program is a thin ~6 KB expr
    // module linked against the shared runtime pulled into the page.
    expect(compileMock).toHaveBeenCalledWith({
      source: 'age > 18',
      vars: [{ name: 'age', type: 'int' }],
      linkMode: 'dynamic',
    });
  });

  it('propagates CelCompileError from the backend', async () => {
    stubFetch(true, WASM_MAGIC);
    compileMock.mockRejectedValue(
      new CelCompileError([{ message: 'Invalid CEL expression' }]),
    );
    const client = new CompileClient();

    await expect(client.compile('1 +', [])).rejects.toBeInstanceOf(
      CelCompileError,
    );
  });

  it('throws on a failed wasm fetch and allows a retry', async () => {
    stubFetch(false, new Uint8Array(0), 404);
    const client = new CompileClient();

    await expect(client.compile('1', [])).rejects.toThrow(/HTTP 404/);
    expect(client.ready).toBe(false);

    // A subsequent successful fetch must not be blocked by the cached
    // in-flight load promise from the failed attempt.
    stubFetch(true, WASM_MAGIC);
    compileMock.mockResolvedValue(WASM_MAGIC);
    const program = await client.compile('1', []);
    expect(Array.from(program.wasm)).toEqual([0, 97, 115, 109]);
  });
});

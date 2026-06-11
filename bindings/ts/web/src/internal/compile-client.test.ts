import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  CompileClientError,
  base64ToBytes,
  compileViaEndpoint,
} from './compile-client.js';

// The client decodes the `cel.abi` from the returned wasm bytes via
// `@cel-wasm/eval`'s `decodeAbi`.  These unit tests exercise the transport
// + error shaping with a stub wasm, so the real decoder is mocked; the
// genuine compile → decode → eval round-trip is pinned in `run.test.ts`.
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

describe('base64ToBytes', () => {
  it('decodes base64 to raw bytes', () => {
    // "wasm" magic 0x00 0x61 0x73 0x6d → base64 "AGFzbQ==".
    expect(Array.from(base64ToBytes('AGFzbQ=='))).toEqual([0, 97, 115, 109]);
  });

  it('decodes the empty string to an empty array', () => {
    expect(base64ToBytes('')).toEqual(new Uint8Array(0));
  });
});

describe('compileViaEndpoint', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  function stubFetch(status: number, body: unknown): void {
    vi.stubGlobal(
      'fetch',
      vi.fn(
        (): Promise<Response> =>
          Promise.resolve({
            status,
            json: (): Promise<unknown> => Promise.resolve(body),
          } as Response),
      ),
    );
  }

  it('returns a Program on a success response', async () => {
    stubFetch(200, {
      ok: true,
      wasmBase64: 'AGFzbQ==',
      byteLength: 4,
      abi: {
        version: 1,
        variables: [],
        fields: [],
        types: [],
        runtimeAbiVersion: 1,
        linkMode: 1,
      },
    });
    const program = await compileViaEndpoint('1 + 2', []);
    expect(Array.from(program.wasm)).toEqual([0, 97, 115, 109]);
    expect(program.abi.version).toBe(1);
  });

  it('throws CompileClientError with diagnostics on a failure response', async () => {
    stubFetch(200, {
      ok: false,
      error: '1:4: missing operand',
      diagnostics: [{ message: 'missing operand', line: 1, column: 4 }],
    });
    await expect(compileViaEndpoint('1 +', [])).rejects.toBeInstanceOf(
      CompileClientError,
    );
    await compileViaEndpoint('1 +', []).catch((err: unknown) => {
      expect(err).toBeInstanceOf(CompileClientError);
      if (err instanceof CompileClientError) {
        expect(err.diagnostics[0]?.line).toBe(1);
        expect(err.diagnostics[0]?.column).toBe(4);
      }
    });
  });

  it('throws on a non-JSON-object response', async () => {
    stubFetch(500, 'oops');
    await expect(compileViaEndpoint('x', [])).rejects.toBeInstanceOf(
      CompileClientError,
    );
  });

  it('sends source and vars in the request body', async () => {
    const fetchMock = vi.fn(
      (_input: string, _init?: RequestInit): Promise<Response> =>
        Promise.resolve({
          status: 200,
          json: (): Promise<unknown> =>
            Promise.resolve({ ok: true, wasmBase64: '', byteLength: 0 }),
        } as Response),
    );
    vi.stubGlobal('fetch', fetchMock);
    await compileViaEndpoint('age > 18', [{ name: 'age', type: 'int' }]);
    const call = fetchMock.mock.calls[0];
    expect(call?.[0]).toBe('/api/compile');
    const rawBody = call?.[1]?.body;
    if (typeof rawBody !== 'string') {
      throw new Error('expected a string request body');
    }
    const body = JSON.parse(rawBody) as {
      source: string;
      vars: { name: string }[];
    };
    expect(body.source).toBe('age > 18');
    expect(body.vars[0]?.name).toBe('age');
  });
});

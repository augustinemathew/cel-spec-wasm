// The browser-side compile client: POST the CEL source to the dev-server
// `/api/compile` endpoint and decode its response into a runnable
// {@link Program}.
//
// This is the demo's one network hop, and it exists only because the
// browser cannot subprocess the native `cel` CLI.  Eval never round-trips
// — once this returns a Program, the run path is pure client-side TS.
// When the emscripten `compiler.wasm` backend lands, this module is the
// single seam that swaps to a client-side compile.

import { decodeAbi, type Program } from '@cel-wasm/eval';

/** A declared free variable for the compile request. */
export interface CompileVar {
  readonly name: string;
  readonly type: string;
}

/** The compile-endpoint route the dev-server middleware mounts. */
const COMPILE_ROUTE = '/api/compile';

/** A diagnostic parsed from the compiler (line/column are 1-based). */
export interface CompileDiagnostic {
  readonly message: string;
  readonly line?: number;
  readonly column?: number;
}

/** Thrown when the compile endpoint reports a parse / type-check failure. */
export class CompileClientError extends Error {
  override readonly name = 'CompileClientError';
  readonly diagnostics: readonly CompileDiagnostic[];

  constructor(message: string, diagnostics: readonly CompileDiagnostic[]) {
    super(message);
    this.diagnostics = diagnostics;
  }
}

interface SuccessResponse {
  readonly ok: true;
  readonly wasmBase64: string;
  readonly byteLength: number;
}

interface FailureResponse {
  readonly ok: false;
  readonly error: string;
  readonly diagnostics: readonly CompileDiagnostic[];
}

/**
 * Compile `source` (with optional declared `vars`) via the dev-server
 * endpoint, returning the portable {@link Program}.
 *
 * @throws {CompileClientError} on a compile failure, carrying the parsed
 *   diagnostics so the caller can mark them inline in Monaco.
 */
export async function compileViaEndpoint(
  source: string,
  vars: readonly CompileVar[],
): Promise<Program> {
  const response = await fetch(COMPILE_ROUTE, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ source, vars }),
  });
  const body: unknown = await response.json();
  const parsed = parseResponse(body, response.status);
  if (!parsed.ok) {
    throw new CompileClientError(parsed.error, parsed.diagnostics);
  }
  // The endpoint returns only the portable wasm bytes; the `cel.abi`
  // descriptor is decoded from them here, client-side — the same decode
  // the eval binding does at plan time.  This keeps protobufjs off the
  // Node dev-server path (the browser bundles it for the run path anyway).
  const wasm = base64ToBytes(parsed.wasmBase64);
  return { wasm, abi: decodeAbi(wasm) };
}

function parseResponse(
  body: unknown,
  status: number,
): SuccessResponse | FailureResponse {
  if (typeof body !== 'object' || body === null) {
    return {
      ok: false,
      error: `compile endpoint returned a non-JSON response (HTTP ${String(status)})`,
      diagnostics: [],
    };
  }
  const record = body as Record<string, unknown>;
  if (record.ok === true) {
    return record as unknown as SuccessResponse;
  }
  const error =
    typeof record.error === 'string'
      ? record.error
      : `compile failed (HTTP ${String(status)})`;
  const diagnostics = Array.isArray(record.diagnostics)
    ? (record.diagnostics as CompileDiagnostic[])
    : [];
  return { ok: false, error, diagnostics };
}

/** Decode a base64 string to raw bytes using the browser's `atob`. */
export function base64ToBytes(base64: string): Uint8Array {
  const binary = atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i += 1) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes;
}

// The Node-side compile endpoint, decoupled from the Vite plugin glue so
// it can be unit-tested directly.
//
// The browser cannot subprocess the native `cel` CLI, so the demo's
// **compile** step is a tiny dev-server endpoint: it runs the CLI in Node
// and returns the portable wasm bytes (base64-encoded for JSON transport).
//
// This handler deliberately drives the `cel` CLI directly rather than
// going through `@cel-wasm/compiler`'s `compile()`.  `compile()` also
// decodes the `cel.abi` (pulling in `@cel-wasm/eval` → protobufjs, a
// CommonJS module whose named exports do not resolve cleanly under a raw
// Node / Vite-SSR ESM loader).  The browser already re-decodes the ABI
// from the wasm bytes client-side (`@cel-wasm/eval`'s `decodeAbi`, which
// Vite transforms for the browser), so the Node side never needs
// protobufjs — keeping this endpoint dependency-light and robust.

import { execFile } from 'node:child_process';
import {
  accessSync,
  constants as fsConstants,
  mkdtempSync,
  readFileSync,
  rmSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);

/** A declared free variable. */
export interface VariableDecl {
  readonly name: string;
  readonly type: string;
}

/** A diagnostic parsed from the compiler (line/column are 1-based). */
export interface Diagnostic {
  readonly message: string;
  readonly line?: number;
  readonly column?: number;
}

/** A `POST /api/compile` request body. */
export interface CompileRequestBody {
  readonly source: string;
  readonly vars?: readonly VariableDecl[];
  readonly optimizeLevel?: 0 | 1 | 2 | 3;
}

/** A successful compile response — the portable artifact (base64). */
export interface CompileSuccess {
  readonly ok: true;
  /** The portable `.wasm` Program, base64-encoded for JSON transport. */
  readonly wasmBase64: string;
  /** Size of the wasm artifact in bytes. */
  readonly byteLength: number;
}

/** A compile failure — parsed diagnostics for inline display in Monaco. */
export interface CompileFailure {
  readonly ok: false;
  /** A one-line human summary. */
  readonly error: string;
  /** Parsed diagnostics (line/column/message where the compiler located one). */
  readonly diagnostics: readonly Diagnostic[];
}

/** The discriminated compile-endpoint response. */
export type CompileResponse = CompileSuccess | CompileFailure;

/**
 * Validate and normalize an untrusted JSON body into a
 * {@link CompileRequestBody}.  Throws a `TypeError` describing the first
 * problem when the body is not a well-formed compile request.
 */
export function parseCompileRequest(body: unknown): CompileRequestBody {
  if (typeof body !== 'object' || body === null) {
    throw new TypeError('request body must be a JSON object');
  }
  const record = body as Record<string, unknown>;
  const { source } = record;
  if (typeof source !== 'string') {
    throw new TypeError('`source` must be a string');
  }
  const vars = parseVars(record.vars);
  const optimizeLevel = parseOptimizeLevel(record.optimizeLevel);
  return {
    source,
    ...(vars !== undefined ? { vars } : {}),
    ...(optimizeLevel !== undefined ? { optimizeLevel } : {}),
  };
}

function parseVars(raw: unknown): readonly VariableDecl[] | undefined {
  if (raw === undefined) {
    return undefined;
  }
  if (!Array.isArray(raw)) {
    throw new TypeError('`vars` must be an array');
  }
  return raw.map((entry, index): VariableDecl => {
    if (typeof entry !== 'object' || entry === null) {
      throw new TypeError(`vars[${String(index)}] must be an object`);
    }
    const { name, type } = entry as Record<string, unknown>;
    if (typeof name !== 'string' || typeof type !== 'string') {
      throw new TypeError(
        `vars[${String(index)}] must have string \`name\` and \`type\``,
      );
    }
    return { name, type };
  });
}

function parseOptimizeLevel(raw: unknown): 0 | 1 | 2 | 3 | undefined {
  if (raw === undefined) {
    return undefined;
  }
  if (raw === 0 || raw === 1 || raw === 2 || raw === 3) {
    return raw;
  }
  throw new TypeError('`optimizeLevel` must be 0, 1, 2, or 3');
}

// Path from this module (web/dev-server/) up to the repo root, where
// `bazel-bin/tools/cel/cel` lives: ../../../../.
const REPO_ROOT_RELATIVE = '../../../../';
const CLI_RELATIVE_PATH = join('bazel-bin', 'tools', 'cel', 'cel');
const CEL_CLI_ENV = 'CEL_CLI';

/**
 * Resolve the path to the `cel` CLI binary.  Honours a `CEL_CLI`
 * environment override; otherwise walks up from this module to the repo
 * root.  Returns `undefined` when the binary is absent.
 */
export function resolveCelCli(): string | undefined {
  const override = process.env[CEL_CLI_ENV];
  if (override !== undefined && override.length > 0) {
    return isExecutable(override) ? override : undefined;
  }
  const here = dirname(fileURLToPath(import.meta.url));
  const candidate = join(here, REPO_ROOT_RELATIVE, CLI_RELATIVE_PATH);
  return isExecutable(candidate) ? candidate : undefined;
}

function isExecutable(path: string): boolean {
  try {
    accessSync(path, fsConstants.X_OK);
    return true;
  } catch {
    return false;
  }
}

/** Build the `cel compile` argv for `request`, writing wasm to `outputPath`. */
export function buildCompileArgs(
  request: CompileRequestBody,
  outputPath: string,
): string[] {
  // The source is parenthesised so a leading `-` is not parsed as a flag.
  const args = ['compile', `(${request.source})`];
  for (const v of request.vars ?? []) {
    args.push('--var', `${v.name}:${v.type}`);
  }
  if (request.optimizeLevel !== undefined) {
    args.push('--O', String(request.optimizeLevel));
  }
  args.push('--output', outputPath);
  return args;
}

/**
 * Run the compile and shape it into a JSON-serializable
 * {@link CompileResponse}.  A non-zero CLI exit (a parse / type-check
 * failure) becomes a {@link CompileFailure} with parsed diagnostics; a
 * missing CLI is rethrown so the endpoint can return a distinguishable
 * 500.
 */
export async function runCompile(
  request: CompileRequestBody,
): Promise<CompileResponse> {
  const cliPath = resolveCelCli();
  if (cliPath === undefined) {
    throw new Error(
      'cel CLI not found — build it with `bazel build //tools/cel:cel` ' +
        `or set ${CEL_CLI_ENV}`,
    );
  }
  const dir = mkdtempSync(join(tmpdir(), 'cel-web-compile-'));
  const outputPath = join(dir, 'out.wasm');
  try {
    try {
      await execFileAsync(cliPath, buildCompileArgs(request, outputPath));
    } catch (err) {
      return { ok: false, ...toFailure(err) };
    }
    const wasm = readFileSync(outputPath);
    return {
      ok: true,
      wasmBase64: wasm.toString('base64'),
      byteLength: wasm.byteLength,
    };
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

/** Shape of a Node `execFile` rejection (the subset read here). */
interface ExecFileError {
  readonly stderr?: string;
  readonly stdout?: string;
  readonly message?: string;
}

function toFailure(err: unknown): { error: string; diagnostics: Diagnostic[] } {
  const e = err as ExecFileError;
  const text =
    (e.stderr ?? '').length > 0
      ? (e.stderr ?? '')
      : (e.stdout ?? e.message ?? 'compilation failed');
  const diagnostics = parseDiagnostics(text);
  const first = diagnostics[0];
  const error =
    first !== undefined
      ? first.line !== undefined && first.column !== undefined
        ? `${String(first.line)}:${String(first.column)}: ${first.message}`
        : first.message
      : 'compilation failed';
  return { error, diagnostics };
}

// The compiler prints diagnostics on lines beginning with `ERROR:`; a
// syntax error can carry a doubled prefix (`ERROR: ERROR: …`).  A located
// diagnostic carries a `<file>:<line>:<col>: <message>` prefix after the
// `ERROR:` run.
const ERROR_PREFIX = /^(?:ERROR:\s*)+/;
const LOCATED_DIAGNOSTIC =
  /^(?<file>[^:\n]*):(?<line>\d+):(?<column>\d+):\s*(?<message>.*)$/;

/**
 * Parse the CLI's stderr into a list of {@link Diagnostic}s.  Every
 * `ERROR:`-prefixed line becomes one diagnostic; located lines keep their
 * 1-based line/column.  Falls back to the whole trimmed stderr as a single
 * location-less diagnostic so the error is never empty.
 */
export function parseDiagnostics(stderr: string): Diagnostic[] {
  const diagnostics: Diagnostic[] = [];
  for (const rawLine of stderr.split('\n')) {
    const line = rawLine.trim();
    if (line.length === 0 || !ERROR_PREFIX.test(line)) {
      continue;
    }
    const body = line.replace(ERROR_PREFIX, '');
    const located = LOCATED_DIAGNOSTIC.exec(body);
    if (located?.groups) {
      diagnostics.push({
        message: located.groups.message?.trim() ?? '',
        line: Number(located.groups.line),
        column: Number(located.groups.column),
      });
      continue;
    }
    const message = body.trim();
    if (message.length > 0 && !message.endsWith(':')) {
      diagnostics.push({ message });
    }
  }
  if (diagnostics.length === 0) {
    const fallback = stderr.trim();
    diagnostics.push({
      message: fallback.length > 0 ? fallback : 'compilation failed',
    });
  }
  return diagnostics;
}

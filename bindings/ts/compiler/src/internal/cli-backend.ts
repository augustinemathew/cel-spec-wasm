// The v1 compile backend: shell out to the already-built native `cel`
// CLI (`bazel-bin/tools/cel/cel`) and read back the wasm it writes.
//
// The CLI links cel-cpp + Binaryen, so cross-compiling it to wasm or
// binding it via N-API is deferred; structuring the backend behind the
// {@link CompileBackend} interface lets those slot in later without
// touching the public `compile()` API.

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

import { CelCompileError, parseDiagnostics } from '../errors.js';

const execFileAsync = promisify(execFile);

/**
 * How the compiled Program links against the CEL runtime.
 *
 *  - `static` — a self-contained ~1.3 MB module with the runtime baked
 *    in (imports only host trampolines + WASI). Runs anywhere with no
 *    companion module.
 *  - `dynamic` — a ~6 KB thin expression module that imports the runtime
 *    surface (`cel.*`); the evaluator instantiates it against a shared
 *    `cel_runtime.wasm`. Smaller artifacts, one runtime shared across
 *    many Programs.
 */
export type LinkMode = 'static' | 'dynamic';

/** A request to compile one expression (already-resolved CLI inputs). */
export interface CompileRequest {
  readonly source: string;
  readonly vars: readonly { readonly name: string; readonly type: string }[];
  /**
   * Host-function declarations (`@host` signatures), each a `.celfn`
   * source string the compiler parses into a function declaration.
   */
  readonly fns?: readonly string[];
  readonly container?: string;
  readonly optimizeLevel?: 0 | 1 | 2 | 3;
  /** Static (self-contained) vs dynamic (runtime-linked) Program. */
  readonly linkMode?: LinkMode;
  /**
   * Absolute path to a serialized `FileDescriptorSet` (the bytes
   * `protoc --descriptor_set_out` emits).  Passed to the CLI's
   * `--descriptor_set` so a proto expression's message types type-check.
   * The path form is the native/CLI backend's input.
   */
  readonly descriptorSet?: string;
  /**
   * A serialized `FileDescriptorSet` supplied **in memory** (the same bytes
   * `protoc --descriptor_set_out` emits).  This is the browser/wasm form —
   * there is no filesystem, so the bytes are marshalled through the compiler
   * wasm directly (a `'d'` record) rather than via a path.  The wasm backend
   * builds a descriptor pool (layered over the generated pool) from them.
   */
  readonly descriptorSetBytes?: Uint8Array;
}

/**
 * A swappable compile engine.  The v1 implementation is the native-CLI
 * subprocess below; an N-API or emscripten backend can implement the
 * same contract later.
 */
export interface CompileBackend {
  /** Compile `request` to portable wasm bytes, or throw on failure. */
  compile(request: CompileRequest): Promise<Uint8Array>;
}

// Path from this module (compiler/src/internal/) up to the repo root,
// where `bazel-bin/tools/cel/cel` lives: ../../../../../.
const REPO_ROOT_RELATIVE = '../../../../../';
const CLI_RELATIVE_PATH = join('bazel-bin', 'tools', 'cel', 'cel');
const CEL_CLI_ENV = 'CEL_CLI';

/**
 * Resolve the path to the `cel` CLI binary.  Honours a `CEL_CLI`
 * environment override; otherwise walks up from this module to the repo
 * root and looks for `bazel-bin/tools/cel/cel`.  Returns `undefined`
 * when the binary is absent (so callers can `it.skip` rather than fail).
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

/**
 * Build the `cel compile` argv for `request`.  The source is wrapped in
 * parentheses so a leading `-` is not parsed as a flag, and `outputPath`
 * receives the emitted wasm.
 */
export function buildCompileArgs(
  request: CompileRequest,
  outputPath: string,
): string[] {
  const args = ['compile', `(${request.source})`];
  for (const v of request.vars) {
    args.push('--var', `${v.name}:${v.type}`);
  }
  if (request.container !== undefined) {
    args.push('--container', request.container);
  }
  if (request.descriptorSet !== undefined) {
    args.push('--descriptor_set', request.descriptorSet);
  }
  if (request.optimizeLevel !== undefined) {
    args.push('--O', String(request.optimizeLevel));
  }
  // Pass --link_mode explicitly (default static) so this backend's output is
  // independent of the `cel` CLI's own default, which is `dynamic`.
  args.push('--link_mode', request.linkMode ?? 'static');
  args.push('--output', outputPath);
  return args;
}

/** A `CompileBackend` that drives the native `cel` CLI as a subprocess. */
export class CliBackend implements CompileBackend {
  readonly #cliPath: string;

  /**
   * @param cliPath path to the `cel` binary; defaults to
   *   {@link resolveCelCli}.  Throws if no binary can be located.
   */
  constructor(cliPath?: string) {
    const resolved =
      cliPath !== undefined
        ? isExecutable(cliPath)
          ? cliPath
          : undefined
        : resolveCelCli();
    if (resolved === undefined) {
      throw new Error(
        `cel CLI not found — build it with ` +
          `\`bazel build //tools/cel:cel\` or set ${CEL_CLI_ENV}`,
      );
    }
    this.#cliPath = resolved;
  }

  async compile(request: CompileRequest): Promise<Uint8Array> {
    const dir = mkdtempSync(join(tmpdir(), 'cel-compile-'));
    const outputPath = join(dir, 'out.wasm');
    try {
      const args = buildCompileArgs(request, outputPath);
      try {
        await execFileAsync(this.#cliPath, args);
      } catch (err) {
        throw toCompileError(err);
      }
      return new Uint8Array(readFileSync(outputPath));
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  }
}

/** Shape of a Node `execFile` rejection (a subset we read). */
interface ExecFileError {
  readonly stderr?: string;
  readonly stdout?: string;
  readonly message?: string;
}

/**
 * Convert an `execFile` rejection into a {@link CelCompileError} by
 * parsing the CLI's stderr.  A non-zero exit is a compile diagnostic;
 * we never leak the raw Node error shape to the caller.
 */
function toCompileError(err: unknown): CelCompileError {
  const e = err as ExecFileError;
  const stderr = e.stderr ?? '';
  const text =
    stderr.length > 0
      ? stderr
      : (e.stdout ?? e.message ?? 'compilation failed');
  return new CelCompileError(parseDiagnostics(text));
}

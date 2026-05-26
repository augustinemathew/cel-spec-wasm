/**
 * `Program` — the compiled artifact, the TS counterpart to
 * `compiler_v2/api/program.h` (`cel::Program`). Pure data: the expr
 * module's wasm bytes plus its decoded `cel.abi`. There is **no TS
 * compiler** — a `Program` is produced by the C++ `cel::Compiler` /
 * `cel compile` and shipped here as bytes (the §2 split boundary).
 *
 * `fromBytes` is the portable core (works in Node + browser).
 * Environment-specific loaders (`fromFile` via `node:fs`, `fromUrl` via
 * `fetch`) live in the Node / browser entry points, not in this pure
 * module, so importing `Program` never drags in `node:fs`.
 */
import { decodeCelAbi, type CelAbi } from './abi.js';

export class Program {
  private constructor(
    /** The expr module's wasm bytes (expr code + `cel.abi` section). */
    public readonly wasmBytes: Uint8Array,
    /** Decoded `cel.abi`, or `null` if the module carries no section
     *  (a variable-free Eval still works). */
    public readonly abi: CelAbi | null,
  ) {}

  /**
   * Construct from raw wasm bytes, decoding the `cel.abi` section. Throws
   * `AbiDecodeError` if the section is present but malformed.
   *
   * @example
   * ```ts
   * import { readFileSync } from 'node:fs';
   * import { Program } from '@celwasm/eval';
   *
   * const program = Program.fromBytes(readFileSync('expr.wasm'));
   * const instance = await engine.plan(program);
   * ```
   */
  public static fromBytes(wasmBytes: Uint8Array): Program {
    return new Program(wasmBytes, decodeCelAbi(wasmBytes));
  }
}

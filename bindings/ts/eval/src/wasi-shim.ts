/**
 * Minimal `wasi_snapshot_preview1` shim for instantiating
 * `cel_runtime.wasm` — the TS counterpart to wasmtime's WASI on the C++
 * side (`engine.cc::RegisterWasiStubs`).
 *
 * P-3 / P-4 findings: the runtime *declares* 14 preview1 imports but for
 * a scalar eval calls exactly ONE — `random_get` (from
 * `__wasilibc_init_ssp`, seeding the stack canary during ctor init). So
 * `random_get` is a real implementation; the other 13 must be *defined*
 * (or instantiation fails with "unknown import") but throw loudly if
 * ever actually called — surfacing a not-yet-supported WASI dependency
 * rather than silently returning a wrong errno. `proc_exit` aborts, so
 * it also throws (mapping the guest abort to a host exception).
 */

/** Thrown when the guest reaches an unsupported WASI call (or aborts). */
export class WasiError extends Error {
  public override readonly name = 'WasiError';
}

/** A WASI preview1 import function: i32 args → i32 errno (0 = success). */
export type WasiFn = (...args: number[]) => number;

/** The exact preview1 import set `cel_runtime.wasm` declares (P-3). A
 *  concrete interface (not a string index) so each member is statically
 *  known — wiring into a linker can't typo a name. */
export interface WasiPreview1Imports {
  readonly random_get: (ptr: number, len: number) => number;
  readonly proc_exit: (code: number) => number;
  readonly environ_get: WasiFn;
  readonly environ_sizes_get: WasiFn;
  /** `clock_time_get(clock_id, precision: i64, result_ptr)` — writes the
   *  current time (u64 nanoseconds) at `result_ptr`. `precision` is an
   *  i64, so it arrives as a `bigint`. */
  readonly clock_time_get: (
    clockId: number,
    precision: bigint,
    resultPtr: number,
  ) => number;
  readonly fd_close: WasiFn;
  readonly fd_fdstat_get: WasiFn;
  readonly fd_prestat_get: WasiFn;
  readonly fd_prestat_dir_name: WasiFn;
  readonly fd_read: WasiFn;
  readonly fd_seek: WasiFn;
  readonly fd_write: WasiFn;
  readonly poll_oneoff: WasiFn;
  readonly sched_yield: WasiFn;
}

const ERRNO_SUCCESS = 0;

/**
 * Build the `wasi_snapshot_preview1` import object. `memory` is a lazy
 * accessor for the instance's linear memory (the runtime owns + exports
 * it, so it isn't available until after instantiation, but `random_get`
 * only fires during the first exported call — see P-4).
 */
export function createWasiPreview1(
  memory: () => Uint8Array,
): WasiPreview1Imports {
  function notImplemented(name: string): WasiFn {
    return (): number => {
      throw new WasiError(`wasi_snapshot_preview1.${name}: unexpected call`);
    };
  }

  return {
    // The one import actually reached during eval: fill `len` random
    // bytes at `ptr`, return success.
    random_get: (ptr: number, len: number): number => {
      crypto.getRandomValues(memory().subarray(ptr, ptr + len));
      return ERRNO_SUCCESS;
    },
    // A guest abort — map to a host exception (never returns).
    proc_exit: (code: number): number => {
      throw new WasiError(`wasi_snapshot_preview1.proc_exit(${code})`);
    },
    // Declared-but-not-reached for the scalar subset (P-3): defined so
    // instantiation succeeds, loud if a future row actually calls one.
    environ_get: notImplemented('environ_get'),
    environ_sizes_get: notImplemented('environ_sizes_get'),
    // Real wall-clock: write the current time (u64 nanoseconds since the
    // Unix epoch, little-endian) at `resultPtr`. This is what CEL `now()`
    // / timestamp ops observe. `Date.now()` is millisecond-resolution, so
    // the low 6 digits are zero — fine for CEL's microsecond semantics.
    clock_time_get: (
      _clockId: number,
      _precision: bigint,
      resultPtr: number,
    ): number => {
      const nowNs = BigInt(Date.now()) * 1_000_000n;
      const mem = memory();
      new DataView(mem.buffer, mem.byteOffset, mem.byteLength).setBigUint64(
        resultPtr,
        nowNs,
        true,
      );
      return ERRNO_SUCCESS;
    },
    fd_close: notImplemented('fd_close'),
    fd_fdstat_get: notImplemented('fd_fdstat_get'),
    fd_prestat_get: notImplemented('fd_prestat_get'),
    fd_prestat_dir_name: notImplemented('fd_prestat_dir_name'),
    fd_read: notImplemented('fd_read'),
    fd_seek: notImplemented('fd_seek'),
    fd_write: notImplemented('fd_write'),
    poll_oneoff: notImplemented('poll_oneoff'),
    sched_yield: notImplemented('sched_yield'),
  };
}

// Public surface of the cel-wasm conformance harness.
//
// Loads the upstream textproto corpus, compiles + evaluates each row,
// and compares the decoded CelValue to the expected value/error, with a
// monotonic baseline ratchet mirroring the C++ gate (§A.7).  The runner
// lands in WI-3.1; this stub fixes the result-shape types.

/** Per-row outcome classification (§A.7). */
export type ConformanceOutcome = 'pass' | 'skip' | 'fail';

/** Aggregate result of a conformance run. */
export interface ConformanceReport {
  readonly pass: number;
  readonly skip: number;
  readonly fail: number;
}

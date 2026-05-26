/**
 * conformance_ts gate — the vitest entry that runs the whole fixtures dir
 * through the real `@celwasm/eval` library and asserts zero-diff vs C++.
 * Driven by `scripts/check_conformance_ts.sh`, which regenerates fixtures
 * from the CURRENT C++ compiler+runner and points us at them via env:
 *   CEL_TS_CONF_FIXTURES — the exporter's out_dir (index.jsonl + <n>.wasm)
 *   CEL_RUNTIME_WASM      — the cel_runtime.wasm path
 * It is excluded from the hermetic unit suite (vitest.config.ts) since it
 * needs those artifacts; run it via the dedicated conformance config.
 */
import { describe, it, expect } from 'vitest';
import { runConformance } from '../src/run.js';

const FIXTURES = process.env.CEL_TS_CONF_FIXTURES;
const RUNTIME = process.env.CEL_RUNTIME_WASM;
// Optional: a FileDescriptorSet covering the corpus message types, so
// `cel_make_message` rows can construct real protobuf-es messages.
const DESCRIPTORS = process.env.CEL_TS_CONF_DESCRIPTORS;

describe('conformance_ts zero-diff vs C++', () => {
  it('every comparable row agrees with cpp_outcome', async () => {
    if (FIXTURES === undefined || RUNTIME === undefined) {
      throw new Error(
        'set CEL_TS_CONF_FIXTURES + CEL_RUNTIME_WASM ' +
          '(run via scripts/check_conformance_ts.sh)',
      );
    }
    const r = await runConformance(FIXTURES, RUNTIME, DESCRIPTORS);

    // Print the confusion matrix for visibility (matches the old runner).
    const lines = [`fixtures: ${r.total}`, 'confusion matrix (cpp -> ts):'];
    for (const k of Object.keys(r.matrix).sort()) {
      lines.push(`  ${k}: ${r.matrix[k] ?? 0}`);
    }
    lines.push(
      `comparable: agree=${r.agree} regressions=${r.regressions} ahead=${r.ahead}`,
    );
    // Account for the cpp=pass ts=skip coverage gap, by reason.
    const gap = Object.entries(r.skipGap).sort((a, b) => b[1] - a[1]);
    if (gap.length > 0) {
      const total = gap.reduce((s, [, n]) => s + n, 0);
      lines.push(`cpp=pass ts=skip coverage gap (${total}), by reason:`);
      for (const [reason, n] of gap) {
        lines.push(`  ${n}\t${reason}`);
      }
    }
    if (r.regressionExamples.length > 0) {
      lines.push('regressions:', ...r.regressionExamples.map((d) => `  ${d}`));
    }
    console.log(lines.join('\n'));

    // The gate: the C++ pass-set is the floor — TS may not REGRESS below it
    // (cpp=pass ts=fail). TS being AHEAD (spec-correct where C++ fails) is
    // reported, not failed.
    expect(r.regressions, r.regressionExamples.join('\n')).toBe(0);
    expect(r.agree).toBeGreaterThan(0);
  }, 180_000);
});

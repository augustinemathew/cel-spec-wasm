/**
 * Micro-benchmark: eval throughput of a long arithmetic expression —
 * `v0 + v1 + … + v30` (30 real runtime additions over 31 bound int vars,
 * `-O3` so binaryen can't constant-fold them) — through the real wasm host.
 * `engine.plan` runs ONCE; the timed loop is pure `instance.eval` (marshal
 * 31 bindings → $eval → decode), so it measures steady-state per-eval cost.
 * Reports ns/eval + evals/sec; the asserts sanity-check the result so the
 * bench can't silently measure a broken eval.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { asInt, Value } from '../../src/value.js';
import { Activation } from '../../src/activation.js';
import { Program } from '../../src/program.js';
import { setup, src, TESTDATA, type Harness } from './harness.js';

let h: Harness;
beforeAll(async () => {
  h = await setup();
});

describe('perf: eval of 30 additions', () => {
  it(`${src('adds30.wasm')} — steady-state eval throughput`, async () => {
    const engine = await h.bareEngine();
    const instance = await engine.plan(
      Program.fromBytes(readFileSync(join(TESTDATA, 'adds30.wasm'))),
    );
    // Bind v0..v30 = 0..30 → sum = 465.
    const act = new Activation();
    for (let i = 0; i <= 30; i++) {
      act.bind(`v${i}`, Value.int(BigInt(i)));
    }
    const SUM = 465n;

    expect(asInt(instance.eval(act))).toBe(SUM);

    // Warm up the JIT, then time a steady-state run.
    for (let i = 0; i < 10_000; i++) {
      instance.eval(act);
    }
    const iters = 50_000;
    const t0 = performance.now();
    for (let i = 0; i < iters; i++) {
      instance.eval(act);
    }
    const ms = performance.now() - t0;

    const nsPer = (ms * 1e6) / iters;
    const perSec = Math.round(iters / (ms / 1000));
    console.log(
      `adds30: ${iters.toLocaleString()} evals in ${ms.toFixed(1)} ms — ` +
        `${nsPer.toFixed(0)} ns/eval, ${perSec.toLocaleString()} evals/sec`,
    );

    expect(asInt(instance.eval(act))).toBe(SUM);
  });
});

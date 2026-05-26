import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['eval/test/**/*.test.ts', 'conformance/test/**/*.test.ts'],
    // Real-wasm e2e (vitest.integration.config.ts) and the conformance_ts
    // gate (vitest.conformance.config.ts) are their own non-hermetic suites
    // — they need cel_runtime.wasm / regenerated fixtures, so they're kept
    // out of `npm test`.
    exclude: [
      '**/node_modules/**',
      '**/integration/**',
      'conformance/test/gate.test.ts',
    ],
    coverage: {
      provider: 'v8',
      // Only the shipped library source is measured (not tests/probes).
      include: ['eval/src/**/*.ts', 'conformance/src/**/*.ts'],
      // Excluded from the unit-coverage gate:
      //   - gen/**     : protobuf-es generated code (not ours to test).
      //   - engine.ts  : the wasm-instantiation shell — needs the real
      //                  cel_runtime.wasm; covered by the conformance_ts
      //                  integration gate, not hermetic unit tests.
      //   - index.ts   : barrel re-exports, no logic.
      exclude: [
        '**/gen/**',
        'eval/src/engine.ts',
        'eval/src/index.ts',
        // Reflection over arbitrary protos: 100% needs a bespoke
        // all-field-kinds proto. Validated by a real protobuf-es unit
        // test (the common matrix) + the real-wasm e2e + conformance,
        // not the hermetic unit gate.
        'eval/src/host/proto-backing.ts',
        // The conformance runner drives the whole corpus through the real
        // library + runtime wasm; it's exercised by the conformance_ts
        // gate (regenerated fixtures), not the hermetic unit suite.
        'conformance/src/run.ts',
      ],
      reporter: ['text', 'html', 'lcov'],
      // `all: true` lists EVERY src file — an untested module shows up as
      // 0% instead of silently not counting, so coverage can't be gamed
      // by simply not importing a file.
      all: true,
      // This is a binary codec + wire host: a missed branch is a latent
      // miscompile. Hold the bar at 100% — drop a specific threshold only
      // with a written justification (a genuinely unreachable defensive
      // arm), never as a blanket lowering.
      thresholds: {
        lines: 100,
        functions: 100,
        statements: 100,
        branches: 100,
      },
    },
  },
});

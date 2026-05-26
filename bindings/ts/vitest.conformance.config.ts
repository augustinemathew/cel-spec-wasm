import { defineConfig } from 'vitest/config';

// conformance_ts gate — runs the full corpus through the real eval library
// and zero-diffs vs C++. NOT hermetic (needs regenerated fixtures +
// cel_runtime.wasm via env), so it's its own suite, kept out of `npm test`
// / coverage. Driven by scripts/check_conformance_ts.sh.
export default defineConfig({
  test: {
    include: ['conformance/test/gate.test.ts'],
    testTimeout: 180_000,
  },
});

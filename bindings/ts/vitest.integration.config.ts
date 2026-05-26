import { defineConfig } from 'vitest/config';

// Real-wasm e2e suite — instantiates the actual `cel_runtime.wasm` and
// evaluates compiled Programs through the production host modules. NOT
// hermetic (needs the runtime wasm built); kept out of `npm test` /
// coverage. Run with `npm run test:integration`; CI builds the runtime
// wasm first (or set CEL_RUNTIME_WASM).
export default defineConfig({
  test: {
    include: ['eval/test/integration/**/*.e2e.test.ts'],
  },
});

import { defineConfig } from 'vitest/config';

// Root vitest config — discovers `*.test.ts` across every workspace
// package.  Coverage uses v8; the gate is intentionally low for the
// scaffold (only stubs + the shared type module exist) and ratchets up
// as real implementation lands in the downstream work items.
export default defineConfig({
  test: {
    include: ['**/src/**/*.test.ts', '**/e2e/**/*.test.ts'],
    exclude: ['**/node_modules/**', '**/dist/**'],
    coverage: {
      provider: 'v8',
      reporter: ['text', 'html'],
      include: ['**/src/**/*.ts'],
      exclude: ['**/*.test.ts', '**/index.ts'],
    },
  },
});

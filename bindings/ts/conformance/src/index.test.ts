import { describe, expect, it } from 'vitest';

import type { ConformanceOutcome, ConformanceReport } from './index.js';

// Smoke test for the conformance package scaffold.  The corpus runner +
// ratchet land in WI-3.1; here we only assert the result-shape types are
// usable from a downstream consumer.
describe('@cel-wasm/conformance scaffold', () => {
  it('exposes the report shape', () => {
    const outcome: ConformanceOutcome = 'pass';
    const report: ConformanceReport = { pass: 1, skip: 0, fail: 0 };
    expect(outcome).toBe('pass');
    expect(report.pass).toBe(1);
  });
});

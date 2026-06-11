import { describe, expect, it } from 'vitest';

import {
  parseTextproto,
  summaryLine,
  type ConformanceReport,
  type ConformanceOutcome,
} from './index.js';

// Smoke test for the conformance package's public surface — the deep
// behaviour lives in the colocated module tests (textproto / corpus /
// classify / compare / baseline) and the pinned-fixture e2e
// (run.test.ts).
describe('@cel-wasm/conformance public surface', () => {
  it('exposes the report + outcome types', () => {
    const outcome: ConformanceOutcome = 'pass';
    const report: ConformanceReport = {
      total: 1,
      pass: 1,
      skip: 0,
      fail: 0,
      skipByCategory: new Map(),
      failures: [],
    };
    expect(outcome).toBe('pass');
    expect(summaryLine(report)).toContain('pass=1');
  });

  it('re-exports the textproto reader', () => {
    expect(parseTextproto('name: "x"').fields.has('name')).toBe(true);
  });
});

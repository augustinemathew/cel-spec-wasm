// Tests for the monotonic baseline ratchet — pass floor + fail ceiling,
// seeding on a missing file, and the --update rewrite.  Mirrors the gate
// logic of scripts/check_conformance_monotonic.sh.

import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import {
  checkBaseline,
  readCount,
  updateBaseline,
  type BaselinePaths,
} from './baseline.js';
import type { ConformanceReport } from './harness.js';

function report(pass: number, fail: number): ConformanceReport {
  return {
    total: pass + fail,
    pass,
    skip: 0,
    fail,
    skipByCategory: new Map(),
    failures: [],
  };
}

describe('checkBaseline', () => {
  let dir: string;
  let paths: BaselinePaths;

  beforeEach(() => {
    dir = mkdtempSync(join(tmpdir(), 'cel-baseline-'));
    paths = {
      baseline: join(dir, '.baseline'),
      maxFail: join(dir, '.max_fail'),
    };
  });

  afterEach(() => {
    rmSync(dir, { recursive: true, force: true });
  });

  it('seeds both files on a fresh run and passes', () => {
    const check = checkBaseline(report(100, 5), paths);
    expect(check.ok).toBe(true);
    expect(readCount(paths.baseline)).toBe(100);
    expect(readCount(paths.maxFail)).toBe(5);
  });

  it('passes when pass count is flat and fail count is flat', () => {
    writeFileSync(paths.baseline, '100\n');
    writeFileSync(paths.maxFail, '5\n');
    expect(checkBaseline(report(100, 5), paths).ok).toBe(true);
  });

  it('passes (and notes) when pass count rises', () => {
    writeFileSync(paths.baseline, '100\n');
    writeFileSync(paths.maxFail, '5\n');
    const check = checkBaseline(report(110, 5), paths);
    expect(check.ok).toBe(true);
    expect(check.messages.join('\n')).toContain('+10 PASS');
  });

  it('fails when pass count drops below the floor', () => {
    writeFileSync(paths.baseline, '100\n');
    writeFileSync(paths.maxFail, '5\n');
    const check = checkBaseline(report(99, 5), paths);
    expect(check.ok).toBe(false);
    expect(check.messages.join('\n')).toContain('REGRESSION');
  });

  it('fails when fail count rises above the ceiling (SKIP→FAIL at flat pass)', () => {
    writeFileSync(paths.baseline, '100\n');
    writeFileSync(paths.maxFail, '5\n');
    const check = checkBaseline(report(100, 6), paths);
    expect(check.ok).toBe(false);
    expect(check.messages.join('\n')).toContain('fail count rose');
  });

  it('passes (and notes) when fail count falls', () => {
    writeFileSync(paths.baseline, '100\n');
    writeFileSync(paths.maxFail, '5\n');
    const check = checkBaseline(report(100, 3), paths);
    expect(check.ok).toBe(true);
    expect(check.messages.join('\n')).toContain('fail count fell');
  });
});

describe('updateBaseline', () => {
  it('rewrites both files to the current counts', () => {
    const dir = mkdtempSync(join(tmpdir(), 'cel-baseline-'));
    const paths: BaselinePaths = {
      baseline: join(dir, '.baseline'),
      maxFail: join(dir, '.max_fail'),
    };
    try {
      writeFileSync(paths.baseline, '100\n');
      writeFileSync(paths.maxFail, '5\n');
      updateBaseline(report(120, 2), paths);
      expect(readFileSync(paths.baseline, 'utf-8').trim()).toBe('120');
      expect(readFileSync(paths.maxFail, 'utf-8').trim()).toBe('2');
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe('readCount', () => {
  it('returns undefined for a missing file', () => {
    expect(readCount(join(tmpdir(), 'does-not-exist-xyz'))).toBeUndefined();
  });
});

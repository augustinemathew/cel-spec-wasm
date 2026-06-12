// The monotonic baseline ratchet, mirroring the C++
// `scripts/check_conformance_monotonic.sh`: the PASS count must not fall
// below `.baseline`, and the FAIL count must not rise above `.max_fail`.
// A SKIP→FAIL conversion at flat pass count is caught by the FAIL ceiling.
//
// Both files are a single integer line.  `--update` rewrites them to the
// current run's counts (done at milestone closeout, not in the loop).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7;
//       scripts/check_conformance_monotonic.sh.

import { existsSync, readFileSync, writeFileSync } from 'node:fs';

import type { ConformanceReport } from './harness.js';

/** The outcome of checking a run against the baseline files. */
export interface BaselineCheck {
  readonly ok: boolean;
  /** Human-readable lines describing the gate result (and any regression). */
  readonly messages: readonly string[];
}

/** Paths to the two ratchet files. */
export interface BaselinePaths {
  readonly baseline: string;
  readonly maxFail: string;
}

/**
 * Gate `report` against the baseline + max-fail files.  A missing file is
 * created with the current count (a first run seeds the ratchet rather
 * than failing).  Returns `ok: false` when PASS dropped below the floor
 * or FAIL rose above the ceiling.
 */
export function checkBaseline(
  report: ConformanceReport,
  paths: BaselinePaths,
): BaselineCheck {
  const messages: string[] = [];
  let ok = true;

  const baseline = readCount(paths.baseline);
  if (baseline === undefined) {
    writeCount(paths.baseline, report.pass);
    messages.push(
      `seeded baseline = ${String(report.pass)} at ${paths.baseline}`,
    );
  } else {
    messages.push(
      `baseline PASS floor = ${String(baseline)}, current = ${String(report.pass)}`,
    );
    if (report.pass < baseline) {
      ok = false;
      messages.push(
        `REGRESSION: pass count dropped ${String(baseline)} → ${String(report.pass)}`,
      );
    } else if (report.pass > baseline) {
      messages.push(
        `ok: +${String(report.pass - baseline)} PASS above baseline (consider --update)`,
      );
    }
  }

  const maxFail = readCount(paths.maxFail);
  if (maxFail === undefined) {
    writeCount(paths.maxFail, report.fail);
    messages.push(
      `seeded max-fail = ${String(report.fail)} at ${paths.maxFail}`,
    );
  } else {
    messages.push(
      `max FAIL ceiling = ${String(maxFail)}, current = ${String(report.fail)}`,
    );
    if (report.fail > maxFail) {
      ok = false;
      messages.push(
        `REGRESSION: fail count rose ${String(maxFail)} → ${String(report.fail)}`,
      );
    } else if (report.fail < maxFail) {
      messages.push(
        `ok: fail count fell ${String(maxFail)} → ${String(report.fail)} (consider --update)`,
      );
    }
  }

  return { ok, messages };
}

/** Rewrite both ratchet files to the current run's counts (`--update`). */
export function updateBaseline(
  report: ConformanceReport,
  paths: BaselinePaths,
): readonly string[] {
  writeCount(paths.baseline, report.pass);
  writeCount(paths.maxFail, report.fail);
  return [
    `baseline updated to ${String(report.pass)} at ${paths.baseline}`,
    `max-fail updated to ${String(report.fail)} at ${paths.maxFail}`,
  ];
}

/** Read an integer count from a ratchet file, or `undefined` if absent. */
export function readCount(path: string): number | undefined {
  if (!existsSync(path)) {
    return undefined;
  }
  const text = readFileSync(path, 'utf-8').trim();
  if (text === '') {
    return undefined;
  }
  const n = Number.parseInt(text, 10);
  return Number.isNaN(n) ? undefined : n;
}

function writeCount(path: string, count: number): void {
  writeFileSync(path, `${String(count)}\n`, 'utf-8');
}

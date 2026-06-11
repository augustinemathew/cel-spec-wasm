// CLI entry for the conformance harness (the `npm run conformance`
// script).  Runs the full corpus, prints the pass/skip/fail summary + the
// per-category skip breakdown, gates against the monotonic baseline, and
// exits non-zero on a regression.  Mirrors
// `scripts/check_conformance_monotonic.sh`'s gate (PASS floor + FAIL
// ceiling).
//
// Usage:
//   node dist/run.js              # run + check the baseline
//   node dist/run.js --update     # rewrite the baseline to current counts
//   node dist/run.js --files basic,lists   # restrict to named files
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7.

import { fileURLToPath } from 'node:url';

import { checkBaseline, updateBaseline } from './baseline.js';
import {
  runCorpus,
  skipBreakdown,
  summaryLine,
  type ConformanceReport,
  type TaggedResult,
} from './harness.js';

// This module lives at conformance/src/ (dev) or conformance/dist/ (built);
// both are two levels under bindings/ts/conformance, four under the repo
// root.  The corpus + baseline files are repo-relative, so resolve from
// the conformance package dir, which is the same in both layouts.
const PACKAGE_DIR = fileURLToPath(new URL('../', import.meta.url));
const CORPUS_DIR = fileURLToPath(
  new URL('../../../../spec/tests/simple/testdata/', import.meta.url),
);
const BASELINE_PATH = `${PACKAGE_DIR}.baseline`;
const MAX_FAIL_PATH = `${PACKAGE_DIR}.max_fail`;

interface CliOptions {
  readonly update: boolean;
  readonly files: readonly string[] | undefined;
  readonly verbose: boolean;
}

function parseArgs(argv: readonly string[]): CliOptions {
  let update = false;
  let verbose = false;
  let files: readonly string[] | undefined;
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--update') {
      update = true;
    } else if (arg === '--verbose' || arg === '-v') {
      verbose = true;
    } else if (arg === '--files') {
      const next = argv[i + 1];
      if (next !== undefined) {
        files = next.split(',').filter((s) => s.length > 0);
        i += 1;
      }
    }
  }
  return { update, files, verbose };
}

function printFailures(report: ConformanceReport): void {
  if (report.failures.length === 0) {
    return;
  }
  process.stdout.write(`\n${String(report.failures.length)} FAIL row(s):\n`);
  for (const f of report.failures) {
    process.stdout.write(
      `  FAIL ${f.file}/${f.section}/${f.name}\n` +
        `       expr: ${f.expr}\n` +
        `       ${f.result.detail}\n`,
    );
  }
}

async function main(): Promise<number> {
  const opts = parseArgs(process.argv.slice(2));

  let done = 0;
  const onRow = (tagged: TaggedResult): void => {
    done += 1;
    if (opts.verbose && tagged.result.outcome === 'fail') {
      process.stderr.write(
        `FAIL ${tagged.file}/${tagged.name}: ${tagged.result.detail}\n`,
      );
    }
    if (done % 200 === 0) {
      process.stderr.write(`  ... ${String(done)} rows\n`);
    }
  };

  const report = await runCorpus({
    corpusDir: CORPUS_DIR,
    ...(opts.files !== undefined ? { files: opts.files } : {}),
    onRow,
  });

  process.stdout.write(`${summaryLine(report)}\n`);
  const breakdown = skipBreakdown(report);
  if (breakdown.length > 0) {
    process.stdout.write(`skip breakdown:\n${breakdown}\n`);
  }
  printFailures(report);

  const paths = { baseline: BASELINE_PATH, maxFail: MAX_FAIL_PATH };
  if (opts.update) {
    for (const line of updateBaseline(report, paths)) {
      process.stdout.write(`${line}\n`);
    }
    return 0;
  }

  const check = checkBaseline(report, paths);
  for (const line of check.messages) {
    process.stdout.write(`${line}\n`);
  }
  // A FAIL row is always a gate failure: every fail must be either fixed
  // or reclassified to a reasoned SKIP (the done-when contract).
  if (report.fail > 0) {
    process.stdout.write(
      `\ngate: ${String(report.fail)} unexplained FAIL row(s) — fix or reclassify as a SKIP category.\n`,
    );
    return 1;
  }
  return check.ok ? 0 : 1;
}

main()
  .then((code) => {
    process.exitCode = code;
  })
  .catch((err: unknown) => {
    process.stderr.write(`conformance run failed: ${String(err)}\n`);
    process.exitCode = 2;
  });

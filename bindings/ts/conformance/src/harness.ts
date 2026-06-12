// The corpus harness: load every `*.textproto` under the corpus
// directory, run each row through {@link runRow}, and aggregate the
// outcomes into a {@link ConformanceReport} with per-category skip counts
// and the full FAIL list (so a regression names the offending rows).
//
// Spec: doc/implementation-plan/rewrite/m29-typescript-bindings.md §A.7.

import { existsSync, readFileSync, readdirSync } from 'node:fs';

import { Engine } from '@cel-wasm/eval';
import { DescriptorSet } from '@cel-wasm/eval/proto';

import type { SkipCategory } from './classify.js';
import { loadSimpleTestFile, type SimpleTest } from './corpus.js';
import { runRow, type ProtoEnv, type RowResult } from './runner.js';
import { parseTextproto } from './textproto.js';

/** A row tagged with its outcome — used for the FAIL / per-row detail list. */
export interface TaggedResult {
  readonly file: string;
  readonly section: string;
  readonly name: string;
  readonly expr: string;
  readonly result: RowResult;
}

/** The aggregate result of a corpus run. */
export interface ConformanceReport {
  readonly total: number;
  readonly pass: number;
  readonly skip: number;
  readonly fail: number;
  /** Per-category skip counts. */
  readonly skipByCategory: ReadonlyMap<SkipCategory, number>;
  /** Every FAIL row, for diagnosis + the regression list. */
  readonly failures: readonly TaggedResult[];
}

/** Options for {@link runCorpus}. */
export interface RunCorpusOptions {
  /** Absolute path to the directory of `*.textproto` corpus files. */
  readonly corpusDir: string;
  /** Restrict the run to these file stems (e.g. `['basic']`); all if omitted. */
  readonly files?: readonly string[];
  /** Called after each row completes (for progress reporting). */
  readonly onRow?: (tagged: TaggedResult) => void;
  /**
   * Absolute path to a serialized `FileDescriptorSet` supplying the
   * conformance test message types.  When present, the harness reads the
   * bytes and proto rows compile (the compiler binding marshals those bytes
   * through the compiler wasm as a `'d'` record) and eval (via
   * `Engine.create({descriptors})`) instead of skipping.  When absent, proto
   * rows SKIP as `proto_unimpl`.
   */
  readonly descriptorSetPath?: string;
}

interface ProtoSetup {
  readonly env: ProtoEnv;
  /** The descriptor bytes for `Engine.create`, or `undefined` if none. */
  readonly descriptorBytes: Uint8Array | undefined;
}

/** Load the descriptor set (once) into both the {@link ProtoEnv} + Engine bytes. */
function loadProtoSetup(descriptorSetPath: string | undefined): ProtoSetup {
  if (descriptorSetPath === undefined || !existsSync(descriptorSetPath)) {
    return {
      env: { descriptors: undefined, descriptorSetBytes: undefined },
      descriptorBytes: undefined,
    };
  }
  const bytes = new Uint8Array(readFileSync(descriptorSetPath));
  return {
    env: {
      descriptors: DescriptorSet.fromFileDescriptorSet(bytes),
      // The compiler binding marshals the same bytes through the compiler
      // wasm (a 'd' record) to type-check proto types in-process.
      descriptorSetBytes: bytes,
    },
    descriptorBytes: bytes,
  };
}

/** Load every corpus row under `corpusDir` (optionally a subset of files). */
export function loadCorpus(
  corpusDir: string,
  files?: readonly string[],
): readonly SimpleTest[] {
  const dir = corpusDir.endsWith('/') ? corpusDir : `${corpusDir}/`;
  const wanted = files === undefined ? undefined : new Set(files);
  const rows: SimpleTest[] = [];
  for (const entry of readdirSync(dir)) {
    if (!entry.endsWith('.textproto')) {
      continue;
    }
    const stem = entry.slice(0, -'.textproto'.length);
    if (wanted !== undefined && !wanted.has(stem)) {
      continue;
    }
    const doc = parseTextproto(readFileSync(`${dir}${entry}`, 'utf-8'));
    rows.push(...loadSimpleTestFile(stem, doc));
  }
  return rows;
}

/**
 * Run the corpus and aggregate.  One {@link Engine} is shared across rows
 * (it is Program-independent); each row plans + evals its own Program.
 */
export async function runCorpus(
  opts: RunCorpusOptions,
): Promise<ConformanceReport> {
  const rows = loadCorpus(opts.corpusDir, opts.files);
  const proto = loadProtoSetup(opts.descriptorSetPath);
  const engine = await Engine.create(
    proto.descriptorBytes !== undefined
      ? { descriptors: proto.descriptorBytes }
      : {},
  );

  let pass = 0;
  let skip = 0;
  let fail = 0;
  const skipByCategory = new Map<SkipCategory, number>();
  const failures: TaggedResult[] = [];

  for (const row of rows) {
    const result = await runRow(row, engine, proto.env);
    const tagged: TaggedResult = {
      file: row.file,
      section: row.section,
      name: row.name,
      expr: row.expr,
      result,
    };
    opts.onRow?.(tagged);
    switch (result.outcome) {
      case 'pass':
        pass += 1;
        break;
      case 'skip':
        skip += 1;
        if (result.category !== undefined) {
          skipByCategory.set(
            result.category,
            (skipByCategory.get(result.category) ?? 0) + 1,
          );
        }
        break;
      case 'fail':
        fail += 1;
        failures.push(tagged);
        break;
    }
  }

  return {
    total: rows.length,
    pass,
    skip,
    fail,
    skipByCategory,
    failures,
  };
}

/** Render a one-line `summary:` line mirroring the C++ runner's output. */
export function summaryLine(report: ConformanceReport): string {
  return `summary: total=${String(report.total)} pass=${String(report.pass)} skip=${String(report.skip)} fail=${String(report.fail)}`;
}

/** Render the per-category skip breakdown, sorted by descending count. */
export function skipBreakdown(report: ConformanceReport): string {
  const lines = [...report.skipByCategory.entries()]
    .sort((a, b) => b[1] - a[1])
    .map(([cat, count]) => `  ${cat}: ${String(count)}`);
  return lines.join('\n');
}

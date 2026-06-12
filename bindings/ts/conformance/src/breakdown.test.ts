// Tests for the per-feature breakdown: per-file stats, the (file ×
// category) grouping, normalized reason grouping, and the fixable /
// out-of-scope split.  Drives synthetic TaggedResult streams so the
// aggregation is tested independent of a corpus run.

import { describe, expect, it } from 'vitest';

import {
  featureBreakdown,
  fileCategoryCells,
  fileStats,
  isFixable,
  normalizeReason,
  OUT_OF_SCOPE_CATEGORIES,
  reasonGroups,
  renderFileStats,
  renderReasonGroups,
} from './breakdown.js';
import type { SkipCategory } from './classify.js';
import type { TaggedResult } from './harness.js';
import type { RowResult } from './runner.js';

function row(
  file: string,
  outcome: RowResult['outcome'],
  opts: { category?: SkipCategory; detail?: string } = {},
): TaggedResult {
  const result: RowResult =
    outcome === 'skip'
      ? {
          outcome,
          ...(opts.category !== undefined ? { category: opts.category } : {}),
          detail: opts.detail ?? '',
        }
      : { outcome, detail: opts.detail ?? '' };
  return { file, section: 's', name: 'n', expr: 'e', result };
}

describe('isFixable / OUT_OF_SCOPE_CATEGORIES', () => {
  it('marks the by-design categories as not fixable', () => {
    expect(isFixable('static_subset')).toBe(false);
    expect(isFixable('disable_check')).toBe(false);
    expect(isFixable('check_only')).toBe(false);
    expect(isFixable('object_value')).toBe(false);
    expect(isFixable('spec_unimpl')).toBe(false);
  });

  it('marks the work categories as fixable', () => {
    expect(isFixable('eval_unimpl')).toBe(true);
    expect(isFixable('bindings')).toBe(true);
    expect(isFixable('envelope')).toBe(true);
    expect(isFixable('type_env')).toBe(true);
    expect(isFixable('cli_limitation')).toBe(true);
    expect(isFixable('ext_unimpl')).toBe(true);
    expect(isFixable('proto_unimpl')).toBe(true);
    expect(isFixable('compile_unimpl')).toBe(true);
  });

  it('OUT_OF_SCOPE_CATEGORIES is the exact by-design set', () => {
    expect([...OUT_OF_SCOPE_CATEGORIES].sort()).toEqual(
      [
        'check_only',
        'disable_check',
        'object_value',
        'spec_unimpl',
        'static_subset',
      ].sort(),
    );
  });
});

describe('normalizeReason', () => {
  it('strips single-quoted literals so per-row exprs group', () => {
    expect(normalizeReason("undeclared reference to 'math.foo'")).toBe(
      "undeclared reference to '…'",
    );
    expect(normalizeReason("undeclared reference to 'strings.bar'")).toBe(
      "undeclared reference to '…'",
    );
    expect(normalizeReason("undeclared reference to 'math.foo'")).toBe(
      normalizeReason("undeclared reference to 'strings.bar'"),
    );
  });

  it('strips double-quoted and backtick literals', () => {
    expect(normalizeReason('expected a message value for ".X"')).toContain(
      '"…"',
    );
    expect(normalizeReason('the `{?key}` construct')).toContain('`…`');
  });

  it('replaces standalone numbers with N', () => {
    expect(normalizeReason('error code=13 here')).toBe('error code=N here');
    expect(normalizeReason('error code=18 here')).toBe('error code=N here');
  });

  it('collapses whitespace and handles empty', () => {
    expect(normalizeReason('  a   b  ')).toBe('a b');
    expect(normalizeReason('')).toBe('(empty reason)');
  });
});

describe('fileStats', () => {
  it('counts pass/skip/fail per file and computes pass rate over evaluated rows', () => {
    const rows = [
      row('basic', 'pass'),
      row('basic', 'pass'),
      row('basic', 'fail'),
      row('basic', 'skip', { category: 'eval_unimpl' }),
      row('lists', 'pass'),
    ];
    const stats = fileStats(rows);
    const basic = stats.find((s) => s.file === 'basic');
    expect(basic).toBeDefined();
    expect(basic?.total).toBe(4);
    expect(basic?.pass).toBe(2);
    expect(basic?.fail).toBe(1);
    expect(basic?.skip).toBe(1);
    // pass rate is over evaluated (pass+fail) = 2/3.
    expect(basic?.passRate).toBeCloseTo(2 / 3);
    const lists = stats.find((s) => s.file === 'lists');
    expect(lists?.passRate).toBe(1);
  });

  it('treats an all-skip file as passRate 1 (no evaluated rows)', () => {
    const stats = fileStats([
      row('dynamic', 'skip', { category: 'static_subset' }),
    ]);
    expect(stats[0]?.passRate).toBe(1);
  });

  it('sorts most-skipped first', () => {
    const rows = [
      row('a', 'skip', { category: 'eval_unimpl' }),
      row('b', 'skip', { category: 'eval_unimpl' }),
      row('b', 'skip', { category: 'eval_unimpl' }),
    ];
    expect(fileStats(rows)[0]?.file).toBe('b');
  });
});

describe('fileCategoryCells', () => {
  it('groups skip rows by (file, category) and ignores pass/fail', () => {
    const rows = [
      row('proto2', 'skip', { category: 'proto_unimpl' }),
      row('proto2', 'skip', { category: 'proto_unimpl' }),
      row('proto2', 'skip', { category: 'eval_unimpl' }),
      row('proto2', 'pass'),
      row('proto2', 'fail'),
    ];
    const cells = fileCategoryCells(rows);
    const proto = cells.find(
      (c) => c.file === 'proto2' && c.category === 'proto_unimpl',
    );
    expect(proto?.count).toBe(2);
    expect(
      cells.find((c) => c.file === 'proto2' && c.category === 'eval_unimpl')
        ?.count,
    ).toBe(1);
    // No pass/fail cells.
    expect(cells.every((c) => c.count > 0)).toBe(true);
  });
});

describe('reasonGroups', () => {
  it('groups by normalized reason across files, counting rows and files', () => {
    const rows = [
      row('math_ext', 'skip', {
        category: 'ext_unimpl',
        detail: "undeclared reference to 'math.greatest'",
      }),
      row('math_ext', 'skip', {
        category: 'ext_unimpl',
        detail: "undeclared reference to 'math.least'",
      }),
      row('string_ext', 'skip', {
        category: 'ext_unimpl',
        detail: "undeclared reference to 'strings.quote'",
      }),
    ];
    const groups = reasonGroups(rows);
    expect(groups).toHaveLength(1);
    expect(groups[0]?.count).toBe(3);
    expect(groups[0]?.files).toEqual(['math_ext', 'string_ext']);
    expect(groups[0]?.fixable).toBe(true);
  });

  it('keeps distinct categories with the same text separate', () => {
    const rows = [
      row('a', 'skip', { category: 'eval_unimpl', detail: 'gap x' }),
      row('b', 'skip', { category: 'proto_unimpl', detail: 'gap x' }),
    ];
    expect(reasonGroups(rows)).toHaveLength(2);
  });

  it('ranks by descending count', () => {
    const rows = [
      row('a', 'skip', { category: 'eval_unimpl', detail: 'rare' }),
      row('a', 'skip', { category: 'bindings', detail: 'common' }),
      row('a', 'skip', { category: 'bindings', detail: 'common' }),
    ];
    expect(reasonGroups(rows)[0]?.reason).toBe('common');
  });

  it('flags by-design categories as not fixable', () => {
    const groups = reasonGroups([
      row('dynamic', 'skip', { category: 'static_subset', detail: 'dyn' }),
    ]);
    expect(groups[0]?.fixable).toBe(false);
  });
});

describe('featureBreakdown + renderers', () => {
  it('assembles all three views', () => {
    const rows = [
      row('basic', 'pass'),
      row('basic', 'skip', { category: 'eval_unimpl', detail: 'a gap' }),
    ];
    const bd = featureBreakdown(rows);
    expect(bd.byFile).toHaveLength(1);
    expect(bd.byFileCategory).toHaveLength(1);
    expect(bd.reasonGroups).toHaveLength(1);
  });

  it('renderFileStats and renderReasonGroups produce non-empty text', () => {
    const rows = [
      row('basic', 'pass'),
      row('basic', 'skip', { category: 'eval_unimpl', detail: 'a gap' }),
    ];
    const bd = featureBreakdown(rows);
    expect(renderFileStats(bd.byFile)).toContain('basic');
    const rg = renderReasonGroups(bd.reasonGroups);
    expect(rg).toContain('FIXABLE');
    expect(rg).toContain('a gap');
  });
});

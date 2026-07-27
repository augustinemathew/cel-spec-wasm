#!/usr/bin/env python3
"""Aggregate an lcov .dat report into per-file / per-directory tables.

Usage: lcov_report.py <combined_report.dat> [--misses]

Input is bazel's `--combined_report=lcov` output (or anything merged by
lcov_merge.py).  Prints per-file and rolled-up per-directory line +
function coverage; `--misses` appends the uncovered line ranges per
file.  Used to produce the tables in
doc/implementation-plan/rewrite/reviews/2026-07-27-test-inventory-and-coverage.md
and the m38 wasm-coverage comparison."""
import sys, collections

path = sys.argv[1]
files = {}  # sf -> dict(lf, lh, fnf, fnh, brf, brh, uncovered=[ranges])
cur = None
for line in open(path):
    line = line.strip()
    if line.startswith('SF:'):
        cur = files.setdefault(line[3:], dict(lf=0, lh=0, fnf=0, fnh=0, brf=0, brh=0, misses=[]))
    elif cur is None:
        continue
    elif line.startswith('LF:'): cur['lf'] = int(line[3:])
    elif line.startswith('LH:'): cur['lh'] = int(line[3:])
    elif line.startswith('FNF:'): cur['fnf'] = int(line[4:])
    elif line.startswith('FNH:'): cur['fnh'] = int(line[4:])
    elif line.startswith('BRF:'): cur['brf'] = int(line[4:])
    elif line.startswith('BRH:'): cur['brh'] = int(line[4:])
    elif line.startswith('DA:'):
        ln, cnt = line[3:].split(',')[:2]
        if int(cnt) == 0: cur['misses'].append(int(ln))
    elif line == 'end_of_record':
        cur = None

def pct(h, f): return f'{100.0*h/f:5.1f}%' if f else '  n/a'

def ranges(misses):
    out, start, prev = [], None, None
    for ln in sorted(misses):
        if start is None: start = prev = ln
        elif ln == prev + 1: prev = ln
        else:
            out.append(f'{start}' if start == prev else f'{start}-{prev}')
            start = prev = ln
    if start is not None:
        out.append(f'{start}' if start == prev else f'{start}-{prev}')
    return ','.join(out)

# per-dir rollup (top 2 path components)
dirs = collections.defaultdict(lambda: dict(lf=0, lh=0, fnf=0, fnh=0))
print('== PER FILE ==')
print(f'{"file":68s} {"lines":>12s} {"line%":>6s} {"funcs":>9s} {"func%":>6s}')
for sf in sorted(files):
    d = files[sf]
    parts = sf.split('/')
    key = '/'.join(parts[:2]) if len(parts) > 1 else parts[0]
    dd = dirs[key]
    dd['lf'] += d['lf']; dd['lh'] += d['lh']; dd['fnf'] += d['fnf']; dd['fnh'] += d['fnh']
    top = '/'.join(parts[:1])
    t = dirs.setdefault('TOTAL:' + top, dict(lf=0, lh=0, fnf=0, fnh=0))
    t['lf'] += d['lf']; t['lh'] += d['lh']; t['fnf'] += d['fnf']; t['fnh'] += d['fnh']
    print(f'{sf:68s} {d["lh"]:5d}/{d["lf"]:<6d} {pct(d["lh"],d["lf"])} {d["fnh"]:4d}/{d["fnf"]:<4d} {pct(d["fnh"],d["fnf"])}')

print()
print('== PER DIR (2-level) ==')
for k in sorted(dirs):
    d = dirs[k]
    print(f'{k:40s} {d["lh"]:6d}/{d["lf"]:<7d} {pct(d["lh"],d["lf"])} {d["fnh"]:5d}/{d["fnf"]:<5d} {pct(d["fnh"],d["fnf"])}')

if len(sys.argv) > 2 and sys.argv[2] == '--misses':
    print()
    print('== UNCOVERED LINE RANGES (files under 100%) ==')
    for sf in sorted(files):
        d = files[sf]
        if d['misses']:
            print(f'{sf}: {ranges(d["misses"])}')

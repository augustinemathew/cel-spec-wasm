#!/usr/bin/env python3
"""Merge N lcov .dat files: sum DA hits per (file,line), union FNDA.

Usage: lcov_merge.py <in1.dat> <in2.dat> [...] <out.dat>

Needed because native coverage must run as two bazel passes (runtime
instrumented for the native suites; runtime de-instrumented for the
wasm-instantiating suites — see the m38 doc §7) whose reports have to
be combined before lcov_report.py."""
import sys, collections

lines = collections.defaultdict(lambda: collections.defaultdict(int))   # sf -> line -> hits
fns = collections.defaultdict(lambda: collections.defaultdict(int))     # sf -> fname -> hits
fnline = collections.defaultdict(dict)                                  # sf -> fname -> line

for path in sys.argv[1:-1]:
    cur = None
    for raw in open(path):
        raw = raw.strip()
        if raw.startswith('SF:'):
            cur = raw[3:]
        elif cur is None:
            continue
        elif raw.startswith('DA:'):
            parts = raw[3:].split(',')
            lines[cur][int(parts[0])] += int(parts[1])
        elif raw.startswith('FN:'):
            ln, name = raw[3:].split(',', 1)
            fnline[cur][name] = int(ln)
            fns[cur][name] += 0
        elif raw.startswith('FNDA:'):
            cnt, name = raw[5:].split(',', 1)
            fns[cur][name] += int(cnt)
        elif raw == 'end_of_record':
            cur = None

with open(sys.argv[-1], 'w') as out:
    for sf in sorted(lines):
        out.write(f'SF:{sf}\n')
        for name, ln in sorted(fnline[sf].items(), key=lambda kv: kv[1]):
            out.write(f'FN:{ln},{name}\n')
        for name, cnt in fns[sf].items():
            out.write(f'FNDA:{cnt},{name}\n')
        out.write(f'FNF:{len(fnline[sf])}\n')
        out.write(f'FNH:{sum(1 for c in fns[sf].values() if c > 0)}\n')
        for ln in sorted(lines[sf]):
            out.write(f'DA:{ln},{lines[sf][ln]}\n')
        out.write(f'LF:{len(lines[sf])}\n')
        out.write(f'LH:{sum(1 for c in lines[sf].values() if c > 0)}\n')
        out.write('end_of_record\n')

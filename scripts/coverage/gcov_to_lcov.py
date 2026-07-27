#!/usr/bin/env python3
"""Convert `llvm-cov gcov` annotated .gcov files into one lcov .dat.

Usage: gcov_to_lcov.py <dir-with-.gcov-files> <out.dat> [source-prefix]

Runs after the m38 wasm-coverage harvest: `llvm-cov gcov *.gcda`
produces one `<src>.gcov` per source file; this folds them into lcov
`SF:`/`DA:`/`LF:`/`LH:` records so scripts/coverage/lcov_report.py and
lcov_merge.py can chew on wasm-side coverage exactly like the native
bazel report.  The `Source:` header inside each .gcov supplies the SF
path (relative, e.g. runtime/cel_time.c)."""
import os
import sys

def parse_gcov(path):
    source = None
    lines = {}
    for raw in open(path, errors='replace'):
        parts = raw.split(':', 2)
        if len(parts) < 3:
            continue
        count, lineno = parts[0].strip(), parts[1].strip()
        if lineno == '0':
            if parts[2].startswith('Source:'):
                source = parts[2][len('Source:'):].strip()
            continue
        if count == '-':
            continue
        n = 0 if count in ('#####', '=====') else int(count.replace('*', ''))
        ln = int(lineno)
        lines[ln] = max(lines.get(ln, 0), n)
    return source, lines

def main():
    gcov_dir, out_path = sys.argv[1], sys.argv[2]
    files = {}
    for name in sorted(os.listdir(gcov_dir)):
        if not name.endswith('.gcov'):
            continue
        source, lines = parse_gcov(os.path.join(gcov_dir, name))
        if source is None or not lines:
            continue
        acc = files.setdefault(source, {})
        for ln, n in lines.items():
            acc[ln] = max(acc.get(ln, 0), n)
    with open(out_path, 'w') as out:
        for source in sorted(files):
            lines = files[source]
            out.write(f'SF:{source}\n')
            for ln in sorted(lines):
                out.write(f'DA:{ln},{lines[ln]}\n')
            out.write(f'LF:{len(lines)}\n')
            out.write(f'LH:{sum(1 for c in lines.values() if c > 0)}\n')
            out.write('end_of_record\n')

if __name__ == '__main__':
    main()

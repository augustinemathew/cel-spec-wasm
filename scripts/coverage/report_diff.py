#!/usr/bin/env python3
"""Diff two coverage report.json files from native_cov_report.py.

Usage: report_diff.py <old/report.json> <new/report.json>

Prints headline/section deltas across all three cuts (all workloads,
e2e-only, tests-no-corpus), per-file line-percentage movement, and the
function-level churn (newly covered / newly uncovered / added /
removed).  The hill-climb loop runs this after every batch so each
report is read against the previous one, not in isolation.
"""

import json
import sys


PCT_KEYS = [
    ("line_pct", "line"),
    ("line_pct_e2e", "line e2e-only"),
    ("line_pct_tests", "line tests"),
    ("function_pct", "function"),
    ("branch_pct", "branch"),
]


def fmt_delta(old, new):
    if old is None or new is None:
        return "n/a"
    d = round(new - old, 2)
    sign = "+" if d >= 0 else ""
    return f"{old}% -> {new}% ({sign}{d})"


def diff_aggregates(old, new, label):
    print(f"== {label}")
    for key, name in PCT_KEYS:
        if key in old or key in new:
            print(f"  {name:18s} {fmt_delta(old.get(key), new.get(key))}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], encoding="utf-8") as f:
        old = json.load(f)
    with open(sys.argv[2], encoding="utf-8") as f:
        new = json.load(f)

    print(f"old: {old['generated_against']} ({old['generated_on']}), "
          f"{len(old['workloads'])} workloads")
    print(f"new: {new['generated_against']} ({new['generated_on']}), "
          f"{len(new['workloads'])} workloads")

    diff_aggregates(old["headline"], new["headline"], "headline")
    for sec in new.get("section_order", []):
        o = old.get("sections", {}).get(sec, {})
        n = new["sections"][sec]
        diff_aggregates(o, n, f"section {sec}")

    print("== per-file movement (|line-pct delta| >= 0.5)")
    moved = []
    for src in sorted(set(old["per_file"]) | set(new["per_file"])):
        o = old["per_file"].get(src)
        n = new["per_file"].get(src)
        if o is None:
            moved.append((src, None, n["line_pct"]))
        elif n is None:
            moved.append((src, o["line_pct"], None))
        elif abs(n["line_pct"] - o["line_pct"]) >= 0.5:
            moved.append((src, o["line_pct"], n["line_pct"]))
    for src, op, np_ in sorted(
        moved, key=lambda t: -(abs((t[2] or 0) - (t[1] or 0)))
    ):
        if op is None:
            print(f"  NEW   {src}: {np_}%")
        elif np_ is None:
            print(f"  GONE  {src} (was {op}%)")
        else:
            print(f"  {'+' if np_ > op else '-'}     "
                  f"{src}: {fmt_delta(op, np_)}")
    if not moved:
        print("  (none)")

    old_zero = set(old["gaps"]["zero_hit"])
    new_zero = set(new["gaps"]["zero_hit"])
    old_fns = set(old["functions"])
    new_fns = set(new["functions"])
    newly_covered = sorted(old_zero - new_zero & old_zero & new_fns)
    newly_covered = sorted((old_zero & new_fns) - new_zero)
    newly_uncovered = sorted((new_zero & old_fns) - old_zero)
    added = sorted(new_fns - old_fns)
    removed = sorted(old_fns - new_fns)
    print(f"== functions: zero-hit {len(old_zero)} -> {len(new_zero)}; "
          f"newly covered {len(newly_covered)}, "
          f"newly uncovered {len(newly_uncovered)}, "
          f"added {len(added)}, removed {len(removed)}")
    for label, keys in (
        ("newly covered", newly_covered),
        ("newly uncovered", newly_uncovered),
        ("added (uncovered only)",
         [k for k in added if k in new_zero]),
        ("removed", removed),
    ):
        for k in keys[:40]:
            e = new["functions"].get(k) or old["functions"].get(k)
            print(f"  {label:22s} {e['file']}:{e.get('line', 0)} "
                  f"{e.get('display', e['function'])}")
        if len(keys) > 40:
            print(f"  {label:22s} ... and {len(keys) - 40} more")

    ob = {(b["file"], b["line"]) for b in old["gaps"]["untaken_branches"]}
    nb = {(b["file"], b["line"]) for b in new["gaps"]["untaken_branches"]}
    print(f"== untaken branch sites: {len(ob)} -> {len(nb)} "
          f"(closed {len(ob - nb)}, opened {len(nb - ob)})")


if __name__ == "__main__":
    main()

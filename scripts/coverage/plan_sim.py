#!/usr/bin/env python3
"""Constraint-guided planner for the coverage hill climb.

Models the climb as budgeted set cover over the line×workload matrix
in report.json: candidate actions are ADD probes (targeting a
function-anchored cluster of e2e-uncovered lines, costed by whether
they can augment an existing suite or need new fixtures) and DELETE
merges (suites whose coverage is fully subsumed — both link modes
zero-unique — flagged for assertion review).  Greedy by marginal
predicted lines per LoC; near-optimal for this submodular objective,
and the plan is re-solved at every measured checkpoint anyway.

Usage:
  plan_sim.py <report.json> [--target-e2e 90] [--loc-budget N]
              [--alpha 1.07] [--verdicts function_verdicts.json]

Outputs the ordered action plan with predicted e2e%/tests% trajectory
and net-LoC consumption, plus the naive-plan comparison.
"""

import argparse
import bisect
import collections
import json
import os
import re
import sys

ERROR_ARM = re.compile(
    r"Mismatch|Error|Poison|Invalid|Trap|Reject|Fail|Wrong|OutOfRange|"
    r"Overflow|Unknown", re.IGNORECASE)

AUGMENT_LOC = 6      # add a row to an existing parameterized table
NEW_TEST_LOC = 18    # new TEST_F in an existing suite
NEW_FILE_LOC = 60    # fixed overhead of a new suite file


def cluster_uncovered(report, e2e_workloads):
    """Function-anchored clusters of e2e-uncovered product lines."""
    fns_by_file = collections.defaultdict(list)
    for e in report["functions"].values():
        if e.get("line"):
            fns_by_file[e["file"]].append((e["line"], e))
    for v in fns_by_file.values():
        v.sort(key=lambda t: t[0])

    clusters = []
    for src, per_line in report["lines"].items():
        if report["per_file"][src].get("harness"):
            continue
        uncov = sorted(int(l) for l, m in per_line.items()
                       if not any(w in e2e_workloads for w in m))
        if not uncov:
            continue
        starts = [s for s, _ in fns_by_file.get(src, [])]

        def fn_of(line, src=src, starts=starts):
            i = bisect.bisect_right(starts, line) - 1
            return fns_by_file[src][i][1] if i >= 0 else None

        run = [uncov[0]]
        for l in uncov[1:]:
            if l - run[-1] <= 2 and fn_of(l) is fn_of(run[0]):
                run.append(l)
            else:
                clusters.append((src, run, fn_of(run[0])))
                run = [l]
        clusters.append((src, run, fn_of(run[0])))
    return clusters


def suite_owning(report, src, e2e_workloads):
    """The e2e suite with the most line-hits on `src` (or None)."""
    counts = collections.Counter()
    for m in report["lines"][src].values():
        for w in m:
            if w in e2e_workloads:
                counts[w] += 1
    if not counts:
        return None, 0
    w, n = counts.most_common(1)[0]
    return w, n


def build_actions(report, verdicts, e2e_workloads):
    verdict_of = {}
    for e in report["functions"].values():
        if e.get("verdict") and e["verdict"] != "gap":
            verdict_of[(e["file"], e["function"])] = e["verdict"]

    actions = []
    file_seen_new = set()
    for src, lines, fn in cluster_uncovered(report, e2e_workloads):
        if fn and (src, fn["function"]) in verdict_of:
            continue  # by-design / test-only: excluded from the climb
        name = (fn or {}).get("display", "?")
        vehicle = "unit" if ERROR_ARM.search(name) else "e2e"
        owner, owner_hits = suite_owning(report, src, e2e_workloads)
        total = report["per_file"][src]["lines_total"]
        if owner and owner_hits >= 0.3 * total:
            loc = AUGMENT_LOC
        elif owner:
            loc = NEW_TEST_LOC
        else:
            loc = NEW_TEST_LOC + (0 if src in file_seen_new else NEW_FILE_LOC)
            file_seen_new.add(src)
        actions.append({
            "kind": "add", "file": src, "lines": lines, "fn": name[:70],
            "vehicle": vehicle, "loc": loc,
            "owner": owner or "(new suite)",
        })

    # Suite-level deletion/merge candidates — ITERATIVE: zero-unique is
    # relative to the surviving corpus, so recompute after each pick
    # (mutual overlap is not collective redundancy).  Only suites where
    # BOTH link modes stay zero-unique survive as candidates, and each
    # carries a mandatory assertion-review flag: coverage subsumption
    # is necessary, never sufficient, for deletion.
    wl = report["workloads"]
    by_suite = collections.defaultdict(list)
    for w, s in wl.items():
        if s["class"] == "e2e":
            by_suite[s["suite"]].append(w)
    suite_of = {w: wl[w]["suite"] for w in wl if wl[w]["class"] == "e2e"}
    line_hits = collections.defaultdict(set)   # (file,line) -> suites
    for src, per_line in report["lines"].items():
        for l, m in per_line.items():
            for w in m:
                if w in suite_of:
                    line_hits[(src, l)].add(suite_of[w])
    alive = set(by_suite)
    while True:
        unique = collections.Counter()
        for hitters in line_hits.values():
            living = hitters & alive
            if len(living) == 1:
                unique[next(iter(living))] += 1
        cands = sorted(s for s in alive if unique[s] == 0)
        if not cands:
            break
        suite = cands[0]
        alive.discard(suite)
        path = os.path.join("e2e", f"{suite}.cc")
        loc = 0
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                loc = sum(1 for _ in f)
        actions.append({
            "kind": "delete", "file": path, "suite": suite,
            "lines": [], "vehicle": "merge", "loc": -loc,
            "fn": "coverage-subsumed vs surviving corpus "
                  "(assertion review REQUIRED before deletion)",
            "owner": suite,
        })
    return actions


def simulate(report, actions, target_e2e, alpha, loc_budget):
    prod = {f: v for f, v in report["per_file"].items()
            if not v.get("harness")}
    lt = sum(v["lines_total"] for v in prod.values())
    le = sum(v["lines_covered_e2e"] for v in prod.values())
    lts = sum(v["lines_covered_tests"] for v in prod.values())

    adds = [a for a in actions if a["kind"] == "add"]
    dels = [a for a in actions if a["kind"] == "delete"]
    plan, loc_used = [], 0

    # Deletions first: free coverage-neutral LoC reclamation.
    for a in dels:
        plan.append((a, le / lt, lts / lt))
        loc_used += a["loc"]

    # Greedy adds by marginal predicted lines per LoC.
    adds.sort(key=lambda a: -(len(a["lines"]) * alpha) / a["loc"])
    for a in adds:
        if 100.0 * le / lt >= target_e2e:
            break
        if loc_budget is not None and loc_used + a["loc"] > loc_budget:
            continue
        gain = min(len(a["lines"]) * alpha,
                   lt - le if a["vehicle"] == "e2e" else lt - lts)
        if a["vehicle"] == "e2e":
            le += gain
            lts = min(lt, lts + gain)
        else:
            lts = min(lt, lts + gain)
        loc_used += a["loc"]
        plan.append((a, 100.0 * le / lt, 100.0 * lts / lt))
    return plan, loc_used, 100.0 * le / lt, 100.0 * lts / lt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("report")
    ap.add_argument("--target-e2e", type=float, default=90.0)
    ap.add_argument("--loc-budget", type=int, default=None)
    ap.add_argument("--alpha", type=float, default=1.07)
    ap.add_argument("--verdicts", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "function_verdicts.json"))
    ap.add_argument("--show", type=int, default=30,
                    help="actions to print")
    args = ap.parse_args()

    with open(args.report, encoding="utf-8") as f:
        report = json.load(f)
    e2e_workloads = {w for w, s in report["workloads"].items()
                     if s["class"] == "e2e"}
    actions = build_actions(report, args.verdicts, e2e_workloads)

    plan, loc, e2e_pct, tests_pct = simulate(
        report, actions, args.target_e2e, args.alpha, args.loc_budget)

    naive_loc = sum(NEW_TEST_LOC for a in actions if a["kind"] == "add"
                    and a["vehicle"] == "e2e")
    adds = [p for p in plan if p[0]["kind"] == "add"]
    dels = [p for p in plan if p[0]["kind"] == "delete"]
    print(f"plan: {len(adds)} probes + {len(dels)} suite merges; "
          f"net LoC {loc:+d} (naive all-new-TEST_F: +{naive_loc})")
    print(f"predicted end state: e2e {e2e_pct:.2f}%  tests {tests_pct:.2f}%")
    print(f"\n{'#':>3} {'act':4} {'veh':5} {'ΔLoC':>5} {'lines':>5} "
          f"{'e2e%':>6}  target")
    for i, (a, e, t) in enumerate(plan[:args.show]):
        tgt = f"{a['file']}::{a['fn']}" if a["kind"] == "add" else a["file"]
        print(f"{i:3d} {a['kind']:4} {a['vehicle']:5} {a['loc']:5d} "
              f"{len(a['lines']):5d} {e:6.2f}  {tgt[:78]}")
    if len(plan) > args.show:
        print(f"... and {len(plan) - args.show} more actions")

    per_suite = collections.Counter()
    for a, _, _ in adds:
        per_suite[a["owner"]] += 1
    print("\nprobe placement (augment target -> count):")
    for s, n in per_suite.most_common(12):
        print(f"  {n:4d}  {s}")


if __name__ == "__main__":
    main()

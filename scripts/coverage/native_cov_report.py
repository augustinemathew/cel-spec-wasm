#!/usr/bin/env python3
"""Combined coverage report — native C++ (with branches) + wasm runtime.

Consumes a `bazel coverage` run in LLVM source-based mode (see
llvm_gcov.sh for the repo_env incantation): each test target leaves a
merged .profdata blob at bazel-testlogs/<pkg>/<name>/coverage.dat.  For
every target this script finds the matching test binary under bazel-bin,
exports lcov (`llvm-cov export -format=lcov`, which carries FNDA / DA /
BRDA records), and joins the per-target data with e2e/test_taxonomy.json
into the same report.json / report.html the wasm pipeline emits —
per-workload function×line attribution, gap/redundancy pivots — plus a
branch layer (aggregate per line: taken/total, and the untaken-branch
site list that drives the "every branch hit" audit).

With `--wasm-cov-root <dir>` (a collect_wasm_gcov.sh output tree) the
wasm-side runtime data merges in as an additional layer: file sets are
disjoint (`runtime/*` vs the native dirs), per-workload attribution is
keyed by target basename so an e2e suite's wasm and native counters
land on one workload row, and the headline gains per-layer subtotals.

Usage:
  scripts/coverage/native_cov_report.py [--repo-root .] --out <dir> \
      [--wasm-cov-root <dir>] [--scope-prefix eval/ ...]

Native scope defaults to the instrumented first-party dirs (compiler/
eval/ shared/ abi/ tools/ conformance/).
"""

import argparse
import collections
import datetime
import glob
import json
import os
import re
import subprocess
import sys

LLVM_COV = os.environ.get("LLVM_COV", "/opt/homebrew/opt/llvm/bin/llvm-cov")
DEFAULT_SCOPES = [
    "compiler/", "eval/", "shared/", "abi/", "tools/", "conformance/",
]
IGNORE_RE = (
    r"external/|third_party/|bazel-out/|_test\.cc$|\.pb\.(h|cc)$|"
    r"test_util|_probe"
)

# Strip bazel sandbox/execroot prefixes off lcov SF paths.
_SF_NORM = re.compile(r"^.*?/execroot/_main/")


def bazel_info(key: str) -> str:
    return subprocess.run(
        ["bazel", "info", key], capture_output=True, text=True, check=True
    ).stdout.strip()


# sh_test workloads: `bazel-bin/<pkg>/<target>` is the wrapper
# script, not the instrumented executable the profile came from —
# llvm-cov must be pointed at the real binaries.  Values are the
# executables the script drives (first is the primary, the rest
# become `-object` args).
SH_TEST_BINARIES = {
    "tools/cel:cel_smoke_test": ["bazel-bin/tools/cel/cel"],
    "examples:examples_smoke_test": sorted(
        p for p in glob.glob("bazel-bin/examples/[0-9][0-9]_*")
        if os.path.isfile(p) and os.access(p, os.X_OK)
        and "." not in os.path.basename(p)),
}


def find_targets(testlogs: str):
    """Yields (workload, profdata_path, [binary_paths])."""
    for dirpath, _dirs, files in os.walk(testlogs):
        if "coverage.dat" not in files:
            continue
        rel = os.path.relpath(dirpath, testlogs)  # <pkg>/<target>
        key = rel.rsplit("/", 1)
        key = f"{key[0]}:{key[1]}" if len(key) == 2 else rel
        binaries = SH_TEST_BINARIES.get(key)
        if binaries is None:
            binary = os.path.join("bazel-bin", rel)
            if not os.path.exists(binary):
                continue
            binaries = [binary]
        binaries = [b for b in binaries
                    if os.path.exists(b) and not os.path.isdir(b)]
        if not binaries:
            continue
        workload = rel.replace("/", ":")
        yield workload, os.path.join(dirpath, "coverage.dat"), binaries


def export_lcov(binaries, profdata: str) -> str:
    cmd = [LLVM_COV, "export", binaries[0]]
    for extra in binaries[1:]:
        cmd += ["-object", extra]
    cmd += [f"-instr-profile={profdata}", "-format=lcov",
            f"-ignore-filename-regex={IGNORE_RE}"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"WARN: llvm-cov export failed for {binaries[0]}: "
              f"{r.stderr.strip().splitlines()[:1]}", file=sys.stderr)
        return ""
    return r.stdout


def parse_lcov(text: str, scopes):
    """{file: {functions: {name: [line, count]}, lines: {line: count},
              branches: {line: [taken, total]}}}"""
    out = {}
    cur = None
    fn_lines = {}
    for raw in text.splitlines():
        if raw.startswith("SF:"):
            path = _SF_NORM.sub("", raw[3:])
            cur = (
                out.setdefault(
                    path, {"functions": {}, "lines": {}, "branches": {}}
                )
                if any(path.startswith(s) for s in scopes)
                else None
            )
            fn_lines = {}
        elif cur is None:
            continue
        elif raw.startswith("FN:"):
            line, name = raw[3:].split(",", 1)
            fn_lines[name] = int(line)
        elif raw.startswith("FNDA:"):
            count, name = raw[5:].split(",", 1)
            entry = cur["functions"].setdefault(
                name, [fn_lines.get(name, 0), 0]
            )
            entry[1] += int(count)
        elif raw.startswith("DA:"):
            line, count = raw[3:].split(",")[:2]
            line = int(line)
            cur["lines"][line] = cur["lines"].get(line, 0) + int(count)
        elif raw.startswith("BRDA:"):
            line, _block, _branch, taken = raw[5:].split(",")
            line = int(line)
            slot = cur["branches"].setdefault(line, [0, 0])
            slot[1] += 1
            if taken not in ("-", "0"):
                slot[0] += 1
    return out


def demangle(names):
    """Batch c++filt; returns {mangled: pretty}."""
    uniq = sorted(set(names))
    try:
        r = subprocess.run(
            ["c++filt"], input="\n".join(uniq), capture_output=True,
            text=True, check=True,
        )
        pretty = r.stdout.splitlines()
        return dict(zip(uniq, pretty))
    except Exception:  # noqa: BLE001 — cosmetic only
        return {n: n for n in uniq}


# Report sections, in display order.  Files that fit none land in
# "Other" so nothing silently disappears from the totals.
SECTIONS = [
    ("Compiler", ("compiler/", "shared/", "abi/")),
    ("Eval", ("eval/",)),
    ("CelRuntime", ("runtime/",)),
    ("Other", ()),
]


def section_of(path: str) -> str:
    for name, prefixes in SECTIONS:
        if any(path.startswith(p) for p in prefixes):
            return name
    return "Other"


def is_harness_file(path: str) -> bool:
    """Files e2e can never execute by construction — harness, CLI,
    build-time emitters, the coverage infra itself.  They stay in the
    tests-100% goal but are excluded from the e2e-90% goal metric."""
    if path.startswith(("tools/", "conformance/", "benchmark/")):
        return True
    # cel_log is the debug/audit logging trampoline — diagnostics
    # infrastructure, not language surface (its formatter is
    # unit-covered; nothing in compiled CEL emits log calls).
    return ("/celfnc_emit/" in path or "test_fakes" in path
            or "wasm_gcov" in path or path == "eval/host/cel_log.cc")


def workload_class(workload: str, taxonomy) -> str:
    """Three classes: "e2e" (the e2e suites, plus the CLI / example /
    plugin-fixture suites declared outside e2e/BUILD.bazel that drive
    the same full public pipeline), "corpus"
    (external workloads — conformance, fuzz), "unit" (everything
    else).  The goal metrics exclude "corpus": e2e-only = e2e;
    tests = e2e+unit."""
    skey = workload
    for suf in ("_dynamic", "_static"):
        if skey.endswith(suf):
            skey = skey[: -len(suf)]
    if skey in taxonomy["suites"]:
        return "e2e"
    if (skey in taxonomy.get("external_e2e_workloads", {})
            and skey != "_comment"):
        return "e2e"
    if skey in taxonomy.get("external_workloads", {}):
        return "corpus"
    return "unit"


def apply_verdicts(functions, verdicts):
    """Attach a dead-code verdict to each function.  `verdicts` entries
    ({file, match, verdict, evidence}) match by file prefix + substring
    of the mangled or demangled name.  Verdict vocabulary:
      dead      — no caller anywhere in first-party code; delete.
      test-only — only unit tests call it; relocate to test support.
      by-design — unreachable tripwire / defense-in-depth; keep, exempt.
      gap       — reachable from the pipeline, missing a test (default
                  for zero-hit functions with no curated entry).
    """
    for e in functions.values():
        hit = None
        for v in verdicts:
            if not e["file"].startswith(v["file"]):
                continue
            if v["match"] in e["function"] or v["match"] in e["display"]:
                hit = v
                break
        if hit:
            e["verdict"] = hit["verdict"]
            e["verdict_evidence"] = hit.get("evidence", "")
        elif e["total_workloads"] == 0:
            e["verdict"] = "gap"


def compute_goal_metrics(report):
    """Verdict-aware goal denominators, product scope.

    The goals are "e2e 90%" and "tests 100% minus by-design", so the
    denominators exclude UNCOVERED lines lying inside functions whose
    verdict makes them unreachable for that cut: by-design and
    test-only for the e2e goal; by-design only for the tests goal (a
    test-only function is legitimately unit-reachable).  Covered lines
    always count — an exemption can never reduce what ran.  A
    function's extent is approximated as [its start, the next
    function's start) over instrumented lines.
    """
    import bisect as _bisect
    fns_by_file = collections.defaultdict(list)
    for e in report["functions"].values():
        if e.get("line"):
            fns_by_file[e["file"]].append((e["line"], e.get("verdict")))
    for v in fns_by_file.values():
        v.sort(key=lambda t: t[0])

    lt = e2e_cov = tests_cov = 0
    e2e_exempt = tests_exempt = 0
    e2e_workloads = set(report["e2e_workloads"])
    for src, per_line in report["lines"].items():
        if report["per_file"][src].get("harness"):
            continue
        starts = [s for s, _ in fns_by_file.get(src, [])]
        verdicts = [v for _, v in fns_by_file.get(src, [])]
        for l, m in per_line.items():
            lt += 1
            covered_e2e = any(w in e2e_workloads for w in m)
            covered_tests = bool(m)
            e2e_cov += covered_e2e
            tests_cov += covered_tests
            i = _bisect.bisect_right(starts, int(l)) - 1
            v = verdicts[i] if i >= 0 else None
            if not covered_e2e and v in ("by-design", "test-only"):
                e2e_exempt += 1
            if not covered_tests and v == "by-design":
                tests_exempt += 1
    h = report["headline"]
    h["line_pct_e2e_goal"] = round(
        100.0 * e2e_cov / (lt - e2e_exempt), 2) if lt > e2e_exempt else 0.0
    h["line_pct_tests_goal"] = round(
        100.0 * tests_cov / (lt - tests_exempt), 2) \
        if lt > tests_exempt else 0.0
    h["goal_exempt_lines_e2e"] = e2e_exempt
    h["goal_exempt_lines_tests"] = tests_exempt


def build(workloads, taxonomy, repo_root):
    fileset = sorted({f for w in workloads.values() for f in w})
    wclass = {w: workload_class(w, taxonomy) for w in workloads}
    e2e_workloads = {w for w, c in wclass.items() if c == "e2e"}
    test_workloads = {w for w, c in wclass.items() if c != "corpus"}

    functions, lines, branches = {}, collections.defaultdict(dict), {}
    for workload, per_file in workloads.items():
        for src, data in per_file.items():
            for fn, (start, count) in data["functions"].items():
                e = functions.setdefault(
                    f"{src}:{fn}",
                    {"file": src, "function": fn, "line": start, "hits": {}},
                )
                if start:
                    e["line"] = min(e["line"] or start, start)
                if count > 0:
                    e["hits"][workload] = e["hits"].get(workload, 0) + count
            for line, count in data["lines"].items():
                if count > 0:
                    lines[src].setdefault(line, {})[workload] = count
            for line, (taken, total) in data["branches"].items():
                slot = branches.setdefault(src, {}).setdefault(line, [0, 0])
                # Branch sites are per-compile identical across targets;
                # aggregate by max-taken (a branch is "hit" if ANY
                # workload took it).
                slot[0] = max(slot[0], taken)
                slot[1] = max(slot[1], total)
    for e in functions.values():
        e["total_workloads"] = len(e["hits"])
        e["total_hits"] = sum(e["hits"].values())

    # Demangle for display; keep mangled key stability.
    pretty = demangle([e["function"] for e in functions.values()])
    for e in functions.values():
        e["display"] = pretty.get(e["function"], e["function"])

    instrumented = {
        src: sorted(
            {l for w in workloads.values() if src in w
             for l in w[src]["lines"]}
        )
        for src in fileset
    }

    per_file = {}
    for src in fileset:
        total = len(instrumented[src])
        covered = sum(1 for l in instrumented[src] if lines[src].get(l))
        covered_e2e = sum(
            1 for l in instrumented[src]
            if any(w in e2e_workloads for w in lines[src].get(l, {}))
        )
        covered_tests = sum(
            1 for l in instrumented[src]
            if any(w in test_workloads for w in lines[src].get(l, {}))
        )
        fns = [e for e in functions.values() if e["file"] == src]
        fn_hit = sum(1 for e in fns if e["total_workloads"] > 0)
        fn_hit_e2e = sum(
            1 for e in fns if any(w in e2e_workloads for w in e["hits"])
        )
        fn_hit_tests = sum(
            1 for e in fns if any(w in test_workloads for w in e["hits"])
        )
        br = branches.get(src, {})
        br_total = sum(t for _, t in br.values())
        br_taken = sum(k for k, _ in br.values())
        per_file[src] = {
            "section": section_of(src),
            "harness": is_harness_file(src),
            "lines_total": total,
            "lines_covered": covered,
            "line_pct": round(100.0 * covered / total, 2) if total else 0.0,
            "lines_covered_e2e": covered_e2e,
            "line_pct_e2e": (
                round(100.0 * covered_e2e / total, 2) if total else 0.0
            ),
            "lines_covered_tests": covered_tests,
            "line_pct_tests": (
                round(100.0 * covered_tests / total, 2) if total else 0.0
            ),
            "functions_total": len(fns),
            "functions_hit": fn_hit,
            "function_pct": (
                round(100.0 * fn_hit / len(fns), 2) if fns else 0.0
            ),
            "functions_hit_e2e": fn_hit_e2e,
            "functions_hit_tests": fn_hit_tests,
            "branches_total": br_total,
            "branches_taken": br_taken,
            "branch_pct": (
                round(100.0 * br_taken / br_total, 2) if br_total else None
            ),
        }

    def subtotal(keys):
        lt = sum(per_file[k]["lines_total"] for k in keys)
        lc = sum(per_file[k]["lines_covered"] for k in keys)
        le = sum(per_file[k]["lines_covered_e2e"] for k in keys)
        lts = sum(per_file[k]["lines_covered_tests"] for k in keys)
        ft = sum(per_file[k]["functions_total"] for k in keys)
        fh = sum(per_file[k]["functions_hit"] for k in keys)
        fe = sum(per_file[k]["functions_hit_e2e"] for k in keys)
        fts = sum(per_file[k]["functions_hit_tests"] for k in keys)
        bt = sum(per_file[k]["branches_total"] for k in keys)
        bk = sum(per_file[k]["branches_taken"] for k in keys)
        return {
            "line_pct": round(100.0 * lc / lt, 2) if lt else 0.0,
            "lines_covered": lc, "lines_total": lt,
            "line_pct_e2e": round(100.0 * le / lt, 2) if lt else 0.0,
            "lines_covered_e2e": le,
            "line_pct_tests": round(100.0 * lts / lt, 2) if lt else 0.0,
            "lines_covered_tests": lts,
            "function_pct": round(100.0 * fh / ft, 2) if ft else 0.0,
            "functions_hit": fh, "functions_total": ft,
            "function_pct_e2e": round(100.0 * fe / ft, 2) if ft else 0.0,
            "function_pct_tests": (
                round(100.0 * fts / ft, 2) if ft else 0.0
            ),
            "branch_pct": round(100.0 * bk / bt, 2) if bt else 0.0,
            "branches_taken": bk, "branches_total": bt,
        }

    headline = subtotal(per_file)
    product = subtotal([f for f in per_file if not per_file[f]["harness"]])
    headline["line_pct_e2e_product"] = product["line_pct_e2e"]
    headline["line_pct_tests_product"] = product["line_pct_tests"]
    headline["product_lines_total"] = product["lines_total"]
    headline["product_lines_covered_e2e"] = product["lines_covered_e2e"]
    sections = {}
    for name, _prefixes in SECTIONS:
        keys = [f for f in per_file if per_file[f]["section"] == name]
        if keys:
            sections[name] = subtotal(keys)

    workload_stats = {}
    for workload, per_file_data in workloads.items():
        covered = {
            (src, l)
            for src, data in per_file_data.items()
            for l, c in data["lines"].items() if c > 0
        }
        unique = {
            (src, l) for (src, l) in covered
            if len(lines[src].get(l, {})) == 1
        }
        fn_unique = [
            k for k, e in functions.items() if list(e["hits"]) == [workload]
        ]
        skey = workload.split(":")[-1]
        for suf in ("_dynamic", "_static"):
            if skey.endswith(suf):
                skey = skey[: -len(suf)]
        tax = taxonomy["suites"].get(skey) or taxonomy.get(
            "external_workloads", {}
        ).get(skey)
        surfaces = (
            tax["surfaces"] if tax
            else [f"unit:{workload.split(':')[0].split('/')[-1]}"]
        )
        workload_stats[workload] = {
            "suite": skey,
            "class": wclass[workload],
            "surfaces": surfaces,
            "lines_covered": len(covered),
            "lines_unique": len(unique),
            "functions_unique": sorted(fn_unique),
        }

    zero = sorted(
        (k for k, e in functions.items() if e["total_workloads"] == 0),
        key=lambda k: (functions[k]["file"], functions[k]["line"]),
    )
    single = sorted(
        (k for k, e in functions.items() if e["total_workloads"] == 1),
        key=lambda k: (functions[k]["file"], functions[k]["line"]),
    )
    branch_gaps = sorted(
        (src, line, taken, total)
        for src, per_line in branches.items()
        for line, (taken, total) in per_line.items()
        if taken < total
    )

    sha = subprocess.run(
        ["git", "-C", repo_root, "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True, check=False,
    ).stdout.strip()

    return {
        "generated_against": sha,
        "generated_on": datetime.date.today().isoformat(),
        "artifact": "combined: native C++ (LLVM source-based, branch = "
                    "any-workload-taken) + wasm runtime (gcov)",
        "headline": headline,
        "sections": sections,
        "section_order": [n for n, _p in SECTIONS if n in sections],
        "e2e_workloads": sorted(e2e_workloads),
        "per_file": per_file,
        "functions": functions,
        "workloads": workload_stats,
        "gaps": {
            "zero_hit": zero,
            "single_workload": single,
            "untaken_branches": [
                {"file": f, "line": l, "taken": t, "total": n}
                for f, l, t, n in branch_gaps
            ],
        },
        "lines": {
            src: {str(l): lines[src].get(l, {}) for l in instrumented[src]}
            for src in fileset
        },
        "branches": {
            src: {str(l): v for l, v in per_line.items()}
            for src, per_line in branches.items()
        },
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--out", required=True)
    ap.add_argument("--wasm-cov-root", default=None,
                    help="collect_wasm_gcov.sh output tree to merge in")
    ap.add_argument("--verdicts", default=None,
                    help="dead-code verdicts JSON (default: "
                         "function_verdicts.json beside this script)")
    ap.add_argument("--scope-prefix", action="append", default=None)
    args = ap.parse_args()
    scopes = args.scope_prefix or DEFAULT_SCOPES

    testlogs = bazel_info("bazel-testlogs")
    with open(
        os.path.join(args.repo_root, "e2e", "test_taxonomy.json"),
        encoding="utf-8",
    ) as f:
        taxonomy = json.load(f)

    workloads = {}
    for workload, profdata, binary in sorted(find_targets(testlogs)):
        lcov = export_lcov(binary, profdata)
        if not lcov:
            continue
        parsed = parse_lcov(lcov, scopes)
        if parsed:
            # Key by target basename so the wasm layer's counters for
            # the same binary merge onto one workload row.
            workloads[workload.split(":")[-1]] = parsed
    if not workloads:
        sys.exit("no per-target coverage.dat found — run bazel coverage "
                 "first (see llvm_gcov.sh header)")

    if args.wasm_cov_root:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from wasm_gcov_report import load_workloads as load_wasm  # noqa: E402
        for workload, per_file in load_wasm(args.wasm_cov_root).items():
            slot = workloads.setdefault(workload, {})
            for src, data in per_file.items():
                if not src.startswith("runtime/"):
                    continue
                # wasm data carries no branch records.
                slot[src] = {**data, "branches": {}}

    report = build(workloads, taxonomy, args.repo_root)

    verdicts_path = args.verdicts or os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "function_verdicts.json",
    )
    if os.path.exists(verdicts_path):
        with open(verdicts_path, encoding="utf-8") as f:
            apply_verdicts(report["functions"], json.load(f)["verdicts"])
    compute_goal_metrics(report)

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "report.json"), "w",
              encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)

    # The HTML embeds a slimmed payload (report.json keeps full detail):
    # per-line full workload maps become [total_hits, n_workloads], and
    # per-function hit maps keep only the top contributors.  Keeps the
    # single-file page publishable (<~3 MB) at 140+ workloads.
    slim = dict(report)
    slim["lines"] = {
        src: {
            l: [sum(m.values()), len(m)] if m else []
            for l, m in per_line.items()
        }
        for src, per_line in report["lines"].items()
    }
    slim["functions"] = {
        k: {
            **e,
            "hits": dict(
                sorted(e["hits"].items(), key=lambda kv: -kv[1])[:6]
            ),
            "more_workloads": max(0, len(e["hits"]) - 6),
        }
        for k, e in report["functions"].items()
    }

    template_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "report_template.html"
    )
    with open(template_path, encoding="utf-8") as f:
        template = f.read()
    sources = {}
    for src in report["per_file"]:
        path = os.path.join(args.repo_root, src)
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                sources[src] = f.read().split("\n")
    html_out = template.replace(
        "/*__DATA__*/null",
        json.dumps({"report": slim, "sources": sources}, sort_keys=True),
    )
    with open(os.path.join(args.out, "report.html"), "w",
              encoding="utf-8") as f:
        f.write(html_out)

    h = report["headline"]
    print(f"workloads: {len(workloads)}")
    print(f"line: {h['line_pct']}% ({h['lines_covered']}/{h['lines_total']})")
    print(f"line e2e-only: {h['line_pct_e2e']}%  "
          f"line tests-no-corpus: {h['line_pct_tests']}%")
    print(f"product scope: e2e-only {h['line_pct_e2e_product']}% "
          f"(goal 90%)  tests {h['line_pct_tests_product']}%")
    print(f"GOAL METRICS (verdict-exempt): "
          f"e2e {h['line_pct_e2e_goal']}% "
          f"(exempt {h['goal_exempt_lines_e2e']} ln)  "
          f"tests {h['line_pct_tests_goal']}% "
          f"(exempt {h['goal_exempt_lines_tests']} ln)")
    print(f"function: {h['function_pct']}% "
          f"({h['functions_hit']}/{h['functions_total']})")
    print(f"branch: {h['branch_pct']}% "
          f"({h['branches_taken']}/{h['branches_total']})")
    print(f"zero-hit functions: {len(report['gaps']['zero_hit'])}; "
          f"untaken branch sites: "
          f"{len(report['gaps']['untaken_branches'])}")


if __name__ == "__main__":
    main()

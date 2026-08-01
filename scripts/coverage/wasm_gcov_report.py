#!/usr/bin/env python3
"""Join per-workload wasm gcov data into a function×test coverage report.

Consumes the output tree of collect_wasm_gcov.sh (raw/<workload>/ dirs of
`llvm-cov gcov -i` intermediate-format .gcov records) plus the suite→surface
classification in e2e/test_taxonomy.json, and emits:

  report.json  — machine-parsable: per-function and per-line hit data per
                 workload, aggregate percentages, gap/redundancy pivots.
  report.html  — single-file clickable explorer (overview, function audit,
                 workload contribution, annotated source drilldown).

Usage:
  scripts/coverage/wasm_gcov_report.py --cov-root <dir> \
      [--repo-root .] [--out <dir>]

The headline number this exists to produce: line + function coverage of
runtime/*.c as compiled into cel_runtime.wasm, after ALL e2e workloads
(and the conformance corpus, when collected) have executed.
"""

import argparse
import collections
import datetime
import html
import json
import os
import subprocess
import sys

# Workload name → taxonomy suite key: strip the link-mode suffix the
# e2e BUILD macros append.
_MODE_SUFFIXES = ("_dynamic", "_static")


def suite_key(workload: str) -> str:
    for suffix in _MODE_SUFFIXES:
        if workload.endswith(suffix):
            return workload[: -len(suffix)]
    return workload


def parse_intermediate(path: str):
    """Parses one `llvm-cov gcov -i` file.

    Returns {source_file: {"functions": {name: [start_line, count]},
                           "lines": {line: count}}}.
    """
    out = {}
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            raw = raw.strip()
            if raw.startswith("file:"):
                cur = out.setdefault(
                    raw[5:], {"functions": {}, "lines": {}}
                )
            elif cur is None:
                continue
            elif raw.startswith("function:"):
                start, count, name = raw[9:].split(",", 2)
                fn = cur["functions"].setdefault(name, [int(start), 0])
                fn[1] += int(count)
            elif raw.startswith("lcount:"):
                line, count = raw[7:].split(",")[:2]
                line = int(line)
                cur["lines"][line] = cur["lines"].get(line, 0) + int(count)
    return out


def load_workloads(cov_root: str):
    """{workload: {file: {functions, lines}}} from raw/<workload>/*.gcov."""
    raw_root = os.path.join(cov_root, "raw")
    workloads = {}
    for workload in sorted(os.listdir(raw_root)):
        wdir = os.path.join(raw_root, workload)
        if not os.path.isdir(wdir):
            continue
        merged = {}
        for name in sorted(os.listdir(wdir)):
            if not name.endswith(".gcov"):
                continue
            for src, data in parse_intermediate(
                os.path.join(wdir, name)
            ).items():
                slot = merged.setdefault(src, {"functions": {}, "lines": {}})
                for fn, (start, count) in data["functions"].items():
                    cur = slot["functions"].setdefault(fn, [start, 0])
                    cur[1] += count
                for line, count in data["lines"].items():
                    slot["lines"][line] = slot["lines"].get(line, 0) + count
        if merged:
            workloads[workload] = merged
    return workloads


def build_report(workloads, taxonomy, repo_root, file_prefix="runtime/"):
    # Scope to first-party runtime sources: the C++ runtime TUs inline
    # instrumented template code from libc++/absl/re2 headers, which
    # is not cel_runtime surface and would dilute the numbers.
    files = sorted(
        {
            f
            for w in workloads.values()
            for f in w
            if f.startswith(file_prefix)
        }
    )

    # ── function table ────────────────────────────────────────────────
    # key: "file:function" → {file, function, line, hits: {workload: n}}
    functions = {}
    fileset = set(files)
    for workload, per_file in workloads.items():
        for src, data in per_file.items():
            if src not in fileset:
                continue
            for fn, (start, count) in data["functions"].items():
                entry = functions.setdefault(
                    f"{src}:{fn}",
                    {"file": src, "function": fn, "line": start, "hits": {}},
                )
                entry["line"] = min(entry["line"], start)
                if count > 0:
                    entry["hits"][workload] = count
    for entry in functions.values():
        entry["total_workloads"] = len(entry["hits"])
        entry["total_hits"] = sum(entry["hits"].values())

    # ── line data ─────────────────────────────────────────────────────
    # per file: line → {workload: count}; plus per-workload covered sets
    lines = {src: collections.defaultdict(dict) for src in files}
    for workload, per_file in workloads.items():
        for src, data in per_file.items():
            if src not in fileset:
                continue
            for line, count in data["lines"].items():
                if count > 0:
                    lines[src][line][workload] = count

    instrumented = {  # every line any workload's .gcno declares
        src: sorted(
            {l for w in workloads.values() if src in w
             for l in w[src]["lines"]}
        )
        for src in files
    }

    # ── per-file aggregates ───────────────────────────────────────────
    per_file = {}
    for src in files:
        total = len(instrumented[src])
        covered = sum(1 for l in instrumented[src] if lines[src][l])
        fns = [e for e in functions.values() if e["file"] == src]
        fn_hit = sum(1 for e in fns if e["total_workloads"] > 0)
        per_file[src] = {
            "lines_total": total,
            "lines_covered": covered,
            "line_pct": round(100.0 * covered / total, 2) if total else 0.0,
            "functions_total": len(fns),
            "functions_hit": fn_hit,
            "function_pct": (
                round(100.0 * fn_hit / len(fns), 2) if fns else 0.0
            ),
        }

    # ── headline ──────────────────────────────────────────────────────
    lt = sum(v["lines_total"] for v in per_file.values())
    lc = sum(v["lines_covered"] for v in per_file.values())
    ft = sum(v["functions_total"] for v in per_file.values())
    fh = sum(v["functions_hit"] for v in per_file.values())
    headline = {
        "line_pct": round(100.0 * lc / lt, 2) if lt else 0.0,
        "lines_covered": lc,
        "lines_total": lt,
        "function_pct": round(100.0 * fh / ft, 2) if ft else 0.0,
        "functions_hit": fh,
        "functions_total": ft,
    }

    # ── per-workload contribution + redundancy ────────────────────────
    workload_stats = {}
    for workload, per_file_data in workloads.items():
        covered = {
            (src, l)
            for src, data in per_file_data.items()
            if src in fileset
            for l, c in data["lines"].items()
            if c > 0
        }
        unique = {
            (src, l)
            for (src, l) in covered
            if len(lines[src][l]) == 1
        }
        fn_unique = [
            k for k, e in functions.items()
            if list(e["hits"]) == [workload]
        ]
        skey = suite_key(workload)
        tax = taxonomy["suites"].get(skey) or taxonomy.get(
            "external_workloads", {}
        ).get(skey, {})
        workload_stats[workload] = {
            "suite": skey,
            "surfaces": tax.get("surfaces", []),
            "lines_covered": len(covered),
            "lines_unique": len(unique),
            "functions_unique": sorted(fn_unique),
        }

    # ── gap pivots ────────────────────────────────────────────────────
    zero = sorted(
        (k for k, e in functions.items() if e["total_workloads"] == 0),
        key=lambda k: (functions[k]["file"], functions[k]["line"]),
    )
    single = sorted(
        (k for k, e in functions.items() if e["total_workloads"] == 1),
        key=lambda k: (functions[k]["file"], functions[k]["line"]),
    )

    sha = subprocess.run(
        ["git", "-C", repo_root, "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True, check=False,
    ).stdout.strip()

    return {
        "generated_against": sha,
        "generated_on": datetime.date.today().isoformat(),
        "artifact": "cel_runtime.wasm (runtime/*.c, wasm32-wasi, "
                    "--//runtime:instrument_wasm)",
        "headline": headline,
        "per_file": per_file,
        "functions": functions,
        "workloads": workload_stats,
        "gaps": {"zero_hit": zero, "single_workload": single},
        "lines": {
            src: {str(l): lines[src][l] for l in instrumented[src]}
            for src in files
        },
    }


def embed_sources(report, repo_root):
    """{file: [source lines]} for the annotated drilldown."""
    out = {}
    for src in report["per_file"]:
        path = os.path.join(repo_root, src)
        if os.path.exists(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                out[src] = f.read().split("\n")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cov-root", required=True)
    ap.add_argument("--repo-root", default=".")
    ap.add_argument("--out", default=None,
                    help="output dir (default: <cov-root>)")
    args = ap.parse_args()
    out_dir = args.out or args.cov_root

    tax_path = os.path.join(args.repo_root, "e2e", "test_taxonomy.json")
    with open(tax_path, encoding="utf-8") as f:
        taxonomy = json.load(f)

    workloads = load_workloads(args.cov_root)
    if not workloads:
        sys.exit(f"no workload data under {args.cov_root}/raw")
    report = build_report(workloads, taxonomy, args.repo_root)

    os.makedirs(out_dir, exist_ok=True)
    json_path = os.path.join(out_dir, "report.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)

    template_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "report_template.html"
    )
    with open(template_path, encoding="utf-8") as f:
        template = f.read()
    payload = {
        "report": report,
        "sources": embed_sources(report, args.repo_root),
    }
    html_out = template.replace(
        "/*__DATA__*/null",
        json.dumps(payload, sort_keys=True),
    )
    html_path = os.path.join(out_dir, "report.html")
    with open(html_path, "w", encoding="utf-8") as f:
        f.write(html_out)

    h = report["headline"]
    print(f"workloads: {len(workloads)}")
    print(
        f"wasm runtime line coverage: {h['line_pct']}% "
        f"({h['lines_covered']}/{h['lines_total']})"
    )
    print(
        f"wasm runtime function coverage: {h['function_pct']}% "
        f"({h['functions_hit']}/{h['functions_total']})"
    )
    print(f"zero-hit functions: {len(report['gaps']['zero_hit'])}")
    print(f"wrote {json_path}\nwrote {html_path}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Joined benchmark reporter (benchmark/DESIGN.md §12).

Reads one Google Benchmark JSON per comparator, joins cells by BM name,
joins the human-facing expression from the corpus YAMLs, and emits:

  * the MAIN report (`--out-md` / `--out-csv`) — STATIC-focused.  It
    carries only the baseline (cel-cpp) and the non-dynamic comparators
    (celwasm-static), so each detail row is 3 data columns:
    `cel-cpp (ns)`, `celwasm-static (ns)`, `celwasm-static ×cel-cpp`.
    Layout: per-surface detail tables FIRST, then the per-operator
    headline (linear-regression slope/setup/crossover) at the BOTTOM.
    Dynamic comparators never appear here.
  * the DYNAMIC report (derived sibling files `<out>-dynamic.md` /
    `<out>-dynamic.csv`, emitted only when a `celwasm-dynamic`
    comparator is present) — the dynamic numbers in isolation
    (`cel-cpp (ns)`, `celwasm-dynamic (ns)`, `celwasm-dynamic ×cel-cpp`),
    since dynamic is slow + noisy and would otherwise crowd the main
    report.  Same detail-first / headline-last layout.
  * a long-format CSV per report (full expressions, machine-readable).
  * optionally, an auto-generated results section spliced into
    benchmark/README.md between the AUTO-GENERATED RESULTS markers
    (the STATIC headline only).

Usage (run.sh drives this; manual invocation):
  report.py --json celwasm-dynamic=/tmp/d.json --json celwasm-static=/tmp/s.json \
            --json cel-cpp=/tmp/c.json --baseline cel-cpp \
            --out-md results/2026-06-11-host.md --out-csv results/2026-06-11-host.csv \
            --update-readme ../README.md
  # writes results/2026-06-11-host.md/.csv (static) AND
  #        results/2026-06-11-host-dynamic.md/.csv (dynamic).

A comparator counts as "dynamic" iff its name contains "dynamic"
(see is_dynamic); everything else is a main-report comparator.

Parity: both bench mains stamp `result=...` labels (DESIGN.md §11); when
comparators disagree on a cell's label the row is tagged
`(parity mismatch)` — a mismatch is a bug, not a number.
"""

import argparse
import csv
import datetime
import io
import json
import pathlib
import re
import socket
import sys

import yaml

# Surface → BM prefix.  MUST stay in sync with BmPrefixForSurface in
# celwasm_bench.cc / celcpp_bench.cc (the C++ table is authoritative).
SURFACE_PREFIXES = {
    "arithmetic": "arith",
    "comparisons": "cmp",
    "comprehensions": "compr",
    "conversions": "conv",
    "index": "idx",
    "lists": "in_list",
    "literals": "lit",
    "logic": "logic",
    "long_strings": "str",
    "maps": "map",
    "policies": "policy",
    "proto": "proto",
    "size": "size",
    "strings": "str",
    "ternary": "ternary",
    "time": "time",
}

CHAIN_SEPS = [" && ", " || ", " + ", " * ", " - ", ", "]
MARKER_BEGIN = "<!-- BEGIN AUTO-GENERATED RESULTS (benchmark/eval/report.py) -->"
MARKER_END = "<!-- END AUTO-GENERATED RESULTS -->"


def load_corpus(corpus_dir):
    """Returns {bm_name: {surface, id, source, tags}} from corpus YAMLs."""
    cells = {}
    for path in sorted(pathlib.Path(corpus_dir).glob("*.yaml")):
        doc = yaml.safe_load(path.read_text())
        surface = doc.get("surface", path.stem)
        prefix = SURFACE_PREFIXES.get(surface)
        if prefix is None:
            sys.exit(f"report.py: no BM prefix for surface '{surface}' "
                     f"({path}); update SURFACE_PREFIXES (and the C++ table)")
        for cell in doc.get("cells", []):
            bm = f"BM_{prefix}_{cell['id']}"
            cells[bm] = {
                "surface": surface,
                "id": str(cell["id"]),
                "source": cell.get("source", ""),
                "tags": cell.get("tags", []),
            }
    return cells


def load_bench_json(path):
    """Returns {bm_name: {ns, label}} from a Google Benchmark JSON.

    Repetition-aware: under --benchmark_repetitions the JSON carries
    per-rep rows plus aggregate rows named `<run_name>_median` etc.
    Rows are keyed by `run_name` (the BM name without the aggregate
    suffix), and a median aggregate always wins over per-rep rows.
    """
    with open(path) as f:
        doc = json.load(f)
    out = {}
    for b in doc.get("benchmarks", []):
        is_aggregate = b.get("run_type") == "aggregate"
        if is_aggregate and b.get("aggregate_name") != "median":
            continue
        name = b.get("run_name", b["name"])
        if name in out and out[name].get("is_median") and not is_aggregate:
            continue
        unit = b.get("time_unit", "ns")
        scale = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}[unit]
        out[name] = {
            "ns": b["real_time"] * scale,
            "label": b.get("label", ""),
            "is_median": is_aggregate,
        }
    return out


def truncate_expr(expr, width):
    """Pattern-aware truncation (DESIGN.md §12.3)."""
    if len(expr) <= width:
        return expr
    for sep in CHAIN_SEPS:
        parts = expr.split(sep)
        if len(parts) > 4:
            head = sep.join(parts[:2])
            tail = sep.join(parts[-2:])
            short = f"{head}{sep}…{sep}{tail} ({len(parts)} terms)"
            if len(short) <= width + 20:
                return short
    return expr[: width - 1] + "…"


def md_escape(s):
    return s.replace("|", "\\|")


def natural_key(s):
    """Sort key splitting digit runs so intAdd2 < intAdd10Terms."""
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", s)]


def fmt_ns(ns):
    if ns is None:
        return "n/a"
    if ns >= 1e6:
        return f"{ns / 1e6:,.2f} ms"
    if ns >= 1e4:
        return f"{ns / 1e3:,.1f} µs"
    return f"{ns:,.0f}"


def parity_tag(row, comparators):
    labels = {row[c]["label"] for c in comparators
              if row.get(c) and row[c]["label"]}
    return " ⚠️parity" if len(labels) > 1 else ""


def linreg(points):
    """points: [(n, ns)] → (slope, intercept) or None."""
    if len(points) < 3:
        return None
    n = len(points)
    sx = sum(p[0] for p in points)
    sy = sum(p[1] for p in points)
    sxx = sum(p[0] * p[0] for p in points)
    sxy = sum(p[0] * p[1] for p in points)
    denom = sxx - sx * sx / n
    if denom == 0:
        return None
    slope = (sxy - sx * sy / n) / denom
    return slope, (sy - slope * sx) / n


FAMILY_RE = re.compile(r"^([A-Za-z][A-Za-z_]*?)(\d+)(Terms)?$")


def sweep_families(rows):
    """Group cells into length-sweep families by id pattern <stem><N>[Terms]."""
    fams = {}
    for bm, row in rows.items():
        m = FAMILY_RE.match(row["id"])
        if not m:
            continue
        key = (row["surface"], m.group(1).rstrip("_"))
        fams.setdefault(key, []).append((int(m.group(2)), bm))
    return {k: sorted(v) for k, v in fams.items() if len(v) >= 3}


def headline_table(rows, comparators, baseline):
    out = io.StringIO()
    fams = sweep_families(rows)
    if not fams:
        return ""
    others = [c for c in comparators if c != baseline]
    print("### Per-operator headline — T(N) = setup + N·per_op\n", file=out)
    print("Linear regression over each length-sweep family; slope is the "
          "steady-state cost of one more operation, crossover is the "
          f"expression length where the comparator overtakes {baseline}.\n",
          file=out)
    hdr = ["surface", "operator family", "points"]
    for c in comparators:
        hdr += [f"{c} slope", f"{c} setup"]
    for c in others:
        hdr += [f"{c} crossover vs {baseline}"]
    print("| " + " | ".join(hdr) + " |", file=out)
    print("|" + "---|" * len(hdr), file=out)
    for (surface, stem), pts in sorted(fams.items()):
        fits = {}
        for c in comparators:
            series = [(n, rows[bm][c]["ns"]) for n, bm in pts
                      if rows[bm].get(c)]
            fits[c] = linreg(series)
        cells = [surface, stem, str(len(pts))]
        for c in comparators:
            fit = fits[c]
            cells += ([f"{fit[0]:,.1f}", f"{fit[1]:,.0f}"] if fit
                      else ["n/a", "n/a"])
        for c in others:
            f_c, f_b = fits.get(c), fits.get(baseline)
            if not f_c or not f_b:
                cells.append("n/a")
            elif f_c[0] >= f_b[0]:
                cells.append("never wins")
            else:
                n = (f_c[1] - f_b[1]) / (f_b[0] - f_c[0])
                cells.append("always wins" if n <= 0 else f"N ≈ {n:,.0f}")
        print("| " + " | ".join(cells) + " |", file=out)
    print("", file=out)
    return out.getvalue()


def detail_tables(rows, comparators, baseline, width):
    out = io.StringIO()
    others = [c for c in comparators if c != baseline]
    by_surface = {}
    for bm, row in rows.items():
        by_surface.setdefault(row["surface"], []).append((row["id"], bm))
    for surface in sorted(by_surface):
        print(f"### {surface}\n", file=out)
        hdr = (["id", "expression"] + [f"{c} (ns)" for c in comparators]
               + [f"{c} ×{baseline}" for c in others])
        print("| " + " | ".join(hdr) + " |", file=out)
        print("|" + "---|" * len(hdr), file=out)
        for _, bm in sorted(by_surface[surface],
                            key=lambda ib: natural_key(ib[0])):
            row = rows[bm]
            expr = row["source"] if "show-full" in row["tags"] else (
                truncate_expr(row["source"], width))
            cells = [row["id"], f"`{md_escape(expr)}`" if expr else ""]
            for c in comparators:
                cells.append(fmt_ns(row[c]["ns"] if row.get(c) else None))
            base = row.get(baseline)
            for c in others:
                if row.get(c) and base:
                    cells.append(f"{base['ns'] / row[c]['ns']:.2f}×")
                else:
                    cells.append("n/a")
            tag = parity_tag(row, comparators)
            cells[0] += tag
            print("| " + " | ".join(cells) + " |", file=out)
        print("", file=out)
    return out.getvalue()


# Canonical column order, independent of --json arg order: the baseline
# (the reference impl, cel-cpp) reads first, then celwasm static before
# dynamic — slowest-to-set-up last.  Unknown comparators keep their input
# order after these.
COMPARATOR_RANK = {"celwasm-static": 0, "celwasm-dynamic": 1}


def is_dynamic(comparator):
    """A comparator is dynamic iff its name mentions 'dynamic'.

    Dynamic comparators are slow + noisy and live in their own report;
    the main (static-focused) report excludes them entirely.
    """
    return "dynamic" in comparator


def order_comparators(comparators, baseline):
    """Baseline first, then a fixed preference order (static < dynamic)."""
    def key(c):
        return (c != baseline, COMPARATOR_RANK.get(c, len(COMPARATOR_RANK)),
                comparators.index(c))
    return sorted(comparators, key=key)


def build_rows(corpus, jsons):
    """Join: bm_name → corpus metadata + one timing dict per comparator."""
    rows = {}
    for comp, bench in jsons.items():
        for bm, timing in bench.items():
            row = rows.setdefault(bm, dict(corpus.get(
                bm, {"surface": "(uncorpused)", "id": bm, "source": "",
                     "tags": []})))
            row[comp] = timing
    return rows


def build_doc(rows, comparators, baseline, title, width):
    """Detail tables first, per-operator headline last (DESIGN.md §12)."""
    headline = headline_table(rows, comparators, baseline)
    detail = detail_tables(rows, comparators, baseline, width)
    return (f"## {title}\n\n"
            f"Eval steady-state, median real time ns/call (lower is better); "
            f"`×{baseline}` > 1.0 means that comparator is faster than "
            f"{baseline}.  `n/a` = cell does not run on that comparator "
            f"(see skip tags in the corpus YAML).\n\n"
            + detail + headline), headline


def dynamic_path(path):
    """results/2026-06-11-host.md → results/2026-06-11-host-dynamic.md."""
    p = pathlib.Path(path)
    return str(p.with_name(f"{p.stem}-dynamic{p.suffix}"))


def write_csv(path, rows, comparators):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["surface", "id", "bm_name", "expression"]
                   + [f"{c}_ns" for c in comparators]
                   + [f"{c}_label" for c in comparators])
        for bm in sorted(rows):
            row = rows[bm]
            w.writerow(
                [row["surface"], row["id"], bm, row["source"]]
                + [f"{row[c]['ns']:.2f}" if row.get(c) else "" for c in comparators]
                + [row[c]["label"] if row.get(c) else "" for c in comparators])


def update_readme(readme_path, summary_md):
    text = pathlib.Path(readme_path).read_text()
    if MARKER_BEGIN not in text or MARKER_END not in text:
        sys.exit(f"report.py: {readme_path} is missing the "
                 f"AUTO-GENERATED RESULTS markers")
    head, rest = text.split(MARKER_BEGIN, 1)
    _, tail = rest.split(MARKER_END, 1)
    pathlib.Path(readme_path).write_text(
        head + MARKER_BEGIN + "\n" + summary_md + "\n" + MARKER_END + tail)


def list_surfaces(corpus_dir):
    """Prints the surface enumeration: name, BM prefix, cell count."""
    by_surface = {}
    for meta in load_corpus(corpus_dir).values():
        s = by_surface.setdefault(meta["surface"],
                                  {"n": 0, "example": meta["id"]})
        s["n"] += 1
    print(f"{'surface':<16} {'bm prefix':<10} {'cells':>5}   example filter")
    for name in sorted(by_surface):
        prefix = SURFACE_PREFIXES[name]
        print(f"{name:<16} {prefix:<10} {by_surface[name]['n']:>5}   "
              f"--benchmark_filter='^BM_{prefix}_'")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list-surfaces", action="store_true",
                    help="enumerate corpus surfaces (name, BM prefix, "
                         "cell count) and exit")
    ap.add_argument("--filter-for", nargs="+", metavar="SURFACE",
                    help="print the --benchmark_filter regex selecting "
                         "the given surfaces, and exit")
    ap.add_argument("--json", action="append",
                    metavar="NAME=PATH",
                    help="comparator name=Google-Benchmark-JSON path")
    ap.add_argument("--baseline", default="cel-cpp")
    ap.add_argument("--corpus-dir",
                    default=str(pathlib.Path(__file__).parent / "corpus"))
    ap.add_argument("--out-md")
    ap.add_argument("--out-csv")
    ap.add_argument("--update-readme")
    ap.add_argument("--max-expr-width", type=int, default=60)
    ap.add_argument("--title", default="")
    args = ap.parse_args()

    if args.list_surfaces:
        list_surfaces(args.corpus_dir)
        return
    if args.filter_for:
        unknown = [s for s in args.filter_for if s not in SURFACE_PREFIXES]
        if unknown:
            sys.exit(f"report.py: unknown surface(s) {unknown}; "
                     f"valid: {sorted(SURFACE_PREFIXES)}")
        prefixes = sorted({SURFACE_PREFIXES[s] for s in args.filter_for})
        print(f"^BM_({'|'.join(prefixes)})_")
        return
    if not args.json:
        ap.error("--json is required (or use --list-surfaces / "
                 "--filter-for)")

    jsons = {}
    for spec in args.json:
        name, _, path = spec.partition("=")
        if not path:
            sys.exit(f"report.py: --json wants NAME=PATH, got '{spec}'")
        jsons[name] = load_bench_json(path)
    comparators = list(jsons)
    if args.baseline not in jsons:
        sys.exit(f"report.py: baseline '{args.baseline}' not among "
                 f"comparators {comparators}")
    comparators = order_comparators(comparators, args.baseline)

    corpus = load_corpus(args.corpus_dir)
    rows = build_rows(corpus, jsons)

    stamp = datetime.date.today().isoformat()
    host = socket.gethostname().split(".")[0]
    title = args.title or f"Eval benchmark results — {stamp}, {host}"

    # Main report: STATIC-focused — baseline + non-dynamic comparators.
    main_comps = [c for c in comparators if not is_dynamic(c)]
    main_doc, main_headline = build_doc(
        rows, main_comps, args.baseline, title, args.max_expr_width)

    # Dynamic report: baseline + dynamic comparators, in its own files.
    dyn_others = [c for c in comparators if is_dynamic(c)]
    dyn_comps = ([args.baseline] + dyn_others) if dyn_others else []
    dyn_doc = None
    if dyn_comps:
        dyn_doc, _ = build_doc(
            rows, dyn_comps, args.baseline, f"{title} (dynamic)",
            args.max_expr_width)

    if args.out_md:
        pathlib.Path(args.out_md).parent.mkdir(parents=True, exist_ok=True)
        pathlib.Path(args.out_md).write_text(main_doc)
        print(f"wrote {args.out_md}")
        if dyn_doc is not None:
            dyn_md = dynamic_path(args.out_md)
            pathlib.Path(dyn_md).write_text(dyn_doc)
            print(f"wrote {dyn_md}")
    if args.out_csv:
        pathlib.Path(args.out_csv).parent.mkdir(parents=True, exist_ok=True)
        write_csv(args.out_csv, rows, main_comps)
        print(f"wrote {args.out_csv}")
        if dyn_comps:
            dyn_csv = dynamic_path(args.out_csv)
            write_csv(dyn_csv, rows, dyn_comps)
            print(f"wrote {dyn_csv}")
    if args.update_readme:
        summary = (f"_Last run: {stamp} on {host} "
                   f"(full tables: `benchmark/eval/results/`)._\n\n"
                   + main_headline)
        update_readme(args.update_readme, summary)
        print(f"updated results section in {args.update_readme}")
    if not (args.out_md or args.out_csv or args.update_readme):
        print(main_doc)


if __name__ == "__main__":
    main()

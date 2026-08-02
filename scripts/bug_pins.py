#!/usr/bin/env python3
"""Parse, validate, and report the CELBUG pin blocks in GTEST_SKIPs.

Every `GTEST_SKIP()` that pins a known bug carries a machine-parseable
CELBUG block (format defined in CLAUDE.md §"Reporting & tracking bugs
/ gaps via tests", rule 5).  This script is the tooling half: it
extracts every block, validates required fields and cross-references,
and renders the queue.

  scripts/bug_pins.py list                  # table, newest severity first
  scripts/bug_pins.py json                  # machine output for other tools
  scripts/bug_pins.py validate              # non-zero exit on a malformed pin
  scripts/bug_pins.py unmigrated            # skips with no CELBUG block yet
  scripts/bug_pins.py issue <ID>            # render a filed-issue body

`validate` is the CI gate: it fails on a missing required field, an
unknown severity, a duplicate id, or a `blocked-by` naming an id that
does not exist.
"""

import argparse
import json
import pathlib
import re
import sys
from collections import Counter

REPO = pathlib.Path(__file__).resolve().parent.parent
BLOCK_RE = re.compile(r"CELBUG v1\n(.*?)\n\s*\)CELBUG\"", re.DOTALL)
# Non-bug skips (by design / harness limitation / deferred feature)
# use a lighter block so they stay OUT of the bug queue but remain
# machine-checkable.  Mixing them into CELBUG would make the tracker
# untrustworthy — ~2/3 of this repo's skips are not defects.
SKIPBLOCK_RE = re.compile(r"CELSKIP v1\n(.*?)\n\s*\)CELSKIP\"", re.DOTALL)
SKIP_FIELDS = {"reason", "why-not-a-bug", "citation"}
SKIP_REASONS = {"by-design", "harness-limit", "deferred-feature"}
SKIP_RE = re.compile(r"GTEST_SKIP\(\)")
REQUIRED = ["id", "severity", "kind", "summary", "repro", "actual",
            "expected", "layer", "blocked-by", "fix-hint"]
# Only these start a new field.  Anything else is a CONTINUATION of the
# previous one — without this, a prose line like "asymmetry: timestamp()
# is fine" silently truncates fix-hint and invents a bogus field.
FIELDS = set(REQUIRED) | {"bindings", "issue", "found-by", "status"}
SEVERITIES = {"P0", "P1", "P2"}
KINDS = {"wrong-value", "missing-feature", "crash", "over-permissive",
         "diagnostics", "precision"}
ID_RE = re.compile(r"^CELW-\d{4}$")


def source_files():
    for p in REPO.rglob("*_test.cc"):
        if "third_party" in p.parts or "bazel-" in str(p):
            continue
        yield p


def parse_block(text, allowed=None):
    """Parse one CELBUG body into a dict. Continuation lines (indented)
    append to the previous field, so fix-hint can wrap."""
    fields, key = {}, None
    for raw in text.split("\n"):
        line = raw.strip()
        if not line:
            continue
        m = re.match(r"^([a-z][a-z-]*):\s*(.*)$", line)
        if m and m.group(1) in (allowed or FIELDS):
            key = m.group(1)
            fields[key] = m.group(2).strip()
        elif key:
            fields[key] += " " + line
    return fields


def collect_skips():
    """The non-bug CELSKIP blocks."""
    out = []
    for path in source_files():
        text = path.read_text(errors="replace")
        for m in SKIPBLOCK_RE.finditer(text):
            f = parse_block(m.group(1), SKIP_FIELDS)
            f["_file"] = str(path.relative_to(REPO))
            f["_line"] = text[: m.start()].count("\n") + 1
            out.append(f)
    return out


def collect():
    pins = []
    for path in source_files():
        text = path.read_text(errors="replace")
        for m in BLOCK_RE.finditer(text):
            fields = parse_block(m.group(1))
            fields["_file"] = str(path.relative_to(REPO))
            fields["_line"] = text[: m.start()].count("\n") + 1
            # Nearest preceding TEST( name, for provenance.
            head = text[: m.start()]
            t = re.findall(r"TEST(?:_[FP])?\(\s*(\w+)\s*,\s*(\w+)", head)
            fields["_test"] = f"{t[-1][0]}.{t[-1][1]}" if t else "?"
            pins.append(fields)
    return pins


def validate(pins):
    errors = []
    ids = Counter(p.get("id", "") for p in pins)
    for p in pins:
        where = f'{p["_file"]}:{p["_line"]} ({p["_test"]})'
        for f in REQUIRED:
            if not p.get(f):
                errors.append(f"{where}: missing required field `{f}`")
        sev, kind, pid = p.get("severity"), p.get("kind"), p.get("id", "")
        if sev and sev not in SEVERITIES:
            errors.append(f"{where}: severity `{sev}` not in {sorted(SEVERITIES)}")
        if kind and kind not in KINDS:
            errors.append(f"{where}: kind `{kind}` not in {sorted(KINDS)}")
        if pid and not ID_RE.match(pid):
            errors.append(f"{where}: id `{pid}` must match CELW-NNNN")
        if pid and ids[pid] > 1:
            errors.append(f"{where}: duplicate id `{pid}`")
    for p in pins:
        for f in p:
            if not f.startswith("_") and f not in FIELDS:
                errors.append(f'{p["_file"]}:{p["_line"]}: unknown field `{f}`')
    known = {p.get("id") for p in pins}
    for p in pins:
        dep = p.get("blocked-by", "none")
        if dep and dep != "none":
            for d in [x.strip() for x in dep.split(",")]:
                if d not in known:
                    errors.append(
                        f'{p["_file"]}:{p["_line"]}: blocked-by `{d}` is not a known pin id')
    return errors


def cmd_list(pins):
    order = {"P0": 0, "P1": 1, "P2": 2}
    pins.sort(key=lambda p: (order.get(p.get("severity", "P2"), 3), p.get("id", "")))
    print(f"{'ID':<11} {'SEV':<4} {'KIND':<16} {'BLOCKED-BY':<11} SUMMARY")
    for p in pins:
        print(f'{p.get("id","?"):<11} {p.get("severity","?"):<4} '
              f'{p.get("kind","?"):<16} {p.get("blocked-by","none"):<11} '
              f'{p.get("summary","")[:60]}')
    counts = Counter(p.get("severity", "?") for p in pins)
    print(f"\n{len(pins)} pinned: " +
          ", ".join(f"{k}={counts[k]}" for k in ("P0", "P1", "P2") if counts[k]))


def cmd_issue(pins, pid):
    p = next((x for x in pins if x.get("id") == pid), None)
    if p is None:
        sys.exit(f"no pin with id {pid}")
    dep = p.get("blocked-by", "none")
    print(f'[{p["severity"]}] {p["summary"]}\n')
    print(f'**Pinned test:** `{p["_test"]}` ({p["_file"]}:{p["_line"]})')
    print(f'**Severity:** {p["severity"]}   **Kind:** {p["kind"]}')
    if dep and dep != "none":
        print(f'**Blocked by:** {dep}')
    print(f'\n## Reproducer\n\n```\n{p["repro"]}\n```')
    if p.get("bindings"):
        print(f'\nActivation: `{p["bindings"]}`')
    print(f'\n- **Actual:** {p["actual"]}\n- **Expected:** {p["expected"]}')
    print(f'\n## Where the fix goes\n\n`{p["layer"]}`\n')
    print(f'## Notes for whoever fixes it\n\n{p["fix-hint"]}\n')
    if p.get("found-by"):
        print(f'_Found by: {p["found-by"]}_')
    print(f'\n_Filed from `{p["_file"]}`; delete the `GTEST_SKIP` there to close._')


def in_line_comment(text, pos):
    """True if `pos` sits after a `//` on its own line.

    Docs and file headers discuss `GTEST_SKIP()` by name; those mentions
    are prose, not pin sites, and reporting them as unmigrated makes the
    count untrustworthy (it over-reported by 3 before this check).
    """
    line_start = text.rfind("\n", 0, pos) + 1
    return text[line_start:pos].lstrip().startswith("//")


def cmd_unmigrated():
    rows = []
    for path in source_files():
        text = path.read_text(errors="replace")
        for m in SKIP_RE.finditer(text):
            if in_line_comment(text, m.start()):
                continue
            tail = text[m.start(): m.start() + 400]
            if "CELBUG v1" not in tail and "CELSKIP v1" not in tail:
                rows.append((str(path.relative_to(REPO)),
                             text[: m.start()].count("\n") + 1))
    for f, ln in rows:
        print(f"{f}:{ln}")
    print(f"\n{len(rows)} GTEST_SKIP(s) without a CELBUG block")
    return rows


def validate_skips(skips):
    """Errors for the non-bug CELSKIP blocks."""
    errors = []
    for r in skips:
        where = f'{r["_file"]}:{r["_line"]}'
        if r.get("reason") not in SKIP_REASONS:
            errors.append(f'{where}: CELSKIP reason `{r.get("reason")}` '
                          f"not in {sorted(SKIP_REASONS)}")
        for field in ("why-not-a-bug", "citation"):
            if not r.get(field):
                errors.append(f"{where}: CELSKIP missing `{field}`")
    return errors


def cmd_skips():
    rows = collect_skips()
    for r in rows:
        print(f'{r.get("reason","?"):<17} {r["_file"]}:{r["_line"]}  '
              f'{r.get("why-not-a-bug","")[:60]}')
    print(f"\n{len(rows)} non-bug skips")
    return 0


def cmd_validate():
    pins = collect()
    errors = validate(pins) + validate_skips(collect_skips())
    for e in errors:
        print(f"ERROR {e}", file=sys.stderr)
    print(f"{len(pins)} pins checked, {len(errors)} error(s)")
    return 1 if errors else 0


def cmd_json():
    print(json.dumps(collect(), indent=2))
    return 0


def main():
    # Each command is a zero-or-one-arg callable; main only dispatches.
    commands = {
        "list": lambda _: cmd_list(collect()),
        "json": lambda _: cmd_json(),
        "validate": lambda _: cmd_validate(),
        "unmigrated": lambda _: cmd_unmigrated() and 0,
        "skips": lambda _: cmd_skips(),
        "issue": lambda pid: cmd_issue(collect(), pid),
    }
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=sorted(commands))
    ap.add_argument("id", nargs="?")
    args = ap.parse_args()
    return commands[args.command](args.id) or 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Cap the bazel ``--disk_cache`` directory to a size budget.

Why this exists
---------------
Bazel's ``--disk_cache`` has no garbage collector on 7.3.2
(``--experimental_disk_cache_gc_max_size`` landed later), so the
directory only ever grows: every action output from every run is added
and nothing is evicted. CI compounds it — ``restore-keys`` pulls the
newest prior cache forward, the run appends to it, and the save writes a
bigger one for the next run to restore. Measured 5.1 GB after ONE cold
``bazel build //...``; left alone it ratchets until the runner fills up
(see ``scripts/ci_free_disk.sh`` for the failure that caused).

Eviction is always safe: the cache is pure content-addressed derived
data, so a missing entry is a cache miss and the action re-runs. We
evict oldest-mtime-first, which approximates LRU because bazel touches
entries it reads.

Python rather than shell because the obvious ``find -printf`` /
``stat -c`` formulation is GNU-only, and the macOS leg needs this too.

Usage:
    scripts/ci_prune_disk_cache.py <cache-dir> [budget-gb]
"""

from __future__ import annotations

import os
import sys


def _human(num_bytes: int) -> str:
    """Formats a byte count the way ``du -sh`` would."""
    size = float(num_bytes)
    for unit in ("B", "K", "M", "G", "T"):
        if size < 1024.0:
            return f"{size:.1f}{unit}"
        size /= 1024.0
    return f"{size:.1f}P"


def _walk(cache_dir: str) -> tuple[list[tuple[float, int, str]], int]:
    """Collects (mtime, size, path) for every file, plus the total size.

    Returns:
        A tuple of (entries, total_bytes). Files that vanish mid-walk
        (a concurrent bazel run) are skipped rather than raising.
    """
    entries: list[tuple[float, int, str]] = []
    total = 0
    for root, _dirs, files in os.walk(cache_dir):
        for name in files:
            path = os.path.join(root, name)
            try:
                st = os.lstat(path)
            except OSError:
                continue
            entries.append((st.st_mtime, st.st_size, path))
            total += st.st_size
    return entries, total


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(
            "usage: ci_prune_disk_cache.py <cache-dir> [budget-gb]",
            file=sys.stderr,
        )
        return 2

    cache_dir = argv[1]
    budget_gb = float(argv[2]) if len(argv) > 2 else 4.0
    budget = int(budget_gb * 1024**3)

    if not os.path.isdir(cache_dir):
        print(f"prune: {cache_dir} does not exist — nothing to prune.")
        return 0

    entries, total = _walk(cache_dir)
    print(
        f"prune: {cache_dir} is {_human(total)}, "
        f"budget {_human(budget)} ({len(entries)} files)"
    )

    if total <= budget:
        print("prune: under budget — nothing to do.")
        return 0

    # Oldest first. Bazel updates mtime on read, so this approximates LRU.
    entries.sort(key=lambda e: e[0])

    removed = 0
    for _mtime, size, path in entries:
        if total <= budget:
            break
        try:
            os.remove(path)
        except OSError:
            continue
        total -= size
        removed += 1

    print(f"prune: removed {removed} file(s); now {_human(total)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

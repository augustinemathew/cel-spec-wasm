#!/usr/bin/env bash
# ci_free_disk.sh — reclaim disk on a GitHub-hosted Linux runner.
#
# WHY THIS EXISTS.  A cold `bazel build //...` needs ~16 GB on this
# repo, measured in the docker/Dockerfile container (2026-08-02):
#
#     execroot (build outputs)      5.4 GB
#     external (fetched deps)       3.2 GB
#     bazel-disk-cache              5.1 GB   <- a 2nd copy of every action
#     repos cache (archives)        1.1 GB
#     sandbox                       0.2 GB
#                                  -------
#                                  ~16.0 GB
#
# A GitHub `ubuntu-24.04` runner starts with roughly 21 GB free, and the
# restored disk cache is a ~3 GB tarball that has to be downloaded AND
# extracted on that same volume.  The margin is thin enough that CI was
# failing outright with
#
#     java.io.IOException: write (No space left on device)
#
# partway through the build.  The runner image ships ~25 GB of SDKs this
# repo never touches (Android, Haskell/GHC, .NET, Swift), so deleting
# them buys more headroom than any amount of build-level tuning.
#
# Everything removed here is preinstalled tooling, NOT anything the
# build produces or fetches.  Each path is guarded — a runner image that
# drops one of these must not fail the job.
#
# Usage:  scripts/ci_free_disk.sh          # Linux CI runners only
#
# No-ops (exit 0) on non-Linux so the macOS leg can call it unguarded.

set -uo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "ci_free_disk.sh: not Linux — nothing to do."
  exit 0
fi

echo "=== disk before ==="
df -h /

# Preinstalled SDKs this repo never uses.  Sizes are the typical
# ubuntu-24.04 image as of 2026-08; they drift, hence the guards.
#
# NOT removed: /opt/hostedtoolcache.  It holds the Python the runner's
# own actions may resolve, and `scripts/` is Python-driven — the few GB
# it would save are not worth a mysterious mid-run failure.
STALE_PATHS=(
  /usr/local/lib/android      # ~12 GB — Android SDK/NDK
  /opt/ghc                    # ~5  GB — Haskell
  /usr/local/.ghcup           # ~3  GB — Haskell toolchain installer
  /usr/share/dotnet           # ~2  GB — .NET
  /usr/share/swift            # ~2  GB — Swift
  /usr/local/share/powershell
  /usr/local/share/chromium
  /usr/local/share/boost
)

for p in "${STALE_PATHS[@]}"; do
  if [[ -e "$p" ]]; then
    # `du` on a huge tree costs seconds; report only what we remove.
    size="$(du -sh "$p" 2>/dev/null | cut -f1 || echo '?')"
    sudo rm -rf "$p" 2>/dev/null && echo "  removed $p ($size)"
  fi
done

# Docker images ship preloaded on the runner and we build no containers
# in CI, so the local image store is dead weight (~3-5 GB).
if command -v docker >/dev/null 2>&1; then
  sudo docker image prune -af >/dev/null 2>&1 \
    && echo "  pruned docker images"
fi

echo "=== disk after ==="
df -h /

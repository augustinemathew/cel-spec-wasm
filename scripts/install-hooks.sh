#!/usr/bin/env bash
# install-hooks.sh — point this clone's git hooks at the in-repo
# `.githooks/` directory.  Idempotent.
#
# Why: git's default `.git/hooks/` lives outside version control,
# so hooks committed to the repo aren't picked up automatically.
# Setting `core.hooksPath` to a tracked directory fixes that.
#
# Run once after cloning:
#   scripts/install-hooks.sh
#
# Uninstall:
#   git config --unset core.hooksPath

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

if [[ ! -d .githooks ]]; then
  echo ".githooks/ not found in repo root" >&2
  exit 2
fi

# Make sure every hook in .githooks is executable.
for h in .githooks/*; do
  [[ -f "$h" ]] || continue
  chmod +x "$h"
done

current="$(git config --get core.hooksPath || true)"
if [[ "$current" == ".githooks" ]]; then
  echo "core.hooksPath already set to .githooks — nothing to do"
  exit 0
fi

git config core.hooksPath .githooks
echo "core.hooksPath -> .githooks"
echo
echo "Installed hooks:"
for h in .githooks/*; do
  [[ -f "$h" ]] || continue
  echo "  $(basename "$h")"
done
echo
echo "Bypass any single push with:   git push --no-verify"
echo "Bypass any single commit with: git commit --no-verify"
echo
echo "pre-commit knobs (all optional):"
echo "  CEL_HOOK_SKIP=1       skip the commit gate entirely"
echo "  CEL_HOOK_STRICT=1     make the fast hygiene checks blocking"
echo "  CEL_COMMIT_REVIEW=0   skip the agent review, keep the fast checks"
echo "  CEL_REVIEW_TIMEOUT=N  seconds to allow the agent (default 90)"

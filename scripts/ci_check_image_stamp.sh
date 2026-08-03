#!/usr/bin/env bash
# ci_check_image_stamp.sh — fail loudly when the warm CI image is stale.
#
# WHY THIS EXISTS.  The CI image bakes a populated bazel --disk_cache so
# jobs skip the ~15-minute cel-cpp compile.  When a dependency pin moves
# — cel-cpp.sha, a MODULE.bazel version, .bazelversion, or the
# Dockerfile's toolchain — every affected action gets a new cache key
# and the baked entries stop matching.
#
# Nothing becomes WRONG: the cache is content-addressed, so bazel
# rebuilds rather than serving a stale artifact.  The damage is silent
# SLOWNESS — CI drifts back to a cold build, or past its timeout, and
# the log gives no hint that the image is the reason.  This turns that
# into a hard, self-explanatory failure.
#
# Usage:  scripts/ci_check_image_stamp.sh [stamp-file]
#         default stamp-file: $CELWASM_CI_IMAGE_STAMP or
#         /opt/celwasm/ci-image.stamp
#
# Exit 0 = image matches this checkout.  Exit 1 = rebuild the image.
# Exit 0 with a notice when no stamp exists, so the script is safe to
# call from a job that is NOT running in the warm image.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"

stamp_file="${1:-${CELWASM_CI_IMAGE_STAMP:-/opt/celwasm/ci-image.stamp}}"

if [[ ! -f "$stamp_file" ]]; then
  echo "ci_check_image_stamp: no stamp at ${stamp_file} — not running in"
  echo "  the warm CI image.  Nothing to verify."
  exit 0
fi

want="$(scripts/ci_image_stamp.sh)"
have="$(tr -d '[:space:]' < "$stamp_file")"

if [[ "$want" == "$have" ]]; then
  echo "ci_check_image_stamp: image matches this checkout (${want:0:12})."
  exit 0
fi

cat >&2 <<MSG
ci_check_image_stamp: STALE CI IMAGE — refusing to run.

  image was built for : ${have:0:12}
  this checkout needs : ${want:0:12}

A pinned input moved since the image was built.  One of:

  third_party/cel-cpp.sha   MODULE.bazel / MODULE.bazel.lock
  .bazelversion            docker/Dockerfile (toolchain)

The baked --disk_cache no longer matches this commit's action keys, so
every job would silently fall back to a full cold build — the ~15-minute
cel-cpp compile this image exists to avoid, on every parallel lane.

Nothing would be built INCORRECTLY; the cache is content-addressed.
The failure here is deliberate: a slow CI that still passes is how a
warm image rots unnoticed.

FIX: rebuild and publish the image, then update the tag CI pulls:

    .github/workflows/ci-image.yml      (or run it via workflow_dispatch)

MSG
exit 1

#!/usr/bin/env bash
# ci_image_stamp.sh — identity of the inputs a warm CI image bakes.
#
# The CI image ships a populated bazel --disk_cache so jobs skip the
# ~15-minute cel-cpp compile.  That cache is content-addressed: change
# any input and the action keys change, the baked entries stop matching,
# and the build silently falls back to compiling from scratch.  Nothing
# is ever WRONG — bazel cannot serve a stale artifact for changed
# inputs — but CI quietly returns to being slow, which is how a warm
# image rots without anyone noticing.
#
# This prints a digest over everything that invalidates the cache:
#
#   third_party/cel-cpp.sha   the vendored dep pin
#   MODULE.bazel(.lock)       every bzlmod dep
#   .bazelversion             bazel is part of the action key
#   docker/Dockerfile         the toolchain.  `FROM ubuntu:24.04` with
#                             unpinned apt clang/lld means the compiler
#                             can move WITHOUT any repo file changing,
#                             so the Dockerfile is hashed as a proxy and
#                             the image records its real clang below.
#
# Baked into the image at build time, re-computed in CI, compared by
# ci_check_image_stamp.sh.
set -euo pipefail
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
{
  cat third_party/cel-cpp.sha
  cat .bazelversion
  cat MODULE.bazel
  cat MODULE.bazel.lock 2>/dev/null || true
  cat docker/Dockerfile
} | shasum -a 256 | cut -d' ' -f1

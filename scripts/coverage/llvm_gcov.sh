#!/usr/bin/env bash
# Coverage-tool shim for `bazel coverage` on macOS.
#
# With the autodetected clang toolchain bazel uses LLVM source-based
# coverage and — a long-standing bazel quirk — invokes the tool from
# the GCOV environment variable in BOTH roles: `llvm-profdata merge`
# (first arg "merge") and `llvm-cov export/gcov`.  Dispatch on the
# first argument so one shim serves both:
#
#   bazel coverage --repo_env=GCOV=$PWD/scripts/coverage/llvm_gcov.sh ...
LLVM_BIN=/opt/homebrew/opt/llvm/bin
case "$1" in
  merge) exec "$LLVM_BIN/llvm-profdata" "$@" ;;
  *)     exec "$LLVM_BIN/llvm-cov" "$@" ;;
esac

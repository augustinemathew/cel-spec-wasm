#!/usr/bin/env bash
# Wrapper that execs the wasi-sdk clang or clang++ depending on whether
# the input looks like C or C++.  Bazel's cc_toolchain calls the same
# tool for c-compile and c++-compile actions, but clang++ rejects
# `-std=c11` and clang doesn't auto-link libc++.  Dispatch on the input
# file extension we see in the args; fall back to clang++ for link /
# archive actions where no source file is present (so libc++ pulls in).
#
# Package depth: //third_party/wasi_sdk/ is two levels from the repo
# root, so `../../external/<repo>` reaches bazel's external store.
#
# Host-agnostic: toolchain resolution fetches exactly one
# `@wasi_sdk_<host>` archive (the one matching the build host), so the
# glob `external/*wasi_sdk_*/bin` resolves to a single directory.  The
# canonical external repo name embeds a bazel-version-specific
# separator (`_main~_repo_rules~wasi_sdk_<host>` on bazel 7.x), so we
# glob rather than spell it out.
for d in "$(dirname "$0")"/../../external/*wasi_sdk_*/bin; do
  WASI_BIN="$d"
done
for arg in "$@"; do
  case "$arg" in
    *.cc|*.cpp|*.cxx|*.C)
      exec "$WASI_BIN/clang++" "$@"
      ;;
    *.c)
      exec "$WASI_BIN/clang" "$@"
      ;;
  esac
done
exec "$WASI_BIN/clang++" "$@"

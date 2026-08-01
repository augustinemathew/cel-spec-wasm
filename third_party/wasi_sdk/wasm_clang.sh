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

# Strip any `-fuse-ld=*` the host config injected.  `.bazelrc` pins
# `build:linux --linkopt=-fuse-ld=lld` (GNU gold crashes on the host
# link), and bazel appends user linkopts AFTER toolchain flags, so the
# override leaks into wasm links too.  For wasm32-wasi(-threads) that
# is merely redundant (clang maps lld → wasm-ld, the default), but for
# wasm32-wasip2 it overrides clang's `wasm-component-ld` driver choice
# and silently emits a CORE module where a Component-Model component
# is expected (wasmtime then rejects it at AddComponent).  No wasm
# target ever wants the host linker override, so drop it here.
#
# Second strip, wasm32-wasip2 only: `-pthread` / `-lpthread`.  Third-
# party deps carry these in their own linkopts (absl and protobuf both
# do, via a select whose default arm assumes a threaded POSIX host).
# The preview2 Component-Model target has no threads — that is why it
# is a separate, `wasi_threads_off` platform — and `-pthread` makes
# clang pass `--shared-memory` to wasm-ld, which then rejects the link
# outright: "--shared-memory is disallowed ... not compiled with
# 'atomics' or 'bulk-memory'".  Dropping the flags is what the target
# already means; the threads-on wasm32-wasi toolchain keeps them.
#
# Bazel passes link args via a params file (`@bazel-out/.../*.params`,
# one arg per line), not argv — so the strips must also look inside any
# `@<file>` arg.  When a params file contains a matching line, we write
# a filtered copy to $TMPDIR and substitute the reference; clean params
# files pass through untouched (a single `grep -q` per file is the only
# extra cost on that path).
strip_re="^['\"]?-fuse-ld="
case " $* " in
  *--target=wasm32-wasip2*) strip_re="^['\"]?(-fuse-ld=|-pthread$|-lpthread$)" ;;
  *)
    for arg in "$@"; do
      case "$arg" in
        @*)
          f="${arg#@}"
          if [ -f "$f" ] && grep -q -- "--target=wasm32-wasip2" "$f"; then
            strip_re="^['\"]?(-fuse-ld=|-pthread$|-lpthread$)"
          fi
          ;;
      esac
    done
    ;;
esac

args=()
for arg in "$@"; do
  case "$arg" in
    -fuse-ld=*) ;;
    -pthread|-lpthread)
      # Kept unless this is a wasip2 link (see above).
      if [ "$strip_re" = "^['\"]?-fuse-ld=" ]; then args+=("$arg"); fi
      ;;
    @*)
      params_file="${arg#@}"
      if [ -f "$params_file" ] && \
         grep -qE "$strip_re" "$params_file"; then
        filtered="$(mktemp "${TMPDIR:-/tmp}/wasm_clang_params.XXXXXX")"
        grep -vE "$strip_re" "$params_file" > "$filtered"
        args+=("@$filtered")
      else
        args+=("$arg")
      fi
      ;;
    *) args+=("$arg") ;;
  esac
done
set -- "${args[@]}"

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

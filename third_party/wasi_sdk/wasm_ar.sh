#!/usr/bin/env bash
# Host-agnostic: exactly one `@wasi_sdk_<host>` archive is fetched by
# toolchain resolution, so the glob resolves to a single directory.
for d in "$(dirname "$0")"/../../external/*wasi_sdk_*/bin; do
  WASI_BIN="$d"
done
exec "$WASI_BIN/llvm-ar" "$@"

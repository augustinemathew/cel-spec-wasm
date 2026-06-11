#!/usr/bin/env bash
# Regenerate the conformance proto descriptor set.
#
# The conformance corpus's proto2 / proto3 rows reference the cel-spec test
# message types (`cel.expr.conformance.proto{2,3}.TestAllTypes` and the
# proto2 extension-scoped message).  Both the compiler binding (via the
# `cel` CLI's `--descriptor_set`) and the eval binding (via
# `Engine.create({descriptors})`) need a serialized `FileDescriptorSet`
# for these types so proto rows type-check + construct + read fields.
#
# This script resolves the cel-spec proto sources + the protobuf
# well-known-type protos from the bazel external tree and runs `protoc
# --descriptor_set_out --include_imports`, producing a self-contained FDS
# committed at conformance/fixtures/cel_conformance_protos.fds (build
# output, like the eval golden .wasm fixtures).  Run this only when the
# cel-spec proto pin changes.
#
# Usage: bindings/ts/scripts/build-conformance-fds.sh
set -euo pipefail

REPO_ROOT="$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)"
OUT_DIR="$REPO_ROOT/bindings/ts/conformance/fixtures"
OUT="$OUT_DIR/cel_conformance_protos.fds"

command -v protoc >/dev/null || { echo "error: protoc is required" >&2; exit 1; }

# The cel-spec protos + the protobuf WKT protos live in the bazel external
# tree; resolve their roots from `bazel info output_base` so the paths
# track the module pins rather than a hard-coded cache path.
OB="$(cd "$REPO_ROOT" && bazel info output_base 2>/dev/null)"
SPEC="$OB/external/cel-spec~/proto"
PB="$OB/external/protobuf~/src"

[ -d "$SPEC" ] || { echo "error: cel-spec protos not found at $SPEC (run a bazel build first)" >&2; exit 1; }
[ -d "$PB" ] || { echo "error: protobuf protos not found at $PB (run a bazel build first)" >&2; exit 1; }

mkdir -p "$OUT_DIR"

protoc \
  --descriptor_set_out="$OUT" \
  --include_imports \
  -I"$SPEC" -I"$PB" \
  cel/expr/conformance/proto2/test_all_types.proto \
  cel/expr/conformance/proto2/test_all_types_extensions.proto \
  cel/expr/conformance/proto3/test_all_types.proto

echo "wrote $(wc -c < "$OUT" | tr -d ' ') bytes to $OUT"

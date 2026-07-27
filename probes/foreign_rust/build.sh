#!/usr/bin/env bash
# Build + run the manual-Rust-plugin probe end-to-end.
#
# Proves the m35 plugin contract is language-agnostic in practice: a
# component authored in Rust against the cel-generated fns.wit, with
# decls embedded by a standalone `cel embed-decls` run, loads and
# evaluates through the identical Plugin::Load -> Use -> Plan -> Eval
# flow — no repo codegen, no C++ authoring.
#
# Requires: rustup with the wasm32-wasip2 target (rust >= 1.82), bazel.
set -euo pipefail
cd "$(dirname "$0")"
REPO_ROOT="$(git rev-parse --show-toplevel)"

echo "== 1. regenerate the WIT contract from the idl =="
( cd "$REPO_ROOT" && bazel build //tools/cel:cel )
GEN_DIR="$(mktemp -d)"
"$REPO_ROOT/bazel-bin/tools/cel/cel" generate \
    --idl=addcase/rustadd.idl --language=cpp --out_dir="$GEN_DIR"
if ! diff -u addcase/wit/fns.wit "$GEN_DIR/fns.wit"; then
  echo "checked-in wit drifted from cel generate output — refresh it" >&2
  exit 1
fi

echo "== 2. build the Rust component (cargo, wasm32-wasip2) =="
( cd addcase && cargo build --release --target wasm32-wasip2 )
CORE=addcase/target/wasm32-wasip2/release/rustadd_plugin.wasm
ls -la "$CORE"

echo "== 3. embed the declarations (standalone embed-decls run) =="
"$REPO_ROOT/bazel-bin/tools/cel/cel" embed-decls \
    --plugin "$CORE" --idl addcase/rustadd.idl \
    --out addcase/rustadd_plugin_embedded.wasm

echo "== 4. run the one-noun flow against the Rust artifact =="
( cd "$REPO_ROOT" && bazel build //probes/foreign_rust:probe_runner )
"$REPO_ROOT/bazel-bin/probes/foreign_rust/probe_runner" \
    addcase/rustadd_plugin_embedded.wasm

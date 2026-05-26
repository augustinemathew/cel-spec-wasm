#!/usr/bin/env bash
# Build + run the foreign-Go ABI probes (string-case + proto-via-serialization).
# Throwaway research per doc/implementation-plan/rewrite/foreign-go-bindgen-findings.md.
#
# Requires: go >=1.24 (for //go:wasmexport + GOOS=wasip1 c-shared), protoc,
# protoc-gen-go on PATH, python3 with `wasmtime` (pip install wasmtime).
set -euo pipefail
cd "$(dirname "$0")"

# protoc-gen-go was installed to /tmp/gobin in the probe session; adjust if needed.
export PATH="${PROTOC_GEN_GO_BIN:-/tmp/gobin}:$PATH"

echo "== regenerate Go protobuf bindings =="
( cd protocase && protoc --go_out=userpb --go_opt=paths=source_relative user.proto )

echo "== build string-case module (stock Go wasip1) =="
( cd strcase && GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o rules.wasm . )
ls -la strcase/rules.wasm

echo "== build proto-case module (stock Go wasip1 -- links protobuf runtime) =="
( cd protocase && go mod tidy >/dev/null 2>&1 || true; \
  GOOS=wasip1 GOARCH=wasm go build -buildmode=c-shared -o rules.wasm . )
ls -la protocase/rules.wasm

# Optional TinyGo builds (string-case works; proto-case TRAPS at runtime --
# TinyGo reflection is incomplete, breaks proto.Unmarshal). Uncomment to repro:
# ( cd strcase   && tinygo build -target=wasip1 -buildmode=c-shared -o rules_tinygo.wasm . )
# ( cd protocase && tinygo build -target=wasip1 -buildmode=c-shared -o rules_tinygo.wasm . )

echo "== run host harnesses =="
python3 host_strcase.py   strcase/rules.wasm
python3 host_protocase.py protocase/rules.wasm

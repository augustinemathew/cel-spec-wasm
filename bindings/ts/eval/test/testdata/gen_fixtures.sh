#!/usr/bin/env bash
# Regenerate the e2e Program fixtures (compiled CEL .wasm) used by
# backings.e2e.test.ts. Each is `cel compile <expr>` against the committed
# customer.fds.bin descriptor set — so the TS host runs the SAME bytes the
# C++ pipeline emits. Re-run after a compiler change that affects codegen:
#
#   ts/eval/test/testdata/gen_fixtures.sh
#
# Requires the cel CLI built: bazel build //compiler_v2/tools/cel:cel
#
# Also writes fixtures.json — a manifest mapping each .wasm back to the CEL
# SOURCE expression it was compiled from. The e2e reads it so each test's
# title is the real source expression (not an opaque filename), and a guard
# test fails if a fixture is missing from the manifest. So "which expression
# is this .wasm?" always has an answer, traceable to this script.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
CEL="${CEL_BIN:-$REPO/bazel-bin/compiler_v2/tools/cel/cel}"
FDS="$HERE/customer.fds.bin"
MSG3_FDS="$HERE/host_msg3.fds.bin"
U="u:celwasm.testdata.Customer"
M="m:celwasm.testdata.HostMsg3"

MANIFEST=() # accumulates JSON "<file>": "<expr>" entries

record() { # <out> <expr> — append a JSON-escaped manifest entry
  local out="$1" expr="$2"
  local esc=${expr//\\/\\\\}; esc=${esc//\"/\\\"} # \ then " for JSON
  MANIFEST+=("  \"$out\": \"$esc\"")
  echo "  $out  <-  $expr"
}

emit() { # <out> <expr> [extra --var ...]
  local out="$1"; shift
  local expr="$1"; shift
  "$CEL" compile "$expr" --descriptor_set "$FDS" "$@" \
    --output "$HERE/$out" >/dev/null
  record "$out" "$expr"
}

emit3() { # <out> <expr>  — HostMsg3 (enum + repeated element types)
  local out="$1"; shift
  local expr="$1"; shift
  "$CEL" compile "$expr" --descriptor_set "$MSG3_FDS" --var "$M" \
    --output "$HERE/$out" >/dev/null
  record "$out" "$expr"
}

write_manifest() {
  local i last=$(( ${#MANIFEST[@]} - 1 ))
  {
    echo "{"
    for i in "${!MANIFEST[@]}"; do
      if (( i < last )); then printf '%s,\n' "${MANIFEST[$i]}"; else
        printf '%s\n' "${MANIFEST[$i]}"; fi
    done
    echo "}"
  } > "$HERE/fixtures.json"
}

echo "Customer scalar field reads:"
emit scalar_string.wasm 'u.name'           --var "$U"
emit scalar_int.wasm    'u.age'            --var "$U"
emit scalar_int64.wasm  'u.user_id'        --var "$U"
emit scalar_uint.wasm   'u.priority'       --var "$U"
emit scalar_uint64.wasm 'u.balance_cents'  --var "$U"
emit scalar_double.wasm 'u.credit_score'   --var "$U"
emit scalar_bool.wasm   'u.is_premium'     --var "$U"
emit scalar_bytes.wasm  'u.session_token'  --var "$U"

echo "Presence + nested message:"
emit has_field.wasm     'has(u.name)'           --var "$U"
emit has_absent.wasm    'has(u.billing_address)' --var "$U"
emit nested_msg.wasm    'u.billing_address.city' --var "$U"

echo "Proto repeated + map field access:"
emit repeated_index.wasm 'u.tags[0]'        --var "$U"
emit map_string.wasm     'u.metadata["k"]'  --var "$U"
emit map_int.wasm        'u.tier_quotas[1]' --var "$U"

echo "Message CONSTRUCTION (cel_make_message + cel_set_field):"
emit mk_message.wasm 'celwasm.testdata.Customer{name: "Ann", age: 7, is_premium: true}'

echo "Proto-typed policy expression (compiled against the Customer schema;"
echo "evaluable with EITHER a real proto message OR a plain JS object):"
emit policy.wasm 'u.is_premium && u.age > 18' --var "$U"

echo "Aggregates as return values:"
emit msg_return.wasm  'u'           --var "$U"
emit list_return.wasm 'u.tags'      --var "$U"
emit map_return.wasm  'u.metadata'  --var "$U"

echo "Caller-built (non-proto) list/map variables:"
emit listidx.wasm    'xs[1]'   --var 'xs:list<int>'
emit maplookup.wasm  'm["k"]'  --var 'm:map<string, int>'

echo "field.wasm (legacy name kept for the message-read test):"
emit field.wasm 'u.name' --var "$U"

echo "Scalar identity (bind a top-level scalar var; expr returns it) —"
echo "covers every scalar as BOTH an input binding and a return type:"
emit id_int.wasm    'x' --var 'x:int'
emit id_uint.wasm   'x' --var 'x:uint'
emit id_double.wasm 'x' --var 'x:double'
emit id_bool.wasm   'x' --var 'x:bool'
emit id_string.wasm 'x' --var 'x:string'
emit id_bytes.wasm  'x' --var 'x:bytes'

echo "Arena-backed string / bytes results (computed in wasm → arena span):"
emit str_concat.wasm   'a + b' --var 'a:string' --var 'b:string'
emit bytes_concat.wasm 'a + b' --var 'a:bytes'  --var 'b:bytes'

echo "Literal / error return kinds:"
emit lit_null.wasm 'null'
emit err_div0.wasm '1 / 0'   # → CEL_ERROR(divide_by_zero) at eval time

echo "Arena-backed list / map RESULTS (built in wasm; CEL_LIST_ARENA/MAP_ARENA):"
emit arena_list_int.wasm  '[1, 2, 3]'
emit arena_list_str.wasm  '["a", "b"]'
emit arena_list_bool.wasm '[true, false]'
emit arena_list_null.wasm '[null, null]'
emit arena_map_ii.wasm    '{1: 10, 2: 20}'
emit arena_map_si.wasm    '{"a": 1}'
emit arena_list_msg.wasm  '[u, u]'    --var "$U"   # list of proto messages
emit arena_map_smsg.wasm  '{"x": u}'  --var "$U"   # map<string, message>

echo "HostMsg3 — enum + proto repeated element types (int/bool/double/message):"
emit3 enum_return.wasm    'm.kind'           # enum → CEL int
emit3 rep_int_index.wasm  'm.rep_i32[0]'     # repeated int32  element
emit3 rep_bool_index.wasm 'm.rep_b[0]'       # repeated bool   element
emit3 rep_dbl_index.wasm  'm.rep_f64[0]'     # repeated double element
emit3 rep_int_return.wasm 'm.rep_i32'        # repeated int32  → CEL list
emit3 rep_msg_return.wasm 'm.rep_msg'        # repeated message → CEL list-of-msg
emit3 rep_msg_field.wasm  'm.rep_msg[0].i32' # field of a list-of-message element

echo "Benchmark fixture (30 real runtime additions; -O3, variable operands"
echo "so binaryen can't constant-fold them away):"
ADDS_EXPR="$(python3 -c "print(' + '.join(f'v{i}' for i in range(31)))")"
ADDS_VARS=()
for i in $(seq 0 30); do ADDS_VARS+=(--var "v$i:int"); done
"$CEL" compile "$ADDS_EXPR" "${ADDS_VARS[@]}" --O 3 \
  --output "$HERE/adds30.wasm" >/dev/null
record adds30.wasm "$ADDS_EXPR"

write_manifest
echo "done. wrote fixtures.json ($(( ${#MANIFEST[@]} )) entries)."

# e2e — full-pipeline test suites

Every target here drives the real pipeline: `Compiler::Compile` →
`Engine::Plan` → `Instance::Eval` under wasmtime.  Files are named by
**language surface**, not by the milestone that authored them — the
file name answers "where is X tested end-to-end?" without a decoder
ring.  (The milestone-named files were renamed 2026-07-26; see the
mapping at the bottom.  `git log --follow` traverses the rename.)

Most targets build twice via `link_mode_e2e_cc_test` —
`<name>_dynamic` / `<name>_static` — covering both `LinkMode`s.
Manual-tagged targets (`host_fn*`) do NOT run under a bare
`bazel test`; see
`doc/implementation-plan/per-component-test-coverage.md`.

## Suite taxonomy

The classification below is duplicated machine-readably in
`e2e/test_taxonomy.json` (consumed by `scripts/coverage/` to join
per-suite coverage data with surface cells; a validation test keeps
the two in sync with the BUILD file).

### Language surface

| Suite | Surface |
|---|---|
| `ident_select_test` | idents, proto field select, `has()`, activation binding |
| `operators_test` | built-in operators: arithmetic, comparison, string/bytes ops, `size`/`in`, polymorphic equality, 3VL; full-pipeline smoke |
| `list_test` | list literals, indexing, repeated proto fields |
| `map_index_test` | map lookup / `in` / equality via the runtime SwissTable index (indexed ≥8 and linear <8 shapes) |
| `comprehension_test` | `all/exists/exists_one/map/filter`, two-var forms, `transformList/Map/MapEntry`, `cel.bind` |
| `conversion_test` | scalar conversions `bool()/int()/uint()/double()/string()/bytes()` |
| `type_value_test` | `type(x)`, type idents, type equality |
| `time_test` | timestamp/duration constructors, arithmetic, ordering, accessors, conversions |
| `proto_literal_test` | proto message literal construction (`kStructExpr`) |
| `any_test` | `google.protobuf.Any` pack / unpack / equality |
| `wrapper_test` | the 9 wrapper WKTs, auto-wrap/unwrap |
| `wkt_field_set_test` | WKT-valued proto fields (`Value`, `Struct`, Any, null-pruning) |
| `proto_from_host_test` | proto construction from host-origin (bound) maps/lists; message-backed collection reads |
| `optional_test` | optionals: `.?`, `[?]`, `optional.*`, `or/orValue`, `optMap/optFlatMap` |
| `partial_eval_test` | unknowns / partial-eval propagation matrix |
| `string_ext_test` | `strings` extension |
| `math_ext_test` | `math` extension |
| `encoders_ext_test` | `encoders` (base64) extension |
| `network_ext_test` | `network` (IP/CIDR) extension |

### Pipeline & host surface

| Suite | Surface |
|---|---|
| `limits_test` | every fixed compilation limit: just-inside pass + just-past loud rejection |
| `static_link_test` | `LinkMode::kStatic` plumbing, dynamic-vs-static value parity |
| `static_init_test` | static initializers (`__wasm_call_ctors`) forcing functions: cctz + double formatting under both link modes |
| `static_aggregate_test` | compile-time materialized const lists/maps (rodata) |
| `optimize_test` | Binaryen `optimize_level` 0-vs-2 value parity |
| `program_roundtrip_test` | `Compile → bytes → Program → Plan → Eval` persistence |
| `activation_boundary_test` | activation marshal across buffer/arena/memory capacity boundaries |
| `slot_aliasing_test` | slot-reuse discipline stress |
| `arena_message_aggregate_eq_test` | arena+arena aggregate equality with messages; poisoned-source propagation |
| `proto2_extension_list_eq_test` | proto2 repeated-extension list equality |
| `proto_arena_lazy_copy_test` | lazy proto field read / arena copy at the wire transition |
| `host_fn_test`, `host_fn_type_matrix_test` | `@host` custom functions + exhaustive type matrix (manual) |
| `known_bugs_test` | confirmed-defect registry: CELBUG pins + fixed-bug regressions |
| `fuzz/` | PBT generator + cel-cpp differential oracle |

## Historical names

Renamed 2026-07-26 (audit
`doc/implementation-plan/rewrite/reviews/2026-07-27-test-inventory-and-coverage.md`
§6).  Old milestone docs cite the old names; this table is the bridge:

| Old | New |
|---|---|
| `mvp_concat_test.cc` | folded into `operators_test.cc` (FullPipelineSmoke) |
| `m2_test.cc` | `ident_select_test.cc` |
| `m2_partial_eval_test.cc` | `partial_eval_test.cc` |
| `m4_test.cc` | `list_test.cc` |
| `m5_test.cc` | `operators_test.cc` |
| `m5b_test.cc` | `comprehension_test.cc` |
| `m7_test.cc` | `proto_literal_test.cc` |
| `m7a_test.cc` | `any_test.cc` |
| `m7b_test.cc` | `time_test.cc` |
| `m8_test.cc` | `wrapper_test.cc` |
| `m9_test.cc` | `type_value_test.cc` |
| `m10_test.cc` | `conversion_test.cc` |
| `m12_test.cc` | `string_ext_test.cc` |
| `m14_test.cc` | `optional_test.cc` |
| `m16_test.cc` | `math_ext_test.cc` |
| `m17_test.cc` | `encoders_ext_test.cc` |
| `m18_test.cc` | `network_ext_test.cc` |
| `m28_static_link_test.cc` | `static_link_test.cc` |
| `m31_static_aggregate_test.cc` | `static_aggregate_test.cc` |
| `m32_swisstable_index_test.cc` | `map_index_test.cc` |
| `cctz_doubles_test.cc` | `static_init_test.cc` |

Going forward: e2e file names name the language surface; milestone
docs cite file names, never the other way round.

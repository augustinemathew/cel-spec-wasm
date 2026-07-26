# Current capabilities — verified ground truth

**Status:** Phase-1 reconciliation artifact, derived 2026-06-09 **from code,
not docs.** This is the empirical baseline for (a) rewriting the HLD and
(b) verifying each milestone's "shipped" claim. Every row cites the
source-of-truth file. Where a doc claims a capability this baseline can't
confirm, the doc is wrong until proven otherwise.

> Method: capabilities are read out of the codegen dispatch arms, the
> runtime `CelKind` enum, the host trampoline table, the compiler public
> API, and the conformance gate — never from a milestone doc's status line.

---

## 1. Conformance (the hard number)

- **total = 2454, pass = 1899 (77.4%), skip = 463 (18.9%), fail = 92 (3.7%)**
  — `conformance/README.md`.
- Addressable-subset pass rate ≈ 91% (the static-subset gate excludes
  `dyn`, proto2 extensions, enum-decay by design).
- Monotonic gate baselines: dynamic `.baseline` = 1899, static
  `.baseline_static` = 1899 — `scripts/check_conformance_monotonic.sh`.

## 2. AST kinds codegen actually lowers

Source of truth: the `EmitK*` dispatch arms in
`compiler/codegen/expr_lower.cc` + `expr_lower_comprehension.cc`.

| AST kind | Emitter | Notes |
|---|---|---|
| Const literal | `EmitKConstLoad` | all scalar literals |
| Ident | `EmitKIdentLoad` | activation-bound variables |
| Select | `EmitKSelect` | proto field read + `has()` (test_only) |
| Select (optional) | `EmitKSelectOptionalBranch` | `?.` optional chaining |
| Index | `EmitKIndexCall` | `list[i]`, `map[k]` |
| List literal | `EmitKListExpr` | |
| Map literal | `EmitKMapExpr` | |
| Struct/message literal | `EmitKStructExpr` | proto construction |
| Call | `EmitGeneralCall` + `EmitConditional` | incl. ternary, cross-numeric re-pick |
| Comprehension | `expr_lower_comprehension.cc` | list + map sources, 1/2-iter |

## 3. CEL value kinds the runtime represents

Source of truth: `CelKind` enum in `runtime/cel_data.h`.

- **Scalars:** bool, int, uint, double, string, bytes, null.
- **Aggregates:** list (`CEL_LIST` / `CEL_LIST_ARENA` / `CEL_LIST_HOST`),
  map (`CEL_MAP` / `CEL_MAP_ARENA` / `CEL_MAP_HOST`) — three origins each
  (literal / arena-built / host-supplied).
- **Well-known:** timestamp, duration, message (proto), type.
- **Optional:** `CEL_OPTIONAL` (m14).
- **Network ext types:** `CEL_IP`, `CEL_CIDR` (m18) — confirms network
  ext kernels are real, not just declared.
- **Poison/sentinel:** `CEL_UNKNOWN` (partial-eval), `CEL_ERROR` with a
  typed error code set (divide-by-zero, modulus-by-zero, overflow,
  no-such-key, duplicate-key, field-not-found, index-out-of-bounds,
  type-mismatch/unsupported, invalid-argument, host-adapter-error).

## 4. Runtime extension kernels present

Source of truth: `runtime/cel_*.c`.

| Kernel | File | Maps to milestone (to verify in Phase 2) |
|---|---|---|
| 3-valued logic | `cel_3vl.c` | m4 |
| Arithmetic + overflow | `cel_arith.c` | m1 |
| Comparison | `cel_compare.c` | m1 |
| Conversions | `cel_convert.c` | m10 |
| Math ext | `cel_math_ext.c` | m16 |
| Network ext | `cel_net_ext.c` | m18 |
| Optional | `cel_optional.c` | m14 |
| String ops | `cel_string_ops.c` | m12 |
| Time (duration/timestamp) | `cel_time.c` | m7b |
| Type subsystem | `cel_type.c` | m9 |
| Message make / field | `cel_make.c` | m7 |
| Arena / memory | `cel_arena.c`, `cel_memory.c` | m-mem (static layout) |
| Host log | `cel_log.c` | host surface |

> **Encoders ext (m17) has no dedicated `cel_*.c` kernel** — base64 may
> live in `cel_convert.c` or `cel_string_ops.c`, or m17's claim is
> partial. **Flag for Phase-2 verification.**

## 5. Host / foreign function surface

Source of truth: trampoline registrations in
`eval/internal/cel_host_wasmtime.cc`, API in `eval/engine.h`.

- **Field access:** `cel_get_field`, `cel_set_field`, `cel_has_field`.
- **List ops:** `cel_list_at/concat/eq/in/iter_open/size`.
- **Map ops:** `cel_map_lookup/eq/in/iter_open/size`.
- **Message:** `cel_make_message`, `cel_message_eq`.
- **Time:** `cel_time_parse`, `cel_timestamp_tz_accessor`,
  `cel_wkt_unwrap_time`, `cel_wkt_unwrap_wrapper`.
- **Custom/foreign fns:** `HostCall` + `BindTyped*` (m21 host-call
  adapter); `@host` documented in `compiler.h` / `engine.h`.
  `@plugin` / `AddPlugin` (m24 foreign-fn via Component Model,
  renamed from `@component` / `AddComponent` by the m35 R sweep) are
  on HEAD — `eval/engine.h::AddPlugin`, plus the m35 one-noun
  surface: `Plugin::Load` (`abi/plugin.h`), `Engine::Use`
  (`eval/engine.h`), `Compiler::Builder::Use` (`compiler/compiler.h`).

## 6. Compile-time options

- **Link modes:** `kStatic` (default — self-contained ~800KB module) and
  `kDynamic` (~10KB, runtime linked separately) — `compiler.h::LinkMode`
  (m28).

---

## Known code-vs-doc contradictions to resolve in Phase 2

1. **`static_memory_builder.cc:142,151`** still `ABSL_CHECK(false)` with
   *"AllocateList is a stub until M5"* / *"AllocateMap is a stub until
   M6"* — yet list/map literals + m5 comprehensions are marked shipped.
   Either these static-layout arms are dead (aggregates flow through a
   different builder) or there's unfinished work behind a "shipped"
   label. **Resolve.**
2. **`activation.cc:29,37`** — `BindLazy` (stub until M2) and
   `OverrideFunction` (stub until M5) still `ABSL_CHECK(false)`. M2 and
   M5 are marked shipped. Confirm these are intentionally-deferred
   sub-features vs. drift.
3. **m22-foreign-fn** status line says *"not yet started"* but
   foreign-fn / Component-Model work shipped on a sibling branch —
   the ledger must record the true cross-branch state.
4. **Encoders ext (m17)** — no dedicated kernel file; confirm base64
   surface is real and where it lives.

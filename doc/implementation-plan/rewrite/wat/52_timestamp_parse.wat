;; CEL source:  timestamp("2009-02-13T23:31:30Z")
;; Decl:        — (no free variables)
;;
;; M7B.D slice — `timestamp(string)` constructor lowers to a single
;; host trampoline call.  Per `m7b-duration-timestamp.md` §4.3
;; (Option C), constructors / formatting go through the host
;; (genuinely need RFC3339 parsing — a ~250 LoC state machine that
;; absl already implements correctly).  Arithmetic + UTC accessors
;; stay in pure wasm (`wat/43_timestamp_accessor.wat`).
;;
;; Memory layout:
;;   [ 0, 16)  reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)  rodata: kConst "2009-02-13T23:31:30Z"
;;                     {kind=CEL_STRING(5), span.ptr=40, span.len=20}
;;   [40, 60)  rodata: string body "2009-02-13T23:31:30Z" (20 bytes)
;;   [64, 88)  workspace: out_slot for the constructed CEL_TIMESTAMP
;;   [88+]  bump arena (malloc'd in heap)
;;
;; New import this slice:
;;   cel_host.cel_timestamp_parse(out_slot, str_slot) — i32×2 → ()
;;
;; cel_timestamp_parse contract (Layer-2 `CelTimestampParseImpl`):
;;   - reads `str_slot` as CEL_STRING.  Any other kind → out_slot =
;;     {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - 3VL absorption — CEL_UNKNOWN/CEL_ERROR in str_slot pass
;;     through unchanged.
;;   - happy path: parses via `absl::ParseTime(absl::RFC3339_full,
;;     ...)`, post-validates the [0001-01-01Z, 9999-12-31T23:59:59Z]
;;     bound (probe B finding: absl admits year>9999, lowercase z,
;;     leap-second `23:59:60`, two-digit year — Layer-2 must reject
;;     those after absl).  On reject: out_slot =
;;     {CEL_ERROR, CEL_ERR_INVALID_ARG}.
;;   - on admit: extracts (seconds, nanos) via IDivDuration ladder,
;;     writes out_slot = {CEL_TIMESTAMP(13), payload.ts =
;;     CelDurTs{seconds, nanos, _pad=0}}.
;;
;; Codegen shape (emitted by `expr_lower.cc::EmitGeneralCall` once
;; `string_to_timestamp` / `timestamp_to_timestamp` overload ids
;; graduate from `kExplicitlyUnimplementedIds`):
;;
;;   1. The argument string lowers normally (rodata frame at [16, 40)
;;      with the body at [40, 60)).
;;   2. EmitGeneralCall looks up `string_to_timestamp` in the
;;      OverloadTable, finds `cel_host.cel_timestamp_parse` with the
;;      slot-out ABI shape `(out_slot, str_slot)` (i32×2 → void),
;;      and emits the call below.
;;
;; This file follows the same shape as `08_map_index_host.wat` —
;; pure host import, no runtime cel_* dispatch hop.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_timestamp_parse"
          (func $cel_timestamp_parse (param i32 i32)))

  ;; rodata @ 16: kind=CEL_STRING(5), _pad, span.ptr=40, span.len=20, pad8.
  (data (i32.const 16)
        "\05\00\00\00" "\00\00\00\00"
        "\28\00\00\00" "\14\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata @ 40: the 20-byte body.
  (data (i32.const 40) "2009-02-13T23:31:30Z")

  (func $eval (result i32)
    ;; Arena begins at 88 (16-byte aligned past rodata + out_slot).
    (call $arena_reset)

    ;; Host trampoline: parse the rodata string, write CEL_TIMESTAMP
    ;; into slot 64.
    (call $cel_timestamp_parse
          (i32.const 64)        ;; out_slot
          (i32.const 16))       ;; str_slot

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

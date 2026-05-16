;; CEL source:  string(duration("3600s"))
;; Decl:        — (no free variables)
;;
;; M7B.D slice — `string(duration)` conversion lowers to a single host
;; trampoline call.  Per `m7b-duration-timestamp.md` §4.3 (Option C),
;; constructors / formatting go through the host because they
;; genuinely need a library: duration text format follows the proto
;; Duration canonical form (`<n>s` for whole seconds, `<n>.NNNs` for
;; fractional), and cel-cpp's reference impl delegates to
;; `internal::EncodeDurationToJson`.  Arithmetic + UTC accessors stay
;; in pure wasm (`wat/50_duration_arithmetic.wat`,
;; `wat/51_timestamp_year_utc.wat`).
;;
;; Memory layout:
;;   [ 0,  16)  reserved + arena cursor/limit
;;   [16,  40)  rodata: kConst duration("3600s") →
;;                  CelValue{kind=CEL_DURATION(12), _pad,
;;                           payload.dur={seconds=3600, nanos=0, _pad=0}}
;;   [40,  64)  workspace: out_slot for the constructed CEL_STRING
;;                          (Layer-2 impl cel_alloc's the bytes into the
;;                          arena and writes the span here).
;;   [64, mem_size)  bump arena (the formatted "3600s" bytes land here)
;;
;; New import this slice:
;;   cel_host.cel_duration_format(out_slot, dur_slot) — i32×2 → ()
;;
;; cel_duration_format contract (Layer-2 `CelDurationFormatImpl`):
;;   - reads `dur_slot` as CEL_DURATION.  Any other kind →
;;     out_slot = {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - 3VL absorption — CEL_UNKNOWN/CEL_ERROR pass through.
;;   - happy path: formats the {seconds, nanos} pair using the proto
;;     Duration canonical text format (cel-cpp parity), cel_allocs the
;;     result bytes inside the arena via `cel_alloc(len)`, writes
;;     out_slot = {CEL_STRING(5), payload.s={ptr=arena_off, len}}.
;;   - Trailing zero suppression: "3600s" (whole seconds), "0.001s"
;;     (millis), "0.000000001s" (nanos).  The proto canonical form
;;     drops trailing zeros in the fractional part — see the
;;     §3.1 format-output column in m7b-duration-timestamp.md.
;;
;; The companion overloads (cel_host.cel_timestamp_format,
;; cel_host.cel_timestamp_parse, cel_host.cel_duration_parse) follow
;; the same 2-arg shape with body-specific Layer-2 dispatch.  Spec-
;; parity admit/reject pinned by `m7b-duration-timestamp.md` §6.2.
;;
;; Codegen shape (emitted by `expr_lower.cc::EmitGeneralCall` once
;; `duration_to_string` graduates from `kExplicitlyUnimplementedIds`):
;;
;;   1. The argument duration lowers normally (rodata frame at [16, 40)).
;;   2. EmitGeneralCall looks up `duration_to_string` in the
;;      OverloadTable, finds `cel_host.cel_duration_format` with the
;;      slot-out ABI shape `(out_slot, dur_slot)` (i32×2 → void), and
;;      emits the call below.
;;
;; This file follows the same shape as `52_timestamp_parse.wat` —
;; pure host import, no runtime cel_* dispatch hop.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel_host" "cel_duration_format"
          (func $cel_duration_format (param i32 i32)))

  ;; rodata @ 16: CEL_DURATION(12) duration("3600s") = {seconds=3600,
  ;; nanos=0}.  3600 = 0x0E10  → little-endian int64.
  (data (i32.const 16)
        "\0c\00\00\00" "\00\00\00\00"
        "\10\0e\00\00\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  (func $eval (result i32)
    ;; Arena starts at 64 (past out_slot, 16-byte aligned).
    (call $cel_reset (i32.const 64) (i32.const 131072))

    ;; Host trampoline: format the rodata duration, write CEL_STRING
    ;; into slot 40 with the bytes cel_alloc'd in the arena.
    (call $cel_duration_format
          (i32.const 40)        ;; out_slot
          (i32.const 16))       ;; dur_slot

    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

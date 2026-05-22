;; CEL source:  timestamp("2009-02-13T23:31:30Z").getFullYear("America/Los_Angeles")
;; Decl:        — (no free variables)
;;
;; M7B.E slice — with-TZ accessor lowers to a single host trampoline
;; call with a dispatch kind.  Per `m7b-duration-timestamp.md` §4.3
;; (Option C), the 10 with-TZ accessor overloads fold to ONE host
;; import (`cel_host.cel_timestamp_tz_accessor`) parameterised by an
;; `accessor_kind` u32 enum — keeps the cel_host ABI surface count
;; bounded and per-import dispatch O(1).  The IANA tzdata database
;; lives on the host (absl::TimeZone::Load); pure wasm has no way
;; to evaluate "America/Los_Angeles" without bundling tzdata, which
;; would inflate cel_runtime.wasm by orders of magnitude.
;;
;; accessor_kind u32 enum (per plan §4.3 / §5 M7B.E):
;;   0 = kYear              (getFullYear)
;;   1 = kMonth             (getMonth, 0-based)
;;   2 = kDate              (getDate, 1-based day-of-month)
;;   3 = kDayOfMonth        (getDayOfMonth, 0-based)
;;   4 = kDayOfYear         (getDayOfYear, 0-based)
;;   5 = kDayOfWeek         (getDayOfWeek, 0=Sunday)
;;   6 = kHours             (getHours, 0-23)
;;   7 = kMinutes           (getMinutes, 0-59)
;;   8 = kSeconds           (getSeconds, 0-59)
;;   9 = kMilliseconds      (getMilliseconds, 0-999)
;;
;; This file traces accessor_kind=0 (kYear) with an IANA-name tz.
;; The fixed-offset path through the same trampoline is in
;; `55_timestamp_hours_fixed_offset.wat`.
;;
;; Memory layout:
;;   [ 0,  16)  reserved + arena cursor/limit
;;   [16,  40)  rodata: kConst timestamp("2009-02-13T23:31:30Z") →
;;                  CelValue{kind=CEL_TIMESTAMP(13), _pad,
;;                  payload.ts={seconds=1234567890, nanos=0, _pad=0}}
;;   [40,  64)  rodata: kConst CEL_STRING tz wrapper →
;;                  CelValue{kind=CEL_STRING(5), _pad,
;;                  span={ptr=64, len=19}, pad8}
;;   [64,  83)  rodata: string body "America/Los_Angeles" (19 bytes)
;;   [88, 112)  workspace: out_slot for the kCallExpr result
;;                          (16-byte aligned past rodata which ends at 83;
;;                          codegen rounds up via RoundUp8/16 in layout_pass.cc)
;;                          expected: {CEL_INT(2), i = 2009}
;;                          (Probe E baseline; the LA TZ on 2009-02-13
;;                          23:31:30Z = 2009-02-13 15:31:30 PST → year=2009.
;;                          A near-midnight UTC ts that crosses the
;;                          dateline would be year=2008 — the negative
;;                          case lives in the §6.7 with-TZ matrix.)
;;   [112, mem_size)  bump arena
;;
;; New import this slice:
;;   cel_host.cel_timestamp_tz_accessor(out_slot, ts_slot, tz_slot,
;;                                       accessor_kind) — i32×4 → ()
;;
;; cel_timestamp_tz_accessor contract (Layer-2
;; `CelTimestampTzAccessorImpl`):
;;   - reads `ts_slot` as CEL_TIMESTAMP.  Any other kind →
;;     out_slot = {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - reads `tz_slot` as CEL_STRING.  Any other kind →
;;     out_slot = {CEL_ERROR, CEL_ERR_TYPE_MISMATCH}.
;;   - 3VL absorption — CEL_UNKNOWN/CEL_ERROR in either operand
;;     passes through (CEL_UNKNOWN takes precedence per the v1
;;     M4 Slice A 3VL helpers).
;;   - happy path: `absl::TimeZone::Load(tz_str)` succeeds; the
;;     trampoline extracts `absl::CivilSecond` fields per
;;     `accessor_kind` and writes out_slot = {CEL_INT(2), i = field}.
;;     For kind=0 (kYear) on the bound ts seconds=1234567890,
;;     tz="America/Los_Angeles" → year=2009.
;;   - tz load failure (unknown IANA name, malformed fixed-offset)
;;     → out_slot = {CEL_ERROR, CEL_ERR_INVALID_ARG}.
;;   - unknown `accessor_kind` (>= 10) is a codegen invariant
;;     violation, not a runtime input — Layer-2 may CHECK-fail.
;;
;; Codegen shape (emitted by `expr_lower.cc::EmitGeneralCall` once
;; `timestamp_with_tz_to_year` and its 9 siblings graduate from
;; `kExplicitlyUnimplementedIds`):
;;
;;   1. ts argument + tz argument lower normally (both rodata frames).
;;   2. EmitGeneralCall looks up `timestamp_with_tz_to_year` in the
;;      OverloadTable, finds `cel_host.cel_timestamp_tz_accessor` with
;;      the slot-out ABI shape `(out_slot, ts_slot, tz_slot,
;;      accessor_kind)` (i32×4 → void), reads the overload's
;;      `accessor_kind` constant (0 for getFullYear), and emits the
;;      call below.
;;
;; The 9 sibling with-TZ accessors are the same call shape with a
;; different `accessor_kind` constant — see
;; `55_timestamp_hours_fixed_offset.wat` for kind=6 (kHours) with a
;; fixed-offset tz, which exercises a different absl::TimeZone code
;; path on the host.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel_host" "cel_timestamp_tz_accessor"
          (func $cel_timestamp_tz_accessor (param i32 i32 i32 i32)))

  ;; rodata @ 16: CEL_TIMESTAMP(13) {seconds=1234567890, nanos=0}.
  ;; 1234567890 = 0x499602D2  → little-endian int64.
  (data (i32.const 16)
        "\0d\00\00\00" "\00\00\00\00"
        "\d2\02\96\49\00\00\00\00"
        "\00\00\00\00" "\00\00\00\00")

  ;; rodata @ 40: CEL_STRING(5) wrapper, span.ptr=64, span.len=19, pad8.
  (data (i32.const 40)
        "\05\00\00\00" "\00\00\00\00"
        "\40\00\00\00" "\13\00\00\00"
        "\00\00\00\00\00\00\00\00")

  ;; rodata @ 64: tz body (19 bytes).
  (data (i32.const 64) "America/Los_Angeles")

  (func $eval (result i32)
    ;; Arena begins at 112 (16-byte aligned past out_slot @ 88).
    (call $arena_reset)

    ;; Host trampoline: dispatch on accessor_kind=0 (kYear).
    (call $cel_timestamp_tz_accessor
          (i32.const 88)        ;; out_slot
          (i32.const 16)        ;; ts_slot
          (i32.const 40)        ;; tz_slot (CEL_STRING wrapper)
          (i32.const 0))        ;; accessor_kind = kYear

    (i32.const 88))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

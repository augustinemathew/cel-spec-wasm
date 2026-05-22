;; CEL source:  optional.ofNonZeroValue(0)                     → optional(None)
;; Decl:        — (no free variables)
;;
;; M14 Slice 0 — locks the **zero-predicate dispatch matrix** for
;; `cel_optional_of_non_zero_at_v`.  Per probe Q6/Q7 (overload
;; `optional_ofNonZeroValue`) and per cel-cpp
;; `runtime/optional_types.cc:58-67`, this overload is a thin
;; wrapper:
;;
;;   OptionalOfNonZeroValue(v) =
;;     if (v.IsZeroValue()) return OptionalNone();
;;     else return OptionalOf(v);
;;
;; The kernel ABI is the same shape as `cel_optional_of_at_v`
;; — `(out_slot, v_slot) → ()`.  What this WAT locks is NOT the
;; signature (covered by symmetry with `_of_at_v`) but the
;; **per-kind zero predicate**, which IS a closed ABI surface
;; the conformance corpus exercises directly
;; (`tests/simple/testdata/optionals.textproto:1-3`).
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_optional_of_non_zero_at_v(out_slot, v_slot) → ()
;;
;; Contract:
;;   1. Read v = *v_slot.
;;   2. Compute is_zero per the matrix below.
;;   3. If is_zero: tail-call cel_optional_none_at(out_slot).
;;      Else:       tail-call cel_optional_of_at_v(out_slot, v_slot).
;;
;; Zero-predicate matrix (locked here for Slice A's C impl):
;;
;;   | v.kind            | is_zero condition                              |
;;   |-------------------|------------------------------------------------|
;;   | CEL_NULL          | always true  (any null → None)                 |
;;   | CEL_BOOL          | v.payload.b == 0                               |
;;   | CEL_INT           | v.payload.i == 0                               |
;;   | CEL_UINT          | v.payload.u == 0                               |
;;   | CEL_DOUBLE        | v.payload.d == 0.0  (bit-exact; -0.0 == +0.0)  |
;;   | CEL_STRING        | v.payload.s.len == 0                           |
;;   | CEL_BYTES         | v.payload.bytes.len == 0                       |
;;   | CEL_LIST_ARENA    | arena_list.count == 0                          |
;;   | CEL_LIST_HOST     | cel_host.cel_list_size(...) == 0  (trampoline) |
;;   | CEL_MAP_ARENA     | arena_map.count == 0                           |
;;   | CEL_MAP_HOST      | cel_host.cel_map_size(...) == 0   (trampoline) |
;;   | CEL_MESSAGE       | IsZeroValue (no set fields AND no unknowns,    |
;;   |                   |   per cel-cpp parsed_message_value.cc:78-86;   |
;;   |                   |   needs a new host trampoline to consult       |
;;   |                   |   reflection)                                  |
;;   | CEL_DURATION      | seconds == 0 AND nanos == 0                    |
;;   | CEL_TIMESTAMP     | seconds == 0 AND nanos == 0                    |
;;   | CEL_TYPE          | always false (a type name carries identity)    |
;;   | CEL_OPTIONAL      | outer present == 0, OR inner is zero per       |
;;   |                   |   recursive descent through this matrix        |
;;   | CEL_UNKNOWN       | propagate v unchanged into out_slot            |
;;   | CEL_ERROR         | propagate v unchanged into out_slot            |
;;
;; The CEL_MESSAGE row was incorrectly documented as "not defined;
;; cel-cpp errors" in an earlier draft of m14-optionals.md §3.4;
;; corrected 2026-05-21 by Slice 0 review (independent
;; reviewer P1).
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: kConst `0`  {kind=CEL_INT(2), payload.i=0}
;;   [40, 64)   workspace: kCall(`optional.ofNonZeroValue`) out_slot
;;   [64, mem_size)  bump arena.  Since v=0 is the CEL_INT zero
;;                   value, the kernel takes the None branch and
;;                   tail-calls cel_optional_none_at, which
;;                   arena_alloc(32)s a None cell at [64, 96).
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_optional_of_non_zero_at_v"
          (func $cel_optional_of_non_zero_at_v (param i32 i32)))

  ;; rodata: CelValue{CEL_INT(=2), _pad=0, payload.i=0, union_pad=0}
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\00\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; `optional.ofNonZeroValue(0)`:
    ;;   out_slot = 40, v_slot = 16  (CEL_INT(0))
    ;; Expected: kernel takes the is_zero=true branch → out_slot
    ;; receives CEL_OPTIONAL(None) backed by a fresh cell at 64.
    (call $cel_optional_of_non_zero_at_v (i32.const 40) (i32.const 16))
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

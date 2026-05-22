;; CEL source:  [?optional.of(10), ?optional.none()]            → list<int>([10])
;; Decl:        — (no free variables)
;;
;; Locks the `cel_list_append_at_if_present` kernel ABI for
;; `[?elem]` list-literal entries.  Per probe Q4
;; (ast_shape_probe_test.cc), `[?elem]` populates
;; `CreateList.optional_indices: repeated int32` on the
;; `kListExpr` — the codegen emits a regular eval-into-slot for the
;; element followed by the `_if_present` kernel call (instead of the
;; unconditional `cel_list_append_at`).
;;
;; The kernel mirrors the existing `cel_list_append_at_if_bool`
;; (cel_runtime.c:345) but the predicate is an `optional<T>` payload
;; rather than a `CelValue{CEL_BOOL}`:
;;
;;   Some(v) ⇒ unwrap inner and append the inner CelValue to the list
;;   None    ⇒ silent no-op
;;   ERROR / UNKNOWN on opt_slot ⇒ propagate verbatim into list_slot
;;     (aborts the comprehension/literal per langdef 3VL)
;;   opt_slot kind != CEL_OPTIONAL ⇒ poison list_slot with
;;     CEL_ERR_TYPE_MISMATCH
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_list_append_at_if_present(list_slot, opt_value_slot)
;;
;;   1. l = cel_value_at(list_slot)
;;   2. If l.kind != CEL_LIST_ARENA: return (consistent with
;;      cel_list_append_at_if_bool — list already poisoned or wrong
;;      kind, nothing to do).
;;   3. opt = cel_value_at(opt_value_slot)
;;   4. If opt.kind == CEL_ERROR || opt.kind == CEL_UNKNOWN:
;;        *list_slot = *opt_slot ;; propagate
;;        return
;;   5. If opt.kind != CEL_OPTIONAL:
;;        poison list_slot with CEL_ERR_TYPE_MISMATCH; return
;;   6. cell = (OptionalCell*)(memory_base + opt.payload.opt)
;;   7. If !cell.present: return ;; silent no-op
;;   8. Otherwise reuse cell.inner as the value slot — write inner
;;      into a scratch slot, then `cel_list_append_at(list_slot,
;;      scratch_slot)`.  ABI-equivalent to:
;;
;;        CelValue tmp = cell.inner;
;;        cel_list_append_at(list_slot, &tmp);
;;
;; ── Memory layout ─────────────────────────────────────────────
;;
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: CelValue{CEL_INT, i=10} — inner value
;;   [40, 64)   workspace: list — CEL_LIST_ARENA result
;;   [64, 88)   workspace: optional.of(10) — Some<int>
;;   [88, 112)  workspace: optional.none() — None
;;   [112, mem_size)  bump arena.  By the end of $eval:
;;                    list header (16 B) + capacity=2 elements
;;                    run (2 × 24 = 48 B) + 2 OptionalCells (2 × 32 B).
;;                    count == 1 — only the Some element appended.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_optional_of_at_v"
          (func $cel_optional_of_at_v (param i32 i32)))
  (import "cel" "cel_optional_none_at"
          (func $cel_optional_none_at (param i32)))
  (import "cel" "cel_list_append_at_if_present"
          (func $cel_list_append_at_if_present (param i32 i32)))

  ;; rodata: CelValue{CEL_INT, i=10} at offset 16.
  (data (i32.const 16)
        "\02\00\00\00"
        "\00\00\00\00"
        "\0a\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)
    ;; step 1: empty arena list at workspace slot 40, capacity 2.
    (call $cel_list_create (i32.const 40) (i32.const 2))
    ;; step 2: optional.of(10) at slot 64.
    (call $cel_optional_of_at_v (i32.const 64) (i32.const 16))
    ;; step 3: optional.none() at slot 88.
    (call $cel_optional_none_at (i32.const 88))
    ;; step 4: append Some(10) — element appended, count=1.
    (call $cel_list_append_at_if_present
          (i32.const 40)   ;; list_slot
          (i32.const 64))  ;; opt_value_slot — Some
    ;; step 5: append None — silent no-op, count stays 1.
    (call $cel_list_append_at_if_present
          (i32.const 40)   ;; list_slot
          (i32.const 88))  ;; opt_value_slot — None
    (i32.const 40))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

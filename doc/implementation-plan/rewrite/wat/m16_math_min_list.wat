;; CEL source:  math.least([3, 1, 2])
;; Decl:        — (no free variables)
;;
;; Slice 0 WAT-first trace for the variadic-min kernel.  The
;; `math.least` / `math.greatest` parser macros (cel-cpp
;; `math_ext_macros.cc`) collapse a list literal — and any 3+ scalar
;; arg list — into a single `kListExpr` arg, so the only runtime
;; surface for the list form is:
;;
;;   cel.cel_math_min_list_at_v(out_slot, list_slot)  — i32, i32 → ()
;;
;; (the `_at_v` suffix = 2 args: out + one value).  `cel_math_max_list`
;; is byte-identical in shape; only the fold direction differs.
;;
;; ABI this trace FREEZES:
;;   - Input is ONE CelValue of kind CEL_LIST_ARENA (=7) at `list_slot`,
;;     whose payload.arena_list.header_ptr points at an ArenaListHeader
;;     { count, capacity, elements_offset } (16 B); elements are a
;;     contiguous count*24 B run of CelValues at `elements_offset`.
;;   - Output is ONE CelValue written at `out_slot` = the min element.
;;   - The kernel reads each element's `kind` (offset 0) at runtime and
;;     folds with the cross-type numeric compare ladder
;;     (cel_numeric_lt semantics), so a MIXED int/uint/double list
;;     yields a dyn-typed result whose runtime kind is the winning
;;     element's kind.  (This trace uses a same-kind int list for a
;;     deterministic, layout-clear baseline; the cross-type fold is
;;     covered by the kernel's unit-test matrix, not the ABI shape.)
;;   - Empty list cannot reach here: the macro rejects empty list
;;     literals at parse time (IsListLiteralWithValidArgs).
;;
;; Expected decoded result: {CEL_INT, i = 1}.
;;
;; NOTE: cel_math_min_list_at_v does not yet exist in cel_runtime.wasm.
;; This trace ASSEMBLES (wasm-as) to freeze the import signature +
;; memory shape; it is registered in wat_runner_test and run
;; end-to-end only once the kernel lands (M16 Slice C).
;;
;; Memory layout:
;;   [ 0,  8)  null sentinel
;;   [ 8, 16)  legacy arena cursor/limit slots (arena now in runtime BSS)
;;   [16, 40)  rodata: element 0   {CEL_INT, i=3}
;;   [40, 64)  rodata: element 1   {CEL_INT, i=1}
;;   [64, 88)  rodata: element 2   {CEL_INT, i=2}
;;   [88,112)  workspace: list CelValue (out of cel_list_create; =88)
;;   [112,136) workspace: min result slot (out=112)
;;   [136+]  bump arena (heap) — cel_list_create reserves the
;;                    ArenaListHeader (16 B) + count*24 B elements run.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_append_at" (func $cel_list_append_at (param i32 i32)))
  (import "cel" "cel_math_min_list_at_v"
          (func $cel_math_min_list_at_v (param i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $arena_reset)

    ;; Build the arena list [3, 1, 2] at slot 88.
    (call $cel_list_create (i32.const 88) (i32.const 3))
    (call $cel_list_append_at (i32.const 88) (i32.const 16))
    (call $cel_list_append_at (i32.const 88) (i32.const 40))
    (call $cel_list_append_at (i32.const 88) (i32.const 64))

    ;; Fold to the minimum element → slot 112.
    (call $cel_math_min_list_at_v (i32.const 112) (i32.const 88))

    (i32.const 112))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

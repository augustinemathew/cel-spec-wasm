;; CEL source:  cel.bind(x, 5, x + 1)         → 6
;; Decl:        — (no free variables; x is bind-introduced)
;;
;; M5 slice (Slice I — `cel.bind` parser-library registration +
;; Shape-C fast path).  Locks the *degenerate* lowering for
;; `cel.bind` — the macro expands to a kComprehensionExpr with
;; `iter_range = []` and `loop_cond = false`, so generic Shape A
;; would correctly produce the right answer (the loop body never
;; runs).  But the loop prologue still costs ~5 wasm ops per
;; bind, which adds up on bind-heavy programs.
;;
;; cel-cpp macro → kComprehensionExpr (bindings_ext.cc):
;;   iter_range  = []                           (empty kCreateList)
;;   iter_var    = #unused                      (sentinel; never read)
;;   accu_var    = x                            (the bound name)
;;   accu_init   = 5                            (the bound value)
;;   loop_cond   = false                        (loop never enters)
;;   loop_step   = x                            (would just re-bind)
;;   result      = x + 1                        (the body expression)
;;
;; ── Key design claim ─────────────────────────────────────────
;;
;; Codegen detects this shape at the entry of `LowerComprehension`:
;;
;;   if (iter_range is empty-list-literal AND
;;       loop_cond is `false` AND
;;       iter_var is the cel-cpp sentinel "#unused"):
;;     emit Shape C: push_scope(accu_var → slot); eval(accu_init);
;;                   eval(result); pop_scope()
;;
;; The result is no `block`/`loop`, no `br_if`s, no per-iter
;; pointer bump — just the eval of `accu_init` followed by the
;; eval of `result`, with `accu_var` bound to `accu_init`'s slot
;; for the body.  Per cel-cpp benchmarks this is ~30% faster on
;; bind-heavy programs vs. the generic Shape A.
;;
;; **Correctness**: Shape A would emit the same answer (the loop
;; never runs, accu retains its init value, `result` evaluates
;; with x = 5).  Shape C is a pure perf optimisation; Shape A is
;; a strict superset.  The gate is at codegen entry, no runtime
;; surface change.  Cite m5-comprehensions-design.md §5 (Shape C).
;;
;; ── No new runtime helpers needed ───────────────────────────
;;
;; Shape C only needs existing helpers (cel_int_add_at_vv for
;; the body's `x + 1`).  This is the easiest WAT in M5 — it
;; could ship today if cel.bind parser registration existed.
;; **Runnable today** (every import resolves against the live
;; runtime; we just have to hand-lower as if the parser had
;; registered cel.bind).
;;
;; Memory layout (Shape C — no comprehension framing):
;;   [ 0, 16)   reserved + arena cursor/limit
;;   [16, 40)   rodata: accu_init = {CEL_INT, i=5}
;;   [40, 64)   rodata: rhs `1` for `x + 1`
;;   [64, 88)   workspace: x_slot ← memcpy from accu_init at 16
;;                          (bound name's storage; the body's kIdent
;;                          arm for `x` lowers to (i32.const 64))
;;   [88,112)   workspace: body result slot (kCall(`_+_`) out)
;;   [112, mem_size)  bump arena (untouched — no list/map alloc)
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_int_add_at_vv"
          (func $cel_int_add_at_vv (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\05\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (call $cel_reset (i32.const 112) (i32.const 131072))

    ;; Shape C step 1: evaluate accu_init (`5`) into the bound
    ;; name's slot.  For a kConst init this is a 24-byte memcpy
    ;; from rodata at 16 → workspace at 64.  A more complex
    ;; accu_init would dispatch through its own kCall arm.
    (i32.store offset=0  (i32.const 64) (i32.load offset=0  (i32.const 16)))
    (i32.store offset=4  (i32.const 64) (i32.load offset=4  (i32.const 16)))
    (i32.store offset=8  (i32.const 64) (i32.load offset=8  (i32.const 16)))
    (i32.store offset=12 (i32.const 64) (i32.load offset=12 (i32.const 16)))
    (i32.store offset=16 (i32.const 64) (i32.load offset=16 (i32.const 16)))
    (i32.store offset=20 (i32.const 64) (i32.load offset=20 (i32.const 16)))

    ;; Shape C step 2: evaluate result (`x + 1`) with x bound to
    ;; the x_slot at 64.  No comprehension loop emitted.
    ;;   kCall(`_+_`, x, kConst(1))
    ;;     → cel_int_add_at_vv(out=88, a=x_slot=64, b=40)
    (call $cel_int_add_at_vv
          (i32.const 88) (i32.const 64) (i32.const 40))

    (i32.const 88))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

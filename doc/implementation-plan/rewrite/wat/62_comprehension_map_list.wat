;; CEL source:  [1, 2, 3].map(v, v * 2)        → [2, 4, 6]
;; Decl:        — (no free variables)
;;
;; M5 slice (Slice D — `filter` / `map` + dynamic-list primitive).
;; Locks the shape codegen emits for `map(v, t)`: a list accumulator
;; that starts empty and grows by one element per iter.
;;
;; cel-cpp macro → kComprehensionExpr (parser/macro.cc::MakeMap):
;;   iter_range  = [1, 2, 3]                    (kCreateList)
;;   iter_var    = v
;;   accu_var    = @result
;;   accu_init   = []                           (empty list literal)
;;   loop_cond   = true                         (no early exit)
;;   loop_step   = @result + [v * 2]         (concat single elem)
;;   result      = @result
;;
;; ── Key design claim ─────────────────────────────────────────
;;
;; cel-cpp's macro emits `accu + [t]` for loop_step.  A naive
;; lowering would compile this as `cel_list_concat(accu, accu,
;; [t])` — allocate a new combined list, copy all existing
;; elements, plus the one new one.  Total work over N iters: O(N²).
;;
;; Slice D adds `cel_list_append_at(list_slot, value_slot)` with
;; geometric (2×) growth → amortised O(N).  Codegen pattern-matches
;; the IR shape `kCall(_+_, accu_ref, kCreateList([single_elem]))`
;; and emits `cel_list_append_at(accu_slot, elem_slot)` directly.
;; See m5-comprehensions-design.md §7.1.
;;
;; **Append-vs-concat is a codegen-side decision, not a runtime
;; one.**  Programs that wrote `accu + [a, b]` literally (a
;; multi-element appendage) fall back to `cel_list_concat`.
;; That's an unusual shape — the macro expansion always produces
;; single-element [t] — but it must remain correct.
;;
;; ── DEPENDS ON Slice D ──────────────────────────────────────
;;
;; The runtime export `cel_list_append_at` does NOT exist yet.
;; This WAT will FAIL to instantiate via `wat_runner` until
;; Slice D ships:
;;   - cel_list.h / cel_list.c add the function body
;;   - `kRuntimeExports` in wat_runner.cc lists `cel_list_append_at`
;;   - BUILD.bazel tag for this WAT moves from `manual` → default
;;
;; Until then: `wasm-as` validates the WAT shape; `wat_runner_test`
;; skips this fixture (tag = manual).
;;
;; Memory layout:
;;   [ 0, 16)   reserved (null sentinel; arena state lives in runtime BSS)
;;   [16, 40)   rodata: list elem [0] = {CEL_INT, i=1}
;;   [40, 64)   rodata: list elem [1] = {CEL_INT, i=2}
;;   [64, 88)   rodata: list elem [2] = {CEL_INT, i=3}
;;   [88,112)   rodata: scalar `2` for `v * 2` rhs
;;   [112,136)  workspace: iter_range list slot (kCreateList result)
;;   [136,160)  workspace: accu_slot (= the dynamic [] → [2,4,6])
;;   [160,184)  workspace: step_out (per-iter `v * 2` result)
;;   [184+]  bump arena (malloc'd in heap) — initial accu list header (16 B)
;;                    + geometric element runs allocate from here.
;;
;; ── Runtime helpers ─────────────────────────────────────────
;;   cel.cel_list_create       (M4.F)         — for iter_range build
;;   cel.cel_list_append_at          (M4.F)         — for iter_range build
;;   cel.cel_int_mul_at_vv     (M5.B)         — `v * 2`
;;   cel.cel_list_append_at    (Slice D NEW)  — geometric-growth append
;;
;; Note on accu_init: a "fresh empty list" is one of two shapes:
;;   (a) `cel_list_create(accu_slot, 0)` — allocates a header with
;;       count=0, capacity=0, elements_offset=0.  Append-from-empty
;;       grows the elements run on the first call.
;;   (b) `cel_list_create(accu_slot, <pre_alloc>)` — pre-size to
;;       `list_size(iter_range)` for the unconditional-map case.
;;       Optimal but specialises codegen per-form.  Rejected in
;;       m5-comprehensions-followon.md §3.6 (too much complexity
;;       for the 24 KB best-case savings; `filter` would over-
;;       allocate anyway).
;; This WAT uses shape (a) — the generic path.
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_append_at" (func $cel_list_append_at (param i32 i32)))
  (import "cel" "cel_int_mul_at_vv"
          (func $cel_int_mul_at_vv (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; rhs `2` for `v * 2`.
  (data (i32.const 88)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $list_hdr i32)
    (local $iter_off i32)
    (local $end_off  i32)

    (call $arena_reset)

    ;; iter_range = [1, 2, 3] at slot 112.
    (call $cel_list_create (i32.const 112) (i32.const 3))
    (call $cel_list_append_at (i32.const 112) (i32.const 16))
    (call $cel_list_append_at (i32.const 112) (i32.const 40))
    (call $cel_list_append_at (i32.const 112) (i32.const 64))

    ;; accu_init = [] at slot 136.  cel_list_create with count=0
    ;; allocates a bare ArenaListHeader; subsequent
    ;; cel_list_append_at grows the element run geometrically.
    (call $cel_list_create (i32.const 136) (i32.const 0))

    ;; Set up iter pointer over iter_range.
    (local.set $list_hdr (i32.load offset=8 (i32.const 112)))
    (local.set $iter_off (i32.load offset=8 (local.get $list_hdr)))
    (local.set $end_off  (i32.add (local.get $iter_off) (i32.const 72)))

    (block $exit
      (loop $continue
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))

        ;; loop_step:
        ;;   step_out = v * 2
        (call $cel_int_mul_at_vv
              (i32.const 160)
              (local.get $iter_off)     ;; v IS iter_off
              (i32.const 88))
        ;;   accu = accu + [step_out]   → cel_list_append_at peephole
        (call $cel_list_append_at
              (i32.const 136)            ;; accu_slot
              (i32.const 160))           ;; step_out

        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (br $continue)))

    ;; result = @result
    (i32.const 136))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

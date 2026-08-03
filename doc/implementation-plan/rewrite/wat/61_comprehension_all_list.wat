;; CEL source:  [1, 2, 3].all(v, v > 0)
;; Decl:        — (no free variables)
;;
;; M5 slice (Slices A–C closeout) — the `all` macro shape, sibling
;; to `60_comprehension_exists_list.wat`.  Both expand to the same
;; kComprehensionExpr skeleton; only `accu_init` and `loop_step` /
;; `loop_cond` differ.  Reading 60 and 61 side-by-side is the
;; clearest demonstration that the codegen arm is *generic* — the
;; only macro-specific bits live in the (already-lowered) accu_init
;; constant and the loop_step kCall expression.
;;
;; cel-cpp macro → kComprehensionExpr (parser/macro.cc::MakeAll):
;;   iter_range  = [1, 2, 3]                    (kCreateList)
;;   iter_var    = v
;;   accu_var    = @result
;;   accu_init   = true                         (vs. `false` in exists)
;;   loop_cond   = @not_strictly_false(@result)
;;                 (vs. `@not_strictly_false(!@result)` in exists)
;;   loop_step   = @result && (v > 0)        (vs. `||` in exists)
;;   result      = @result
;;
;; ── Difference matrix exists ↔ all ──────────────────────────
;;
;;   exists                            all
;;   accu_init = false                 accu_init = true
;;   loop_cond peephole:               loop_cond peephole:
;;     accu is bool true  → exit         accu is bool false → exit
;;     (kind==BOOL && b!=0)              (kind==BOOL && b==0)
;;   loop_step uses cel_or             loop_step uses cel_and
;;
;; The peephole inversion is load-bearing: per langdef
;; §"Comprehensions" the wrapped form is
;; `@not_strictly_false(@result)`, which "stays true unless the
;; result is *strictly false* — i.e. CEL_BOOL with payload.b == 0".
;; ERROR / UNKNOWN in accu DO NOT cause loop exit (3VL has no
;; short-circuit for those kinds in `all`; the loop runs to
;; completion, propagating whichever wins).
;;
;;   - For `exists`: payload.b != 0 means we found a true element,
;;     so we can exit early.  ERROR/UNKNOWN accu means we must
;;     keep iterating (a later true would still flip us to true,
;;     not propagate the error).
;;   - For `all`:    payload.b == 0 means we found a false element,
;;     so we can exit early.  ERROR/UNKNOWN means same — keep
;;     iterating.
;;
;; This is why the peephole MUST read the kind word, not just the
;; payload: ERROR and UNKNOWN park `payload.err` / `payload.unk` at
;; the same offset 8, so a payload-only test mistakes a poisoned accu
;; for a bool and exits early.  A static bool TYPE does not rule the
;; poison out — `loop_step = bool && bool` type-checks as bool and
;; still evaluates to an error at runtime, which is exactly how
;; `[0, 2].exists(x, 2/x == 1)` returned divide_by_zero instead of
;; true.  Cite m5-comprehensions-design.md §9.4 for the
;; error-short-circuit rule.
;;
;; ── Key design claims (inherited from 60, restated for clarity) ──
;;
;;   1. **Uniform kIdent load** — `v` IS `iter_off`.
;;   2. **No per-iter memcpy of v** — moving pointer, no copy.
;;   3. **Same-slot aliasing into cel_and** — `cel_and(accu, accu,
;;      step_out)` works identically to `cel_or` in 60: cel_3vl.h's
;;      same-slot contract applies to both.
;;
;; Memory layout (mirrors 60 exactly; differs only in accu_init bytes):
;;   [ 0,  8)   null sentinel (arena cursor now in runtime BSS)
;;   [ 8, 16)   pad (legacy arena-limit slot — now in BSS)
;;   [16, 40)   rodata: list elem [0] = {CEL_INT, i=1}
;;   [40, 64)   rodata: list elem [1] = {CEL_INT, i=2}
;;   [64, 88)   rodata: list elem [2] = {CEL_INT, i=3}
;;   [88,112)   rodata: accu_init = {CEL_BOOL, b=true}    ← TRUE here
;;   [112,136)  rodata: rhs of `v > 0` = {CEL_INT, i=0}
;;   [136,160)  workspace: kCreateList result slot
;;   [160,184)  workspace: accu_slot
;;   [184,208)  workspace: step_out scratch
;;   [208+]  bump arena (malloc'd in heap)
;;
;; ── Runtime helpers — all already exported ──
;;   cel.cel_and    (M5.G — 3VL conjunction; same-slot aliasing OK)
;;   …rest as 60.
;;
;; **Runnable today.**
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_append_at" (func $cel_list_append_at (param i32 i32)))
  (import "cel" "cel_int_gt_at_vv"
          (func $cel_int_gt_at_vv (param i32 i32 i32)))
  (import "cel" "cel_and" (func $cel_and (param i32 i32 i32)))

  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; accu_init = {CEL_BOOL, true} at [88, 112) — payload.b at offset 8 = 1.
  (data (i32.const 88)
        "\01\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 112)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $list_hdr i32)
    (local $iter_off i32)
    (local $end_off  i32)

    (call $arena_reset)

    ;; iter_range = [1, 2, 3]
    (call $cel_list_create (i32.const 136) (i32.const 3))
    (call $cel_list_append_at (i32.const 136) (i32.const 16))
    (call $cel_list_append_at (i32.const 136) (i32.const 40))
    (call $cel_list_append_at (i32.const 136) (i32.const 64))

    ;; accu_slot at 160 ← rodata true at 88.
    (i32.store offset=0  (i32.const 160) (i32.load offset=0  (i32.const 88)))
    (i32.store offset=4  (i32.const 160) (i32.load offset=4  (i32.const 88)))
    (i32.store offset=8  (i32.const 160) (i32.load offset=8  (i32.const 88)))
    (i32.store offset=12 (i32.const 160) (i32.load offset=12 (i32.const 88)))
    (i32.store offset=16 (i32.const 160) (i32.load offset=16 (i32.const 88)))
    (i32.store offset=20 (i32.const 160) (i32.load offset=20 (i32.const 88)))

    (local.set $list_hdr (i32.load offset=8 (i32.const 136)))
    (local.set $iter_off (i32.load offset=8 (local.get $list_hdr)))
    (local.set $end_off  (i32.add (local.get $iter_off) (i32.const 72)))

    (block $exit
      (loop $continue
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))

        ;; Exit when accu IS the bool false — strictly false ⇒ `all`
        ;; short-circuit.  Kind-gated exactly like 60; differs only in
        ;; testing the payload for zero instead of non-zero.
        (br_if $exit
               (i32.and
                 (i32.eq (i32.load offset=0 (i32.const 160)) (i32.const 1))
                 (i32.eqz (i32.load offset=8 (i32.const 160)))))

        ;; loop_step: @result && (v > 0)
        (call $cel_int_gt_at_vv
              (i32.const 184)
              (local.get $iter_off)       ;; v IS iter_off
              (i32.const 112))
        (call $cel_and
              (i32.const 160)             ;; accu_slot (out)
              (i32.const 160)             ;; accu_slot (a)  — alias OK
              (i32.const 184))            ;; step_out  (b)

        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (br $continue)))

    (i32.const 160))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

;; CEL source:  [1, 2, 3].exists(v, v > 0)
;; Decl:        — (no free variables)
;;
;; M5 slice (Slices A–C closeout) — milestone-final WAT for the
;; canonical `exists` comprehension over a list literal.  Supersedes
;; the M5-prototype `05_comprehension_exists.wat` (drafted before
;; this milestone existed; kept on disk as a historical predecessor
;; — its `Key design claim` header is the authoritative articulation
;; of the "kIdent uniform load" doctrine, which still holds here).
;; This file locks the shape codegen is committed to emit: real
;; runtime exports, real memory layout, byte-accurate offsets that
;; `expr_lower.cc::LowerComprehension` reproduces.
;;
;; cel-cpp macro → kComprehensionExpr (parser/macro.cc::MakeExists):
;;   iter_range  = [1, 2, 3]                    (kCreateList)
;;   iter_var    = v
;;   accu_var    = @result
;;   accu_init   = false
;;   loop_cond   = @not_strictly_false(!@result)  ;; exit when accu true
;;   loop_step   = @result || (v > 0)
;;   result      = @result
;;
;; ── Key design claims locked here ───────────────────────────
;;
;;   1. **Uniform `kIdent` load.**  Inside `loop_step` the reference
;;      `v` lowers to `(local.get $iter_off)` — bit-for-bit what
;;      we emit for a free variable resolved to a workspace slot.
;;      The only thing comprehension-specific is the *set* site
;;      (loop header bumps `$iter_off` by 24 per iter); the read
;;      site is the existing kIdent arm verbatim.  Predecessor 05
;;      coined this rule; 60 keeps it.
;;
;;   2. **No per-iter memcpy of `v`.**  `v` IS `iter_off` — a
;;      moving pointer into the list payload.  An alternative
;;      lowering would copy each element into a fixed workspace
;;      slot, but that's 24 B of memmove per iter for no win.
;;      LayoutPass binds `iter_var → kIterPointerLocal` per
;;      m5-comprehensions-followon.md §3.4 specifically to enable
;;      this.
;;
;;   3. **Loop-cond peephole: read accu's bool payload directly.**
;;      cel-cpp's macro lowering wraps `!@result` in
;;      `@not_strictly_false`, which compiles into ~6 wasm ops.
;;      For `exists`, the equivalent test is "accu's bool payload
;;      != 0" — one `i32.load offset=8`.  Codegen emits the
;;      peephole; the general path remains correct for forms
;;      where loop_cond isn't recognisable.
;;
;;   4. **Same-slot aliasing into `cel_or`.**  Per cel_3vl.h
;;      contract: `cel_or(out, a, b)` never reads `a`/`b` after
;;      writing `out`, so `cel_or(accu_slot, accu_slot, step_out)`
;;      is well-defined and saves a slot.  Locked here so codegen
;;      doesn't allocate a separate "accu_next" workspace.
;;
;; Memory layout (computed by LayoutPass):
;;   [ 0,  8)   null sentinel + arena cursor (written by cel_reset)
;;   [ 8, 16)   arena limit / pad
;;   [16, 40)   rodata: list elem [0] = {CEL_INT, i=1}
;;   [40, 64)   rodata: list elem [1] = {CEL_INT, i=2}
;;   [64, 88)   rodata: list elem [2] = {CEL_INT, i=3}
;;   [88,112)   rodata: accu_init = {CEL_BOOL, b=false}
;;   [112,136)  rodata: rhs of `v > 0` = {CEL_INT, i=0}
;;   [136,160)  workspace: kCreateList result slot (list header pointer)
;;   [160,184)  workspace: accu_slot (initialised from rodata at 88)
;;   [184,208)  workspace: step_out scratch (per-iter `v > 0` result)
;;   [208, mem_size)  bump arena — cel_list_create allocates the
;;                    ArenaListHeader (16 B) + 3 × 24 B element run
;;                    here; total 88 B at offset 208.
;;
;; ── Runtime helpers this WAT calls — all already exported ──
;;   cel.cel_reset            (M1)
;;   cel.cel_alloc            (M1)
;;   cel.cel_list_create      (M4.F) — header + count×24 zero-fill
;;   cel.cel_list_set         (M4.F) — write element[i] from slot
;;   cel.cel_int_gt_at_vv     (M5.B) — bool `v > 0`
;;   cel.cel_or               (M5.G) — non-strict 3VL disjunction
;;
;; **Runnable today** — every import resolves against
;; `cel_runtime.wasm`; this WAT is the design lock AND the
;; regression test for codegen's emitted shape.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  (import "cel" "cel_list_create" (func $cel_list_create (param i32 i32)))
  (import "cel" "cel_list_set" (func $cel_list_set (param i32 i32 i32)))
  (import "cel" "cel_int_gt_at_vv"
          (func $cel_int_gt_at_vv (param i32 i32 i32)))
  (import "cel" "cel_or" (func $cel_or (param i32 i32 i32)))

  ;; Three CEL_INT elements at [16, 88).
  (data (i32.const 16)
        "\02\00\00\00" "\00\00\00\00"
        "\01\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\02\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  (data (i32.const 64)
        "\02\00\00\00" "\00\00\00\00"
        "\03\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; accu_init = {CEL_BOOL(1), false} at [88, 112).
  (data (i32.const 88)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; rhs of `v > 0` = {CEL_INT, i=0} at [112, 136).
  (data (i32.const 112)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    ;; LOCAL CATALOGUE ────────────────────────────────────────
    (local $list_hdr   i32)  ;; ArenaListHeader pointer (arena offset)
    (local $iter_off   i32)  ;; moving pointer into the element run;
                             ;; serves as $v_off (uniform kIdent load)
    (local $end_off    i32)  ;; one-past-end of the element run

    ;; ── RESET ─ arena begins past workspace (= 208).
    (call $cel_reset (i32.const 208) (i32.const 131072))

    ;; ── EVALUATE iter_range — kCreateList[1, 2, 3] ─────────
    ;; Build the list at workspace slot 136.  Allocates the
    ;; ArenaListHeader at the current arena cursor (208) and
    ;; a 3 × 24 B element run immediately after it (224).
    (call $cel_list_create (i32.const 136) (i32.const 3))
    (call $cel_list_set (i32.const 136) (i32.const 0) (i32.const 16))
    (call $cel_list_set (i32.const 136) (i32.const 1) (i32.const 40))
    (call $cel_list_set (i32.const 136) (i32.const 2) (i32.const 64))

    ;; ── EVALUATE accu_init — kConst false ───────────────────
    ;; accu_slot at 160 ← rodata false at 88 (24-byte memcpy).
    (i32.store offset=0  (i32.const 160) (i32.load offset=0  (i32.const 88)))
    (i32.store offset=4  (i32.const 160) (i32.load offset=4  (i32.const 88)))
    (i32.store offset=8  (i32.const 160) (i32.load offset=8  (i32.const 88)))
    (i32.store offset=12 (i32.const 160) (i32.load offset=12 (i32.const 88)))
    (i32.store offset=16 (i32.const 160) (i32.load offset=16 (i32.const 88)))
    (i32.store offset=20 (i32.const 160) (i32.load offset=20 (i32.const 88)))

    ;; ── COMPREHENSION SETUP ─────────────────────────────────
    ;; Read list payload.arena_list.header_ptr (offset 8 of CelValue
    ;; at workspace slot 136).
    (local.set $list_hdr (i32.load offset=8 (i32.const 136)))
    ;; iter_off = ArenaListHeader.elements_offset (offset 8 in header).
    (local.set $iter_off
               (i32.load offset=8 (local.get $list_hdr)))
    ;; end_off = iter_off + count(=3) * 24.
    (local.set $end_off
               (i32.add (local.get $iter_off) (i32.const 72)))

    ;; ── LOOP ────────────────────────────────────────────────
    (block $exit
      (loop $continue
        ;; Exit #1: iter past end.
        (br_if $exit
               (i32.ge_u (local.get $iter_off) (local.get $end_off)))

        ;; Exit #2: loop_cond peephole — accu's bool payload != 0
        ;; means `exists` has already found a true element.  Read
        ;; payload.b at offset 8 of accu_slot (160).
        (br_if $exit (i32.load offset=8 (i32.const 160)))

        ;; loop_step: @result || (v > 0)
        ;; (v > 0)  → cel_int_gt_at_vv(step_out=184, v=iter_off, rhs=112)
        (call $cel_int_gt_at_vv
              (i32.const 184)
              (local.get $iter_off)    ;; v IS iter_off — kIdent uniform
              (i32.const 112))
        ;; accu = accu || step_out — same-slot aliasing OK per
        ;; cel_3vl.h same-slot contract.
        (call $cel_or
              (i32.const 160)          ;; accu_slot (out)
              (i32.const 160)          ;; accu_slot (a)
              (i32.const 184))         ;; step_out  (b)

        ;; Advance iter by sizeof(CelValue) = 24.
        (local.set $iter_off
                   (i32.add (local.get $iter_off) (i32.const 24)))
        (br $continue)))

    ;; result = @result — return accu_slot offset.
    (i32.const 160))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

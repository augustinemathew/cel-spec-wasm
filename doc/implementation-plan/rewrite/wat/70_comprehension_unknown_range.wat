;; CEL source:  xs.exists(e, e > 0)   — with xs's slot UNKNOWN
;; Decl:        xs: list<int>  (free variable, kLocal source)
;;
;; Range-absorption prologue for comprehensions (cleanup-backlog
;; #14).  cel-cpp evaluates the iter_range FIRST and, when it is an
;; ERROR or UNKNOWN value, the comprehension result IS that value —
;; no accu_init use, no loop body, no result expression
;; (third_party/cel-cpp/eval/eval/comprehension_step.cc:165-169:
;; `case ValueKind::kError: ABSL_FALLTHROUGH_INTENDED; case
;; ValueKind::kUnknown: result = std::move(range); return ...`).
;;
;; This WAT locks the absorption shape codegen emits at the start of
;; EVERY comprehension prologue (list and map sources alike), BEFORE
;; cel_list_arena_view / cel_map_iter_init / accu pre-sizing run —
;; those all assume an iterable range and would otherwise turn the
;; poison into a zero-count walk (the empty-range-identity soundness
;; gap: exists→false, all→true, map/filter→[]).
;;
;; ── Key design claims locked here ───────────────────────────
;;
;;   1. **Absorption flows through the ACCU slot.**  On a poisoned
;;      range the prologue copies the range CelValue (kind +
;;      payload, so the unknown's attribute id / the error's code
;;      survive) into accu_slot and branches past the whole
;;      prologue+loop region.  The result expression still runs:
;;      `@result`-shaped results (exists/all/map/filter) read the
;;      poison straight from the accu; `exists_one`'s
;;      `@result == 1` 3VL-absorbs it through `_==_`.  Observably
;;      identical to cel-cpp's `result = std::move(range)` for
;;      every macro shape, and reuses the existing accu-poison
;;      propagation machinery instead of a second result path.
;;
;;   2. **One check, both kinds, both source reprs.**  The guard is
;;      `kind == CEL_UNKNOWN(15) || kind == CEL_ERROR(16)` on the
;;      range value's kind word.  ERROR takes the same branch as
;;      UNKNOWN (each propagates itself; with a single operand
;;      there is no dominance question — consistent with the
;;      ERROR-dominates-UNKNOWN strict-call precedence in
;;      doc/design/03-abi-and-memory.md §8.1).  Map-source
;;      comprehensions emit the identical guard before
;;      cel_map_iter_init.
;;
;;   3. **The guarded region is a named block.**  Prologue + loop
;;      live inside `$comp_absorb_<expr_id>`; the absorption br
;;      targets it, landing immediately before the result
;;      expression.  The accu wasm local is set BEFORE the guard so
;;      `@result` reads work on both paths.
;;
;; Memory layout (mirrors LayoutPass for one list<int> variable):
;;   [ 0, 16)   reserved (null sentinel + legacy pad)
;;   [16, 40)   workspace: xs variable slot — pre-seeded by the data
;;              segment below with {CEL_UNKNOWN, payload.u=7} to
;;              emulate the partial-eval marshal blanking the slot
;;              (eval/instance.cc BareVariableUnknownId); payload 7
;;              stands in for the interned attribute id and must
;;              survive the copy byte-for-byte.
;;   [40, 64)   rodata: rhs of `e > 0` = {CEL_INT, i=0}
;;   [64, 88)   rodata: accu_init = {CEL_BOOL, b=false}
;;   [88,112)   workspace: accu_slot (@result)
;;   [112,136)  workspace: step_out scratch (per-iter `e > 0`)
;;
;; Expected result: the CelValue at the returned offset (88) is
;; {CEL_UNKNOWN, payload.u=7} — NOT {CEL_BOOL, false}.
;;
;; **Runnable today** — every import resolves against
;; `cel_runtime.wasm`; with the data segment seeding CEL_UNKNOWN the
;; loop body never executes (asserted by wat_runner_test via the
;; accu payload), and flipping the seed to a concrete arena list
;; re-enables the normal walk (control covered by 60_*.wat).
(module
  (import "cel" "memory" (memory 2 1024 shared))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "cel_copy_slot" (func $cel_copy_slot (param i32 i32)))
  (import "cel" "cel_list_arena_view"
          (func $cel_list_arena_view (param i32) (result i32)))
  (import "cel" "cel_int_gt_at_vv"
          (func $cel_int_gt_at_vv (param i32 i32 i32)))
  (import "cel" "cel_or" (func $cel_or (param i32 i32 i32)))

  ;; xs slot = {CEL_UNKNOWN(15), payload.u=7} at [16, 40) — the
  ;; marshal's whole-variable-unknown write, baked into the image.
  (data (i32.const 16)
        "\0f\00\00\00" "\00\00\00\00"
        "\07\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; rhs of `e > 0` = {CEL_INT(2), i=0} at [40, 64).
  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")
  ;; accu_init = {CEL_BOOL(1), false} at [64, 88).
  (data (i32.const 64)
        "\01\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $xs_off   i32)  ;; variable prelude: xs slot offset (kLocal)
    (local $accu_off i32)  ;; accu_var local — set on BOTH paths
    (local $src      i32)  ;; arena-shaped source slot (normal path)
    (local $hdr      i32)  ;; ArenaListHeader pointer
    (local $iter     i32)  ;; moving element pointer (= e)
    (local $end      i32)  ;; one-past-end of the element run

    (call $arena_reset)

    ;; ── Variable prelude — xs's slot offset into its local.
    (local.set $xs_off (i32.const 16))

    ;; ── accu local binds BEFORE the guard: `@result` reads must
    ;;    resolve on the absorption path too.
    (local.set $accu_off (i32.const 88))

    (block $comp_absorb
      ;; ── RANGE ABSORPTION GUARD ─────────────────────────────
      ;; kind ∈ {CEL_UNKNOWN(15), CEL_ERROR(16)} → the comprehension
      ;; result IS the range value: copy it into the accu slot and
      ;; skip prologue + loop entirely.
      (if (i32.or
            (i32.eq (i32.load (local.get $xs_off)) (i32.const 15))
            (i32.eq (i32.load (local.get $xs_off)) (i32.const 16)))
        (then
          (call $cel_copy_slot (local.get $accu_off) (local.get $xs_off))
          (br $comp_absorb)))

      ;; ── NORMAL PROLOGUE (unchanged shape; see 60_*.wat) ────
      ;; accu_slot ← accu_init (rodata false).
      (call $cel_copy_slot (local.get $accu_off) (i32.const 64))
      ;; Resolve to an arena-shaped slot (host lists snapshot).
      (local.set $src (call $cel_list_arena_view (local.get $xs_off)))
      (local.set $hdr (i32.load offset=8 (local.get $src)))
      (local.set $iter (i32.load offset=8 (local.get $hdr)))
      (local.set $end
                 (i32.add (local.get $iter)
                          (i32.mul (i32.load (local.get $hdr))
                                   (i32.const 24))))

      ;; ── LOOP ───────────────────────────────────────────────
      (block $exit
        (loop $continue
          (br_if $exit
                 (i32.ge_u (local.get $iter) (local.get $end)))
          ;; loop-cond peephole: accu bool payload != 0 → done.
          (br_if $exit (i32.load offset=8 (local.get $accu_off)))
          ;; loop_step: @result || (e > 0)
          (call $cel_int_gt_at_vv
                (i32.const 112) (local.get $iter) (i32.const 40))
          (call $cel_or
                (local.get $accu_off) (local.get $accu_off)
                (i32.const 112))
          (local.set $iter
                     (i32.add (local.get $iter) (i32.const 24)))
          (br $continue))))

    ;; result = @result — accu slot holds {CEL_UNKNOWN, u=7} here.
    (local.get $accu_off))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

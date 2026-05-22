;; CEL source:  optional.none()                                → optional(None)
;; Decl:        — (no free variables)
;;
;; M14 Slice 0 — locks the **distinct 0-input ABI** for the
;; standalone None constructor.  Per cel-cpp overload table
;; (`third_party/cel-cpp/checker/optional.cc` overload
;; `optional_none`), `optional.none()` is its own overload
;; — NOT a special case of `optional.of()` with a magic argument.
;; Independent reviewer's 2026-05-21 P1 finding: omitting this
;; WAT and arguing it's "covered by symmetry" repeats M2's silent
;; GTEST_SKIP pattern.
;;
;; ── New runtime kernel this slice locks ────────────────────
;;
;;   cel.cel_optional_none_at(out_slot) → ()
;;
;; Signature is the 0-input cousin of `cel_optional_of_at_v`:
;;   `_at_v`  = 1 input   (cel_optional_of_at_v)
;;   `_at_vv` = 2 inputs  (cel_select_optional_field_at_vv)
;;   `_at`    = 0 inputs  (cel_optional_none_at)
;;
;; Contract:
;;   1. arena_alloc(32) — get a fresh OptionalCell at `cell_off`.
;;      OOM ⇒ out_slot = {CEL_ERROR, err=CEL_ERR_OVERFLOW}, return.
;;   2. cell[0..4]  = 0            ;; present=None
;;   3. cell[4..8]  = 0            ;; explicit pad zero
;;   4. cell[8..32] = 0            ;; inner zero-filled — never read
;;                                 ;; when present=0, but pinned for
;;                                 ;; cell-equality memcmp-friendliness
;;   5. out_slot.kind        = CEL_OPTIONAL (=14)
;;   6. out_slot._pad        = 0
;;   7. out_slot.payload.opt = cell_off
;;   8. out_slot remaining payload bytes = 0
;;
;; Future shared-static-None optimisation (see WAT 1's
;; "OptionalCell immutability contract" section): step 1 becomes
;; "return the fixed offset of the runtime's static None cell"
;; instead of arena_alloc'ing.  ABI contract is unchanged — every
;; M14 kernel respects cell-immutability.
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   workspace: out_slot — receives CEL_OPTIONAL(None)
;;   [40, mem_size)  bump arena.  arena_alloc(32) lands the
;;                   OptionalCell at [40, 72):
;;                     [40, 44)  cell.present = 0  (None)
;;                     [44, 48)  cell._pad    = 0
;;                     [48, 72)  cell.inner   = zero (unread)
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_optional_none_at"
          (func $cel_optional_none_at (param i32)))

  (func $eval (result i32)
    (call $arena_reset)
    ;; `optional.none()`:
    ;;   out_slot = 16, no operand slots.
    (call $cel_optional_none_at (i32.const 16))
    (i32.const 16))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

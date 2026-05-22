;; CEL source:  {'k': 1}.?missing.orValue('default')           → string("default")
;; Decl:        — (no free variables)
;;
;; M14 Slice 0 — locks two more kernel ABIs and the end-to-end
;; None-propagation chain:
;;
;;   1. `cel_select_optional_field_at_vv` on a non-optional source
;;      with an absent key → produces `optional.none()` (cell with
;;      `present=0`).  Same kernel as WAT 3 — this WAT just hits
;;      the absent-key branch instead of the found-key branch.
;;
;;   2. `cel_optional_or_value_at_vv` — the orValue kernel.  Per
;;      probe Q7 (overload `optional_orValue_value`).  Receiver-form
;;      kCall like hasValue (WAT 2); ABI is post-flatten:
;;          (out_slot, opt_slot, default_slot) → ()
;;
;; Semantically:
;;   optional.some(v).orValue(default) → v
;;   optional.none().orValue(default)  → default
;;
;; The WAT exercises the second branch (None ⇒ default).
;;
;; ── orValue contract ───────────────────────────────────────
;;
;;   cel.cel_optional_or_value_at_vv(out_slot, opt_slot, default_slot)
;;
;;   1. Read opt = *opt_slot.
;;   2. If opt.kind != CEL_OPTIONAL ⇒
;;        out_slot = {CEL_ERROR, err=CEL_ERR_TYPE_MISMATCH}.
;;      (3VL absorption: UNKNOWN/ERROR operands propagate.)
;;   3. cell = cel_value_at(opt.payload.opt)
;;   4. If cell.present:
;;        *out_slot = cell.inner       ;; 24-byte memcpy of the
;;                                     ;; wrapped CelValue (unwrap)
;;      else:
;;        *out_slot = *default_slot    ;; 24-byte memcpy of default
;;
;; Note the unwrap: orValue returns the bare inner value, NOT
;; another optional.  Per cel-cpp `optional_orValue_value`
;; (third_party/cel-cpp/checker/optional.cc).  Mirrors langdef
;; §"optional types": `T.orValue(T)` produces a `T`.
;;
;; ── Short-circuit codegen requirement (Slice B concern) ─────
;;
;; cel-cpp implements `or` / `orValue` with a jump step (see
;; `third_party/cel-cpp/eval/eval/optional_or_step.cc:60-105`,
;; class `OptionalHasValueJumpStep`).  RHS is NOT evaluated when
;; LHS is Some.
;;
;; This kernel ABI is eager — both operands must be evaluated
;; into slots before the call.  That's correct for the common
;; case where the RHS is a literal or a pure ident
;; (`opt.orValue(0)`, `opt.or(other_opt)` with `other_opt` a
;; bound variable).
;;
;; For RHS that can produce errors or unknowns
;; (`opt.orValue(1/0)`, `opt.orValue(f(x))`), eager evaluation
;; produces a CEL_ERROR / CEL_UNKNOWN that this kernel will
;; discard if `opt` is Some — observationally fine for the
;; result, but it CAN matter for conformance rows that check
;; "no error was reported" or for partial-eval rows that
;; expect an unknown attribute set NOT to absorb a side branch.
;;
;; **Codegen (Slice B) must emit a short-circuit branch when
;; the RHS is impure**: evaluate LHS, peek `cell.present`, branch
;; on present to skip RHS-eval and unwrap (orValue) / forward
;; (or), else fall through to RHS-eval and kernel call.  For
;; pure RHS (kConst, kIdent — annotated by `WasmAnnotations`
;; later), emit the eager kernel call directly.
;;
;; This WAT exercises the eager case with a literal RHS
;; ('default'), where eager and short-circuit are
;; observationally identical.  The short-circuit WAT lands in
;; Slice B alongside the codegen branch.
;;
;; Memory layout:
;;   [ 0, 16)   reserved null + arena scaffolding
;;   [16, 40)   rodata: key 'k' CelValue
;;                {kind=CEL_STRING(5), payload.s={ptr=256, len=1}}
;;   [40, 64)   rodata: value 1 CelValue
;;                {kind=CEL_INT(2), payload.i=1}
;;   [64, 88)   rodata: key 'missing' CelValue
;;                {kind=CEL_STRING(5), payload.s={ptr=257, len=7}}
;;   [88, 112)  rodata: default 'default' CelValue
;;                {kind=CEL_STRING(5), payload.s={ptr=264, len=7}}
;;   [112, 136) workspace: map {'k': 1} — CEL_MAP_ARENA
;;   [136, 160) workspace: .?missing result — optional<int> = None
;;   [160, 184) workspace: .orValue('default') result — string("default")
;;   [184, mem_size)  bump arena (map header + entries run +
;;                    None OptionalCell from .?missing).
;;
;;   String bytes:
;;     [256, 257)  "k"
;;     [257, 264)  "missing"
;;     [264, 271)  "default"
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "arena_reset" (func $arena_reset))
  (import "cel" "arena_alloc" (func $arena_alloc (param i32) (result i32)))
  (import "cel" "cel_map_create" (func $cel_map_create (param i32 i32)))
  (import "cel" "cel_map_insert" (func $cel_map_insert (param i32 i32 i32)))
  (import "cel" "cel_select_optional_field_at_vv"
          (func $cel_select_optional_field_at_vv (param i32 i32 i32)))
  (import "cel" "cel_optional_or_value_at_vv"
          (func $cel_optional_or_value_at_vv (param i32 i32 i32)))

  ;; rodata: key 'k' CelValue.
  ;;   kind=CEL_STRING(5), _pad=0, payload.s={ptr=256(0x100), len=1}
  (data (i32.const 16)
        "\05\00\00\00"
        "\00\00\00\00"
        "\00\01\00\00"
        "\01\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: value 1 CelValue.  CEL_INT, i=1.
  (data (i32.const 40)
        "\02\00\00\00"
        "\00\00\00\00"
        "\01\00\00\00\00\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: key 'missing' CelValue.
  ;;   kind=CEL_STRING(5), _pad=0, payload.s={ptr=257(0x101), len=7}
  (data (i32.const 64)
        "\05\00\00\00"
        "\00\00\00\00"
        "\01\01\00\00"
        "\07\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; rodata: default 'default' CelValue.
  ;;   kind=CEL_STRING(5), _pad=0, payload.s={ptr=264(0x108), len=7}
  (data (i32.const 88)
        "\05\00\00\00"
        "\00\00\00\00"
        "\08\01\00\00"
        "\07\00\00\00"
        "\00\00\00\00\00\00\00\00")
  ;; String bytes at [256, 271):
  ;;   "k" + "missing" + "default"
  (data (i32.const 256) "kmissingdefault")

  (func $eval (result i32)
    (call $arena_reset)
    ;; step 1: build map {'k': 1} at workspace slot 112.
    (call $cel_map_create (i32.const 112) (i32.const 1))
    (call $cel_map_insert
          (i32.const 112)  ;; map slot
          (i32.const 16)   ;; key slot ('k')
          (i32.const 40))  ;; value slot (1)
    ;; step 2: .?missing — `_?._` over the map with key 'missing'.
    ;; The kernel hits the absent-key branch and produces
    ;; optional.none() at workspace slot 136.
    (call $cel_select_optional_field_at_vv
          (i32.const 136)  ;; out_slot
          (i32.const 112)  ;; src_slot — MAP_ARENA (NOT optional)
          (i32.const 64))  ;; key_slot — string 'missing'
    ;; step 3: .orValue('default') — receiver-form kCall on the
    ;; None at slot 136 with default at slot 88.  Post-receiver-
    ;; flatten ABI: (out_slot=160, opt_slot=136, default_slot=88).
    ;; Since the opt is None, the kernel copies *default_slot into
    ;; out_slot — producing the bare CEL_STRING "default".
    (call $cel_optional_or_value_at_vv
          (i32.const 160)  ;; out_slot
          (i32.const 136)  ;; opt_slot — optional.none()
          (i32.const 88))  ;; default_slot — string("default")
    (i32.const 160))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))

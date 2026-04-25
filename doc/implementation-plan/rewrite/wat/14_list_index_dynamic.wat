;; CEL source:  xs[0]    (where xs's origin is unprovable at compile
;;                        time — e.g. ?: between an arena literal and
;;                        a bound list.  At M4 the ?: codegen isn't
;;                        lowered yet, so this trace stands as the
;;                        list-dispatcher's executable spec for when
;;                        M5 lights it up.)
;; Decl:        xs : list<int>     (operand origin treated as kDynamic)
;;
;; M4.C slice — runtime-side kDynamic dispatcher for lists.
;; When ResolvePass cannot prove a single origin (mixed ?: arms,
;; future kCall return, …) the kCallExpr arm emits
;; `call $cel.cel_list_at`, which tail-calls into either the
;; arena fast path or the host trampoline based on the operand's
;; runtime CelKind:
;;
;;   CEL_LIST_ARENA → return_call cel_list_at_arena
;;   CEL_LIST_HOST  → return_call cel_host.cel_list_at
;;   anything else (CEL_UNKNOWN / CEL_ERROR / type mismatch) →
;;                  poison out_slot or absorb 3VL.
;;
;; The musttail discipline is enforced in cel_runtime.c via
;; `__attribute__((musttail))`; the wasm tail-call feature must
;; be on at the engine level (mirrored in
;; api/engine.cc::Engine::Builder::Build).
;;
;; This trace authors the dispatcher CALL SITE; the dispatcher
;; BODY lives in cel_runtime.wasm.  A test pre-writes a
;; CEL_LIST_ARENA-shaped CelValue into xs's slot to exercise
;; the arena arm (or CEL_LIST_HOST to exercise the host arm).
;;
;; Memory layout — same as 13 since the call-site lowering only
;; differs in target name.
(module
  (import "cel" "memory" (memory 2))
  (import "cel" "cel_reset" (func $cel_reset (param i32 i32)))
  (import "cel" "cel_alloc" (func $cel_alloc (param i32) (result i32)))
  ;; The dispatcher itself — no `_arena` / `_host` suffix.
  (import "cel" "cel_list_at" (func $cel_list_at (param i32 i32 i32)))

  (data (i32.const 40)
        "\02\00\00\00" "\00\00\00\00"
        "\00\00\00\00\00\00\00\00" "\00\00\00\00\00\00\00\00")

  (func $eval (result i32)
    (local $xs_off i32)

    (local.set $xs_off (i32.const 16))
    (call $cel_reset (i32.const 88) (i32.const 131072))

    ;; kDynamic arm of `_[_]`: runtime dispatcher decides arena
    ;; vs. host at runtime by reading xs's CelKind tag.
    (call $cel_list_at
          (i32.const 64)         ;; out_slot
          (local.get $xs_off)    ;; list_slot — kind tag drives dispatch
          (i32.const 40))        ;; index_slot

    (i32.const 64))

  (export "eval" (func $eval))
  (export "memory" (memory 0)))
